# LoomPar user API boundary

The application-facing interface is the opaque C API in
`include/cxloom/cxloom_mem.h`. Users call `cl_runtime_create`/`cl_runtime_destroy`
and the pthread-shaped operations:

```c
cl_pthread_create(runtime, &thread, start_routine, argument_bytes, byte_count);
cl_pthread_join(runtime, thread);
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

Join now accepts an optional `void **retval`, and detached threads release their
native handle without a join. Completion messages carry a fixed-width
`uint64_t` result token, so remote joins no longer depend on process-local
pointer addresses. Applications should treat the token as an opaque result
handle or place structured results in a shared GPtr object.

Mutexes use a local native lock for a single-host private runtime. When multiple
hosts share a LoomMem region, initialization allocates an internal shared token
object and lock/unlock acquire and release its write lease, while preserving the
same opaque API. Distributed condition variables use a shared sequence word:
waiters release the distributed mutex, poll the sequence through LoomMem, and
reacquire it after signal/broadcast advances the sequence. This is a portable
notification protocol, though it is polling based.
Timed waits are available through `cl_pthread_cond_timedwait`; timeout returns
`CL_UNAVAILABLE` after the mutex has been reacquired.

`cl_mem_read` and `cl_mem_write` are the C-level GPtr access path. They perform
LoomMem acquire/release operations for shared mappings, so callers do not need
to resolve a process-local address. Private single-host runtimes still require
the existing local-resolve API for direct memory access.

The `arg_bytes` parameter is the C API's explicit serialization boundary. It
copies a bounded argument record into the launch message; it never transmits a
process-local pointer. For remote work, shared objects are accessed through the
existing LoomMem API and GPtr references embedded in that record. Inline
arguments are limited by the queue payload (104 bytes in the current wire
format). Callback failures are reported as a failed join.

Function identity is derived internally from the callback's exported symbol
using `dladdr`, then registered under a stable name on each participating
runtime. Applications should link callbacks with visible symbols when they may
execute remotely. If no symbol is available, the runtime uses a process-local
fallback identity and the callback is suitable only for local execution; this
limitation will be removed by a future cluster function manifest.

The C++ `LoomParRuntime`, `ThreadManager`, `PlacementScheduler` and queue message
types are implementation interfaces for tests and runtime integration. User
code should not depend on placement hints, load snapshots, migration state or
control messages.

`cl_pthread_barrier_init` records a fixed local participant count for one world
barrier. All configured hosts participate. The barrier is blocking and reusable;
the user calls it once per phase from each member of the application-defined
cohort. Host membership, generation messages and release/acquire hooks are
managed internally.
