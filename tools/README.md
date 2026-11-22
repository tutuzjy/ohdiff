# 日志实验工具

本目录保留两个用于构造增量备份实验输入的工具：

| 工具 | 用途 |
| --- | --- |
| [log_generator](log_generator/README.md) | 交互生成二进制操作日志，不修改真实文件系统 |
| [updater](updater/README.md) | 随机修改测试目录，可同时输出操作日志 |

两个工具通过 `-DBUILD_LOG_TOOLS=ON` 启用，具体构建方式见主项目 [README.md](../README.md#日志实验工具)。
