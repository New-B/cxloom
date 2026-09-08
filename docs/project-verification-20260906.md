# 当前项目验证（2026-09-06）

本次检查针对包含未提交改动的当前工作区；未覆盖或回退已有修改。

## 完成情况

| 范围 | 当前判断 |
| --- | --- |
| LoomMem / Phase 0–5B | 共享布局、分配回收、队列、token、块级一致性及用户 API 已实现，本次本地回归通过；历史 DAX 验证见基线报告，本次未重验 DAX。 |
| LoomPar / Phase 6–7 | 本地及远程 create/join、嵌套创建、代际 barrier 和内存发布同步已实现，16 进程验证通过；容器 DAX 验收仍待完成。 |
| Phase 8 | 调度策略、负载信息、准入限制及迁移事务模型已有实现和测试；仍属部分完成。运行时收到迁移请求明确返回 kUnimplemented，跨进程 continuation 迁移未完成。 |
| Phase 9 | 已有微基准和压力验证入口；真实容器调度性能、应用评估、故障恢复及系统性硬化尚未完成。 |

README 的 Current Status 和部分里程碑文字落后于当前代码；不能据此认为项目仍仅有生命周期实现，也不能将迁移模型测试通过视为真实迁移完成。

## 本次实测

- 新建 `build/verification-20260906`，以 Debug 配置完整编译成功。
- CTest **22/22 通过**，总耗时 **10.05 秒**，包括 LoomMem、C API、调度、资源限制、fiber、迁移模型及两项 16 进程测试。
- 补充线程压力验证：16 个独立文件映射进程，每主机 4 个创建者、30 轮；**16/16 PASS**，每主机执行 3,600 次，总计 **57,600 次**，检查嵌套 create/join 和每轮线程记录回收；host 15 报告耗时 54.227 秒。
- 补充同步验证：**16/16 PASS**，每主机 1–2 个参与者、固定 **24 轮**，验证 create/join 发布、barrier 代际可见性、参与数冲突传播及失败后的继续运行。同步程序不读取 CL_PAR_ROUNDS，日志文件名中的 30rounds 仅反映调用参数，实际为 24 轮。
- 调度模拟基准成功复现已有报告：队列压力场景加权策略平均延迟为 2，轮转为 4；偏斜工作负载下加权策略 P95 为 903，轮转为 848。单位是模拟单位，不能证明真实 DAX 性能提升。
- `git diff --check` 通过。

## 16 容器启动阻塞

实际执行了 `bash scripts/launch-numa-containers.sh 16`，在镜像预检阶段退出，未启动容器。直接调用 Docker 确认错误为：

```text
dial unix /var/run/docker.sock: socket: operation not permitted
```

启动脚本将所有 `docker image inspect` 失败都显示为镜像缺失，因此其 “Docker image cxloom:dev is missing” 输出不能证明镜像不存在。当前执行环境也未暴露 `/dev/dax*`。环境禁止提权，无法在本会话完成容器启动；没有运行 DAX 工作负载或格式化 DAX 区域。

在允许访问 Docker 且具备 DAX 设备的环境中，仍需启动或确认 16 个容器，检查 NUMA 绑定，然后依次运行 host-init、queue、token、coherence、LoomPar threads 和 sync 验收。共享区域会由各项测试重新初始化，必须串行运行这些测试。

本次日志保存在 `run/verification-20260906/`：`configure.log`、`build.log`、`ctest.log`、`container-launch.log`、`docker-access.log`、两个 `*16hosts*` 日志及 `scheduler.csv`。该目录被 Git 忽略。
