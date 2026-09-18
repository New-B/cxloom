# LoomPar user API boundary

The application-facing interface is the opaque C API in
`include/cxloom/cxloom_mem.h`. Users call `cl_runtime_create`/`cl_runtime_destroy`
and the pthread-shaped operations:

```c
cl_pthread_create(runtime, &thread, start_routine, argument_bytes, byte_count);
cl_pthread_join(runtime, thread, &retval);
cl_pthread_detach(runtime, thread);
cl_pthread_mutex_lock(&mutex);
cl_pthread_cond_wait(&condition, &mutex);
cl_pthread_cond_timedwait(&condition, &mutex, timeout_ms);
cl_pthread_barrier_init(runtime, &barrier, local_participant_count);
cl_pthread_barrier_wait(&barrier);
cl_pthread_barrier_destroy(&barrier);
cl_mem_read(runtime, gptr, offset, buffer, bytes, timeout_ms);
cl_mem_write(runtime, gptr, offset, buffer, bytes, timeout_ms);
```

`cl_pthread_t` contains only the home-host and local thread ID. The user does not
select an execution host, send a CREATE request, inspect a host-pair queue,
choose a scheduler policy, transfer a GTID, or migrate a thread. Those are
internal LoomPar decisions.

Join accepts an optional `void **retval`. The callback result belongs to its
invocation, not to the registered function. Completion transports a fixed-width
`uint64_t` token; join reads the completed record before reclaiming it. Failed
joins leave the caller's retval untouched. Remote result tokens are opaque
handles, not dereferenceable pointers; put structured results in shared GPtr
objects. Joining without requesting a result still reclaims all invocation state.

Detach removes the obligation to join, but retains an active home record until
completion. A detached thread cannot be joined or detached twice. Running
capacity is released at completion, independently of join/detach. Finalize rejects
active detached invocations. Local fiber stacks are reclaimed only after the
scheduler regains control; remote completion still returns to home for accounting.

Mutexes use a context-owned local gate and, in multi-host runtimes, a shared
LoomMem write lease. Successful lock/unlock perform acquire/release hooks. They
support native callers and fibers; a fiber does not hold a native mutex while
suspended. Unlock by a different execution context is rejected.

Condition variables register waiters before releasing the application mutex.
Signal selects one waiter; broadcast selects all current waiters. Native callers
sleep, while fibers park. Timed waits return `CL_UNAVAILABLE` on timeout after
reacquiring the mutex; lock-reacquisition errors are propagated. Destroy rejects
locally active waiters and locked mutexes. Callers must quiesce every host before
destroying a distributed synchronization object.

Distributed conditions keep waiter/notification bitmaps in a shared object and
poll with acquire boundaries. They support 64 simultaneous waiters per object;
an additional wait returns `CL_UNAVAILABLE` with the mutex still held. Each host
needs a local handle to the **same** object: the creator calls
`cl_pthread_mutex_export` / `cl_pthread_cond_export`, communicates the resulting
GPtrs through application bootstrap, and peers call `cl_pthread_mutex_attach` /
`cl_pthread_cond_attach`. Calling init independently allocates distinct objects.
Attached handles pin the shared allocation and their destroy releases that pin;
destroy all attached handles before destroying the allocating handle. Export and
attach require a multi-host runtime; mismatched object types are rejected.

LoomPar fibers remain on their original execution host and worker after first
execution. Join, barrier, mutex, condition and token waits park the fiber, allowing
queued work to run even when the number of waiting callbacks exceeds the worker
count. The scheduler checks parked notifications/deadlines at up to 1 ms intervals.
Native callers retain blocking semantics. Arbitrary blocking application system
calls and native pthread operations are not automatically converted to fiber waits.

`cl_mem_read` and `cl_mem_write` are the C-level GPtr access path. They perform
LoomMem block-level acquire/release operations on the shared region, including
in single-host configurations. Callers do not need to resolve a local address.

The `arg_bytes` parameter is the C API's explicit serialization boundary. It
copies a bounded argument record into the launch message; it never transmits a
process-local pointer. For remote work, shared objects are accessed through the
existing LoomMem API and GPtr references embedded in that record. Inline
arguments for registered cluster functions are limited to 80 bytes (128-byte
queue payload minus the 48-byte thread prefix). Larger data must be supplied
through GPtrs. Callback failures are reported as a failed join.

## Cluster function registration

Every participating process calls `cl_pthread_register_functions` during
bootstrap, before multi-host `cl_pthread_create`. The same manifest can be
compiled into each binary; callbacks can be private/static symbols and can have
different virtual addresses under ASLR. No `-rdynamic`, `dladdr` or dummy create
on execution hosts is required.

```c
static void *worker(void *bytes) { /* decode the agreed argument record */ return 0; }
const cl_pthread_function_t functions[] = {
    {"application.worker", worker, 1, sizeof(struct worker_args), UINT64_C(0x1001)}
};
cl_status_t status = cl_pthread_register_functions(runtime, functions, 1, 10000);
/* Check status before creating work. Every configured host makes this call. */
```

