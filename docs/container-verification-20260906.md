# 16 容器实测（2026-09-06）

当前工作区已成功启动 `cxloom-h0` 至 `cxloom-h15`，并通过以下真实
`/dev/dax0.0` 共享区域测试。此前报告中的 Docker 权限阻塞在本次环境中未出现。

## 环境与依赖修复

- 16 容器各占 8 个物理核，共 128 个不重叠 CPU，按主机 ID 对 4 个 NUMA 节点轮转分配，CPU 与内存节点一致；每容器内存上限 32 GiB。
- 首次构建因缺少 Python 3 失败。已在 `docker/Dockerfile` 添加 `python3`，成功重建 `cxloom:dev`，并重新创建本次启动的 16 个容器。
- 所有 DAX 工作负载依次运行；各测试的 host 0 会重新初始化共享区域。

## 验证结果

| 测试 | 参数与结果 |
| --- | --- |
| NUMA 绑定 | 16/16 通过，检查 CPU 集合无重叠、内存绑定和 host 环境变量 |
| host-init | 16/16 加入成功，每主机 4 次分配，分配无重叠、远端读取正确 |
| queue transport | 每主机 100,000 轮，240 个队列，每主机 3,000,000 次操作；16/16 `errors=0` |
| token stress | 每主机 10,000 轮；16/16 `errors=0` |
| coherence stress | 每主机 1,000 轮；16/16 `errors=0` |
| LoomPar threads | `CL_PAR_ROUNDS=30`，每主机 4 个创建者，16/16 PASS；每主机执行 3,600 次，共 57,600 次，`home_records=0`，嵌套 create/join 通过 |
| LoomPar sync | 16/16 PASS，固定 24 轮 barrier，覆盖 create/start/complete/join/barrier |
| 容器内回归 | 在 cxloom-h0 完整编译后 CTest 22/22 通过，耗时 6.05 秒；smoke 通过 |

线程验证使用 30 轮，未运行默认 300 轮长时间压力测试。CTest 只在 h0 执行；
上表六项共享区域工作负载均由全部 16 容器参与。本次通过不代表尚未实现的
跨进程 continuation 迁移已经完成，也不构成系统性性能评估。

最终检查时 16 个容器仍全部运行，均未标记 OOMKilled，保留供后续使用。
原有工作区修改未回退；本次代码修改仅补充镜像 Python 依赖。

日志与最终容器配置：`run/verify-16-20260906/`。其中 `results.json`
记录队列、token、一致性、线程及同步脚本退出码，均为 0；
`threads-details/` 和 `sync-details/` 保存逐主机构建及运行日志。
