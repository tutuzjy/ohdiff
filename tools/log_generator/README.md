交互式生成二进制文件系统事件日志，用于手工构造日志测试数据。文件和目录的实际变更需另行执行。

默认构建不包含此工具。编译方式参考主项目 [README.md](../../README.md#日志实验工具)，配置时通过 `-DBUILD_LOG_TOOLS=ON` 启用。编译后的输出是 `build/<arch>/tools/log_generator/log_generator`。

用法：

```bash
log_generator <日志路径>
```

按提示输入事件类型、相对于待备份目录的路径，以及是否仅修改元数据（`0` 否，`1` 是）。事件类型 `0`～`3` 分别表示文件创建、修改、删除、重命名，`4`～`7` 表示对应的目录操作；重命名时先输入新路径，再输入旧路径，输入事件类型 `8` 退出。

例如：

```bash
build/linux/tools/log_generator/log_generator /tmp/changes.log
```

启动时会覆盖指定路径下已有的日志文件。