A descriptor declares a stable name, nonzero ABI version, exact argument byte
count and nonzero application-assigned schema ID. Change the version/schema ID
when the serialized argument layout or result-token interpretation changes. The
runtime compares these declarations; it does not prove that two callback bodies
implement identical semantics. Names are 1–63 bytes; a manifest has at most 256
entries. Names/IDs and local callback bindings must be unique. Registration order
may differ. Empty manifests are supported, but cannot launch any callbacks.

Registration installs all local bindings before advertising descriptors through
the existing CXL queues. Peers can receive advertisements before their own
registration call. Fragment indices support out-of-order delivery; identical
repeats are harmless and conflicting repeats fail closed. Each node waits for
matching complete manifests from every configured host, including platform ABI
checks. Only names/schema metadata cross the queues, never callback pointers.
Stable function IDs identify the installed callback on the execution host, which
also validates the invocation's argument length before running it.

`CL_UNAVAILABLE` means a registration timeout: installed bindings remain intact
and an identical call may wait again. `CL_FAILED_PRECONDITION` reports an
incompatible peer manifest or an attempt to replace the installed manifest.
Local invalid/duplicate declarations are rejected before installation. There is
one immutable manifest per runtime/bootstrap session; dynamic replacement and
code distribution are not supported. Keep all runtimes progressing during the
collective, including when reporting an error.

Multi-host create without a manifest returns `CL_FAILED_PRECONDITION`;
an unlisted callback returns `CL_NOT_FOUND`; a wrong argument size returns
`CL_INVALID_ARGUMENT`. A nested create on a remote worker cooperatively waits
if that worker's host is still completing its local registration rendezvous.
The user continues to call `cl_pthread_create` with the local callback pointer;
placement and remote resolution remain internal. Single-host applications retain
the existing no-manifest API; once they install a manifest they use its contract.

The C++ `LoomParRuntime`, `ThreadManager`, `PlacementScheduler` and queue message
types are implementation interfaces for tests and runtime integration. User
code should not depend on placement hints, load snapshots, migration state or
control messages.

`cl_pthread_barrier_init` records a fixed local participant count for one world
barrier. All configured hosts participate. The barrier is blocking and reusable;
the user calls it once per phase from each member of the application-defined
cohort. Host membership, generation messages and release/acquire hooks are
managed internally.

## Regression coverage for lifecycle and cooperative waits

`cxloom_loompar_regression_test` checks repeated/concurrent result-bearing calls,
exception results, nested joins deeper than the worker pool, 24-fiber barriers,
signal versus broadcast, simultaneous timed waits, detach before/after completion,
running-capacity reuse, staged-write isolation and a token waiter cohort larger
than the worker pool. `cxloom_fiber_test` also checks context restoration.

The 16-process threads workload now checks nonzero remote result tokens and
remote detach reclamation. `cxloom_loompar_condition_process_test` exercises
shared-handle attachment, cross-host mutex exclusion, one-waiter signal,
broadcast, repeated timed waits, mutex reacquisition and shared-object cleanup
across 16 independent file-backed processes. These are not a new DAX hardware
acceptance run.

Cluster registration and the public C remote lifecycle are verified by
`cxloom_cluster_registry_test` and the five `cxloom_loompar_cluster_*_test`
process scenarios. The 16-container results and the separate DAX device blocker
are recorded in [the registration verification report](loompar-cluster-registration-20260918.md).

## 工作集驱动的内存感知放置

新增 `cl_pthread_create_with_working_set`，按调用的 GPtr 范围、读写类型和权重，结合实际 token、最后写入者和当前版本副本选择执行节点。原 `cl_pthread_create` 保持兼容。接口示例、代价模型、布局升级和验证范围见 [内存感知放置说明](loompar-memory-aware-placement.md)。

工作集及其读写类型由应用在创建调用时声明。运行时会在真正执行
`cl_mem_read` / `cl_mem_write` 时执行一致性检查，但创建时尚未运行回调，
无法自动知道未来将访问哪些 GPtr。错误或缺失的提示只影响放置质量，
不能绕过 LoomMem 的访问权限和版本检查。

`cl_config_t::placement_policy` 可选择：

- `CL_PLACEMENT_MEMORY_AWARE`（默认）：执行负载、队列压力和工作集局部性联合放置。
- `CL_PLACEMENT_ROUND_ROBIN`：在未达到容量上限的节点间轮询。
- `CL_PLACEMENT_LEAST_LOADED`：选择归一化可运行负载最低的节点。

轮询和最小负载是独立基线，不查询也不校验工作集局部性；工作集仅在
`CL_PLACEMENT_MEMORY_AWARE` 下参与决策。显式内部放置同样绕过局部性查询。

执行负载将正在占用 worker 的 Fiber、已就绪 Fiber 和阻塞 Fiber 分开统计。
最小负载和内存感知策略以 `(executing + ready) / workers` 表示计算需求；
blocked 仍计入活动线程容量，但不计为 CPU 可运行负载。已派发而尚未出现在
远端遥测中的调用作为预测负载加入。节点每 100 ms 发布一次状态，创建和完成
事件也触发更新；待发送的旧负载消息会被同目标的新样本替换。
