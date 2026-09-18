# 当前项目验证（更新至 2026-09-18）

本报告以当前工作区代码和 Debug 构建为准，替代 2026-09-06 的阶段性快照。

| 范围 | 当前判断 |
| --- | --- |
| LoomMem / Phase 0–5B | 共享布局、GPtr 分配回收、SPSC 队列、token、块级版本一致性、缓存版本跟踪和 C API 已实现并通过回归。物理非一致 CXL/DAX 验证仍受环境限制。 |
| LoomPar / Phase 6–7 | 集群函数注册、远程 create、返回值、join、detach、Fiber 阻塞调度、barrier、分布式 mutex/condition 和 release/acquire 边界已实现。固定世界成员模型和缺少主机恢复仍是边界。 |
| Phase 8 | 内存感知、轮询、最小负载三种放置策略已实现；执行负载区分 executing/ready/blocked，并通过版本化 LOAD_UPDATE 广播。副本热度和测量型成本模型按当前范围暂不实现。 |
| Phase 9 | 性能基准、物理 CXL 验收、故障注入、主机故障恢复、取消/截止时间和应用评估仍未完成。 |

## 当前实测

- Debug 构建成功，CTest **38/38 通过**。
- 覆盖 LoomMem 分配、回收、token、块范围、读写缓存和跨主机一致性。
- 覆盖 LoomPar 本地与 16 主机远程生命周期、嵌套 create/join、返回值、detach 回收和队列背压。
- 覆盖 cluster manifest 的缺失、schema、ABI、超时和成功场景。
- 覆盖 Fiber 阻塞、就绪队列、worker 归一化、遥测延迟预测和容量限制。
- 额外 16 主机场景分别强制 round-robin 与 least-loaded，均通过远程启动、join 和回收检查。
- `git diff --check` 通过。

## 运行边界

共享文件映射和容器脚本验证协议行为，不等同于物理多机非一致 CXL 的可见性或性能证明。运行中 Fiber/continuation 迁移不属于 pinned V1 契约；仓库中的迁移事务模型是未来实验接口，运行时收到迁移请求仍会返回 unsupported。

下一阶段应优先补齐故障检测与清理、取消/截止时间、动态 barrier 成员、任意阻塞系统调用隔离，以及真实 CXL 环境下的延迟、吞吐、token 转移和队列指标。
