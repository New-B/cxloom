# LoomPar 集群函数注册与远程生命周期验证（2026-09-18）

本次在当前工作区实现公共 C API `cl_pthread_register_functions`。每个节点
提供同一份函数清单及本地回调绑定；通过 CXL 控制队列核对稳定名称、ABI
版本、参数大小、schema 标识和平台 ABI。注册顺序及本地函数地址可以不同，
无需导出回调符号，也无需在执行节点先创建一次线程。

只有本地绑定全部安装后才公布清单；创建者收齐所有节点的匹配清单后才允许
公共 C API 启动任务。接收端按稳定函数 ID 解析本地回调并检查参数大小。
清单分片容忍重排与相同重复，冲突重复或不兼容清单会失败；超时可用相同
清单重试。每个 runtime 只支持一份不可变清单，不分发代码或自动验证函数体
语义。具体调用方式及边界见 [API 说明](loompar-user-api.md#cluster-function-registration)。

## 验证结果

- 独立 Debug 构建及全仓 CTest：**34/34 通过**，89.36 秒。
- 注册协议单测覆盖：注册顺序不同、本地地址不同、提前接收、重复/重排、
  超时后重试、空清单、参数长度、重复绑定、不可变约束、名称/数量/ABI/schema
  不一致及冲突重复。
- 超时用例随后改为通过 bootstrap barrier 确保参与者延迟，并增加全局执行计数
  校验；该最终用例单独复验通过，33.82 秒。
- 16 独立进程覆盖成功启动、缺失函数、ABI 不一致、schema 不一致及超时重试。
  不兼容清单均在公共 API 创建线程前被拒绝。
- `cxloom-h0` 至 `cxloom-h15` 的 **16 容器共享文件后端验收全部通过**：
  共 768 次主调用，其中 764 次实际在创建者以外的节点执行；512 次主调用
  通过 join 回收，256 次通过 detach 回收；另有 128 次嵌套 create/join。
  全部结果槽及非零返回 token 校验通过，各节点执行计数合计恰为 768，
  创建端与执行端的远程计数一致，16 个 runtime 全部成功 finalize。
- 回调使用不导出的私有符号，参数使用 GPtr 引用共享结果，队列容量为 2。
  每个节点轮流作为创建者，由生产调度器自动选择执行节点；未使用强制目标
  主机接口或以本地调用代替远程验证。

## DAX 验收边界

已实际尝试 `/dev/dax0.0` 容器验收，但 host 0 在映射前失败。设备节点仍存在，
实际打开返回 `No such device or address`；现有 `cxloom_host_init` 也复现相同
错误。此次没有启动 DAX 工作负载，不把共享文件验收记作硬件 DAX 通过。

共享文件容器验证使用新建的独立文件，经已有 `/cxloom-shared` 共享挂载访问，
未覆盖已有应用数据。真实 DAX 验收仍需在设备恢复可用后重新执行：

```bash
CL_PAR_PROGRAM=cxloom_loompar_cluster_process ./scripts/run-loompar-containers.sh
```

该程序固定验证 16 × (32 joined + 16 detached) 次主调用及嵌套调用，
不使用脚本面向旧 threads 程序的 rounds/creators/delay 参数。
脚本亦支持 `CL_PAR_BACKING_FILE` 指定在各容器相同路径已存在的共享测试文件。

完整日志位于 `run/cluster-registration-20260918/`：
`ctest.log`、`container-file.log`、`container-file-details/`、`results.json`、
`dax-attempt.log` 和 `dax-details/`。目录按项目惯例被 Git 忽略。
