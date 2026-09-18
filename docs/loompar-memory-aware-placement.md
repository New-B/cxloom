# LoomPar 工作集与内存感知放置

线程在创建时选择执行节点，运行期间保持 pinned。本实现贯通 LoomMem 的一致性状态查询、调用工作集提示、LoomPar 放置和既有远程启动/join 协议。

## 调用接口

原来的 `cl_pthread_create` 保持兼容。需要内存感知放置时使用：

```c
cl_working_set_entry_t working_set[] = {
    {input, 0, input_bytes, CL_MEMORY_READ, 1.0},
    {output, 0, output_bytes, CL_MEMORY_WRITE, 2.0},
};
cl_status_t status = cl_pthread_create_with_working_set(
    runtime, &thread, registered_callback, &args, sizeof(args),
    working_set, 2);
```

`object` 必须是分配返回的对象 GPtr，`offset` 为对象内偏移，`bytes=0` 表示剩余范围；weight 是相对访问权重，必须有限、正数且不大于 1e6。每次最多 256 项。重叠项按重复访问累加。读写访问用 `CL_MEMORY_READ_WRITE`。非法范围和提示会返回错误。

跨节点调用仍需集群函数注册，参数仍受 80 字节限制，大数据通过 GPtr 传递。工作集只用于 home 节点创建时决策，不占远程参数载荷，不授予访问权限，也不延长对象在子线程运行期间的寿命；调用者需保证对象存活至访问完成。

C++ 使用 `ThreadPlacementHint::working_set`，适用于 `CreateThread` 和 `CreateRegisteredThread`。未提供工作集时，既有 dominant GPtr 转换为整个对象的读写提示。完全没有提示时沿用负载调度。

读写类型是创建时的用户提示，不由系统静态推断。系统能够观察已经发生的
LoomMem 读写，却无法在首次创建前知道回调未来的控制流和 GPtr 访问集合。
实际访问仍由 LoomMem 执行 token 和版本检查，因此提示不正确只会降低放置
效果，不会改变一致性语义。

## 真实局部性

`LoomMemRuntime::QueryLocality` 按访问范围内的一致性块返回覆盖字节数、当前版本、token 所有者、最后发布写入的节点和持有当前版本副本的节点位图。

- 查询期间持有对象引用，防止回收元数据。
- last_writer 仅随 modified release 更新；初始值 kMaxHosts 表示尚无写入。token 转移、放弃写入不改变它。
- 每个块为各节点保存缓存版本加一，零表示没有缓存。缓存安装、提升、淘汰、失效时更新；DRAM 缓存命中路径不增加共享原子写。
- 查询将缓存版本与当前发布版本比较。其他节点保留旧快照不意味着下一次 acquire 可以复用它。
- 通过 writeback epoch 重试避免把一次写入前后的字段拼接。连续 32 次采样失败返回 Unavailable，调用者可重试。不同块不保证同一时刻快照；放置结果是建议，不能作为一致性权限依据。

## 放置代价

保留原来的容量限制、负载衰减、队列和未完成启动历史成本，叠加工作集成本：

- 读：有当前版本本地副本为 0，否则为 1；最后写入节点的无副本成本估计为 0.75。
- 写：token 在本地为 0，否则为 1。
- 读写：两项相加。
- 按覆盖字节数乘 weight 加权取平均，再乘 `scheduler_remote_locality_penalty`。

这是可解释的启发式，并非实测网络延迟模型。足够大的负载可压过局部性收益，容量限制始终有效。线程创建先验证放置，再发布调用者暂存写入，然后重新采样决策，确保使用发布后的局部性。发布后若竞争导致创建失败，release 边界已经发生。

当前还提供 round-robin 和 least-loaded 两个基线策略。三种策略均遵守显式
目标和容量限制。least-loaded 使用按 worker 数归一化的 executing + ready，
忽略 blocked 的 CPU 成本，并计入尚未被远端样本覆盖的已派发调用；
memory-aware 在该执行负载上继续叠加现有队列、历史和局部性项。本阶段没有
增加副本访问热度，也没有校准新的硬件访存成本模型。

## 布局与成本

bootstrap/allocator 布局升级到 13，coherence 布局升级到 3。所有节点需统一升级并重新初始化共享区域，不能直接接入旧布局。

为精确区分各节点旧副本和新副本，块描述符从 64 字节增为 576 字节（64 个节点的版本槽）。默认 4 KiB 块对应约 14.1% 的块元数据成本，细粒度块开销更高；共享 coherence 区域可容纳的块数相应减少。这是本实现的明确代价，后续可以使用稀疏副本目录优化，但不能用对象级 replica_hosts 生命周期位图代替版本化的块局部性。

## 验证

`cxloom_loompar_locality_test` 使用两个节点的独立 runtime 和真实共享文件映射验证：token 转移、abort 不修改 writer、发布版本、旧副本排除、acquire 后重新缓存、LRU 淘汰、分块权重、过载回退和非法提示；并通过集群注册回调实际验证两个方向的远程启动、返回值和 join 回收。

C API 测试覆盖新增入口及范围错误。现有跨进程集群测试继续验证远程生命周期。共享文件测试不等同于物理多机 CXL/DAX 性能验证。本阶段未实现动态访问热度采样、硬件延迟校准或运行中迁移。

2026-09-18 的局部性里程碑验证：Debug 构建成功；完整 CTest 35/35 通过（90.56 秒）；补充暂存写入发布后重新放置与 C 接口测试后，局部性、C API、调度器三项定向回归全部通过。

同日负载与策略扩展验证：完整 CTest 38/38 通过（237.03 秒）。新增
`cxloom_loompar_load_test` 覆盖 executing/ready/blocked 分类、worker 归一化、
遥测延迟预测、容量限制和轮询游标；两个 16 进程场景分别强制使用
round-robin 与 least-loaded，并验证远程返回、join 和 detach 回收。
