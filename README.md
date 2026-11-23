# ohdiff：利用 F2FS 操作日志加速增量备份

ohdiff 是一个增量备份示例，基于 [ohdiff(origin)](https://github.com/PenXLa/oh-diff/)改进。它沿用不可变备份层、FastCDC 内容分块和 xxHash 跨层去重机制，并利用 F2FS 操作日志减少增量备份中的目录遍历、元数据查询和不必要的内容读取。

阅读导航：

- [目录结构](#目录结构)
- [设计思路](#设计思路)
- [如何利用 F2FS log 加速增量备份](#如何利用-f2fs-log-加速增量备份)
- [日志加速相关代码位置](#日志加速相关代码位置)
- [环境与依赖](#环境与依赖)
- [独立构建](#独立构建)
- [备份与恢复](#备份与恢复)
- [日志实验工具](#日志实验工具)
- [参与开发人员](#参与开发人员)

## 目录结构

```text
ohdiff/
├── README.md
├── CMakeLists.txt          # 独立构建入口
├── .gitmodules            # 依赖仓库地址
├── LICENSE                 # Apache-2.0 许可证
├── include/               # 备份层、日志事件等接口
├── src/                   # 备份、恢复、分块和日志处理实现
├── tools/                 # 可选的日志生成器与目录修改器
└── third_party/           # 按提交固定的 Git submodule
```

## 设计思路

### 不可变备份层

ohdiff 将每次备份抽象为一个 `ArchiveBackupLayer`。初始备份生成基础层，后续备份追加增量层。常规备份通过新增层保存变化，不回写历史层；显式执行层合并、裁剪等维护操作时则会重写相关归档。

每层由两类文件组成：

- `.index`：目录和文件元数据、文件引用的 chunk 索引、chunk 的物理位置、删除和修改索引，以及层标识和时间等信息。
- `.data`：本层新增的、去重后的 chunk 数据，可按配置使用 Zstd 压缩。

文件元数据保存文件名、父目录索引、大小、修改时间、权限和 chunk 索引列表。chunk 描述保存摘要、所在层及层内偏移等信息。因此，新层中的文件可以继续引用历史层的 chunk，无需再次保存相同内容。

恢复某个历史版本时，`BackupLayer::merge<VirtualBackupLayer>()` 将截至该版本的层组合为内存视图，`VirtualBackupLayer::reconstruct()` 重建可见目录关系，再由恢复任务按 chunk 引用读取相应层的数据。虚拟层用于组织索引，不要求先把各层的数据复制成一份完整备份。

相关代码：[BackupLayer.h](include/BackupLayer.h)、[BackupLayer.cpp](src/BackupLayer.cpp)、[Backup.h](include/Backup.h)、[RestoreTask.cpp](src/RestoreTask.cpp)。

### FastCDC：按内容决定分块边界

文件内容通过 `StreamCDC` 分为可变长度的 chunk。实现使用 Gear hash 和不同区间的掩码寻找内容边界，并限制 chunk 大小；当前配置的最小值、目标平均值和最大值分别为 4 KiB、16 KiB、64 KiB，文件末尾允许产生更小的尾块。

按内容确定边界可以使文件局部插入或删除后，后续一部分未变化内容重新形成相同的 chunk，增加跨版本复用的机会。Gear hash 在这里用于选择分块边界。

相关代码：[Chunker.cpp](src/Chunker.cpp)、[Chunker.h](include/Chunker.h)、[common/defs.h](include/common/defs.h)。

### xxHash：识别已经保存的 chunk

`BackupTask` 开始时，将历史 chunk 的摘要加入 `chunk_table`。每个待保存的 chunk 通过 `XXH3_128bits()` 计算 128 位摘要：摘要已存在时复用其全局 chunk 索引；尚未出现过时才分配新索引、按需压缩并写入本层 `.data`。同次备份中的重复 chunk 也可复用。

FastCDC 负责分块，xxHash 负责摘要判重。当前实现以摘要相同作为复用判据，xxHash 是非密码学哈希；这里的去重机制不提供加密或身份校验能力。

相关代码：[BackupTask.cpp](src/BackupTask.cpp) 中的构造函数、`onAddedFile()`、`add_chunk()`，以及 [checksum.h](include/checksum/checksum.h)。

### 删除与修改索引

ohdiff 通过 `del_file_index` 和 `del_dir_index`，即删除索引（del index），记录被删除对象的历史全局索引。重建视图时据此隐藏对象，无需在每次备份时删除或改写历史记录。

在沿用删除索引的基础上，ohdiff 增加了按历史索引记录修改的能力，使备份层能够直接承接日志事件：

| 变化 | 层内表达 | 内容处理 |
| --- | --- | --- |
| 新建文件或目录 | 追加 `file_index` / `dir_index` | 新文件分块、去重，保存尚未出现的 chunk |
| 删除文件或目录 | 追加 `del_file_index` / `del_dir_index` | 按历史索引标记删除，保留历史备份数据 |
| 修改文件内容 | 在 `modified_file_index` 中记录旧索引对应的新元数据和 chunk 列表 | 读取该文件，继续使用 FastCDC 和 xxHash 去重 |
| 仅修改文件元数据 | 在 `modified_file_index` 中记录更新后的元数据 | 沿用旧 chunk 列表，避免重新读取文件内容 |
| 目录元数据修改、重命名或移动 | 通过 `modified_dir_index` 等记录更新后的名称、父索引和权限 | 按日志标记与路径映射处理，无需将旧内容全部视为新数据 |

文件修改由 `onModifiedFileMetadata()` 与 `onModifiedFileContent()` 分别处理：前者更新元数据并复用原有 chunk 列表，后者读取变化后的文件并重新分块、去重。两者都保留对象的历史索引，并把变化写入新层的修改表。组合层视图时应用修改表，删除索引仍用于决定对象是否可见。历史归档在常规增量备份中保持不变。

此外，`VirtualBackupLayer::reconstruct()` 构造 `relative_path_index_map` 和 `index_dirpath_map`，将日志中的相对路径关联到备份中的全局索引。这是将文件系统事件转换为层内索引变化的连接点。

相关代码：[BackupLayer.h](include/BackupLayer.h)、[BackupLayer.cpp](src/BackupLayer.cpp)、[BackupTask.cpp](src/BackupTask.cpp)。

## 如何利用 F2FS log 加速增量备份

当源目录很大而实际变化很少时，依靠递归扫描发现变化仍需遍历大量未变化路径。ohdiff 利用 F2FS 操作日志定位变化，再结合历史备份索引处理相应对象；分块、去重和压缩继续负责减少需要保存的数据量。

### 日志输入与运行前提

本样例消费配套 F2FS 文件操作日志生产端输出的二进制事件文件。**本目录包含用户态日志消费和备份逻辑，不包含内核侧日志生产实现；仅使用普通 F2FS 文件系统并不足以提供该输入。**

`VirtualFS` 对待备份根目录执行 `stat()`，按其 inode 号定位日志文件：

```text
/data/log/<待备份根目录的 inode>.log
```

增量备份可以通过 `backup --log-file <path>` 指定事件文件。省略该选项时使用上面的默认路径。日志读取失败会报错并结束当前备份。

每条事件依次包含：

| 字段 | 当前读取类型 | 含义 |
| --- | --- | --- |
| `type` | `EventType`，底层为 `uint8_t` | 文件或目录的创建、修改、删除、重命名，共八种事件 |
| `metadata_only` | `uint8_t` | 是否仅涉及元数据，决定是否需要重新读取文件内容 |
| `path_size` | `uint16_t` | 当前路径的字节长度 |
| `old_path_size` | `uint16_t` | 重命名前路径的字节长度 |
| `path` / `old_path` | 对应长度的字节串 | 相对于待备份根目录的路径；重命名事件携带新旧路径 |

读取器按本机字段大小及字节序解析，生产端需要与其保持一致。这些事件描述路径和操作，不包含备份所需的文件内容。

### 从事件流构造变化树

`VirtualFS::read_log_entry()` 将日志读入内存，解析为 `LogEntry` 列表；随后按记录顺序调用 `apply_log_entry()`，构造由 `TreeNode` 组成的变化树。变化树记录相关路径及祖先，而非重新枚举整棵源目录树。

节点上的 `is_created`、`deleted`、`metadata_only`、`renamed_from` 等状态用于归纳连续操作。例如：

- 一个文件在两次备份之间先创建再删除，最终无需保存该临时文件。
- 同一路径删除后重建，通过同名节点的不同历史记录区分旧对象和新对象。
- 文件或目录重命名时，通过 `renamed_from` 保留与旧备份索引的联系；`resolve_old_path()`、`BackupMap::resolve_renamed_index()` 处理祖先目录重命名后的路径解析。
- 文件发生过内容修改后，后续仅修改元数据的事件不会把它重新归类为纯元数据变化。

这一步把操作历史转换为备份任务需要处理的变化状态，避免把每条日志都执行成一次文件备份。

### 按变化树生成新的增量层

```text
配套 F2FS 日志生产端
        │ 输出路径事件
        ▼
/data/log/<inode>.log
        │ read_log_entry() 按顺序解析
        ▼
VirtualFS / TreeNode 变化树
        │ 结合历史路径与索引映射
        ▼
BackupTask 按变化路径处理创建、删除、修改、重命名
        ├── 删除 ───────────────► del_file_index / del_dir_index
        ├── 仅元数据变化 ───────► modified_*_index，复用旧 chunk 列表
        └── 新增或内容变化 ─────► FastCDC → XXH3_128bits → chunk 复用或写入
                                                    │
                                                    ▼
                                       新增 .index / .data 备份层
```

具体入口是 `BackupTask::exec()`：没有基础层时，仍通过 `onAddedDir()` 遍历源目录完成全量备份；已有基础层时，构造 `VirtualFS`，将变化树交给 `compare_directories_recursive()`。该函数遍历变化树的子节点，通过 `compare_subdir()`、`compare_subfile()` 将变化分派给相应处理函数。

因此，未变化的源路径通常无需再逐一枚举和查询元数据；纯元数据变化可以跳过内容读取；删除可直接映射为旧索引的删除标记。新增或内容变化的文件仍需读取当前内容、重新分块和去重。新增目录等相关处理路径也可能枚举其当前目录项，日志加速并不意味着完全消除目录访问。

例如，基础层中有 `a.txt`、`c.txt`、`d.txt` 及大量未变化文件；随后将 `a.txt` 重命名为 `b.txt`、修改 `c.txt`、删除 `d.txt`，并创建后删除一个临时文件。日志完整且重命名被标记为纯元数据操作时，新层可用修改表描述 `b.txt` 并复用原内容，仅对 `c.txt` 执行内容处理，用删除索引描述 `d.txt`，跳过已消失的临时文件。

**加速主要来自将“扫描源目录以发现变化”改为“读取操作日志并处理变化路径”，以及避免纯元数据变化触发内容重读。** FastCDC 与 xxHash 继续负责处理实际需要读取的内容，两者与日志加速互相补充。

当前实现仍会加载历史索引、组合层视图、重建路径映射，并将历史 chunk 摘要加入查找表；同时会读入整个日志文件。运行成本还取决于历史索引规模、日志长度和变化文件大小，不能将整个备份过程描述为只与变化文件数量有关，也不能将文件级操作日志理解为字节范围差分。

### 日志正确性与备份边界

跳过扫描的前提是日志完整覆盖“基础备份状态到本次备份状态”的操作，路径对应同一个备份根目录，事件顺序和 `metadata_only` 标记正确，并且读取的文件内容与日志边界一致。

当前用户态代码没有管理日志轮转、消费位点或日志清空，也没有建立源目录快照或文件系统冻结边界。这些需要由配套环境协调；日志缺失、重复覆盖旧备份区间或备份期间发生未协调的并发修改，都不能靠此样例自动恢复成可靠的变化集合。

为手工构造实验输入，ohdiff 还提供 [log_generator](tools/log_generator/README.md) 和 [updater](tools/updater/README.md)：前者交互生成日志但不修改真实文件，后者修改测试目录并可同时输出事件日志。它们用于实验和复现，不等同于内核侧对实际 F2FS 操作的持续采集。

## 日志加速相关代码位置

阅读日志加速实现时，优先查看前三项，再查看备份层和内容处理部分。

| 文件 | 主要类型或函数 | 职责 |
| --- | --- | --- |
| [include/log/log.h](include/log/log.h) | `EventType`、`LogEntry`、`TreeNode`、`VirtualFS`、`BackupMap` | 定义日志事件、变化树状态及路径到备份索引的关联接口 |
| [src/log/log.cpp](src/log/log.cpp) | `VirtualFS()`、`read_log_entry()`、`apply_log_entry()`、`resolve_old_path()`、`resolve_renamed_index()` | 定位并解析日志，归纳事件，处理重命名与历史索引映射 |
| [src/BackupTask.cpp](src/BackupTask.cpp) | `exec()`、`compare_directories_recursive()`、`compare_subdir()`、`compare_subfile()`、`onModifiedFileMetadata()`、`onModifiedFileContent()` | 切换全量与日志增量路径，按变化树生成新层，决定是否读取文件内容 |
| [include/BackupLayer.h](include/BackupLayer.h) | `ArchiveBackupLayer`、`VirtualBackupLayer`、`BackupLayer::merge_to()` | 定义删除和修改索引、序列化及层视图组合逻辑 |
| [src/BackupLayer.cpp](src/BackupLayer.cpp) | `VirtualBackupLayer::reconstruct()` | 应用删除信息、重建目录关系与路径索引映射 |
| [src/Chunker.cpp](src/Chunker.cpp)、[checksum.h](include/checksum/checksum.h) | `StreamCDC`、`StrongChecksum`；调用点为 `BackupTask::add_chunk()` | 对新增或内容变化文件执行内容分块与摘要去重 |
| [tools/updater/log_writer.h](tools/updater/log_writer.h)、[log_generator/main.cpp](tools/log_generator/main.cpp) | `append_log_entry()`、日志生成示例 | 展示实验输入的二进制事件写入方式 |

## 环境与依赖

### 获取源码和依赖

构建需要 Git、CMake 3.28 或更新版本，以及支持 C++23 的 C++ 工具链。

```bash
git clone --recurse-submodules https://github.com/tutuzjy/ohdiff
cd ohdiff
```

已有检出可运行 `git submodule update --init --recursive`。Git 将依赖固定到主仓记录的提交。Boost 的源码还包含递归 submodule。

| 路径 | 用途 |
| --- | --- |
| `third_party/oneTBB` | 并行任务、并发容器和内存分配 |
| `third_party/boost` | ProgramOptions 和 Serialization |
| `third_party/xxHash` | chunk 摘要 |
| `third_party/zstd` | chunk 压缩和解压 |
| `third_party/fmt` | 日志和错误信息格式化 |

FastCDC 实现在 [src/Chunker.cpp](src/Chunker.cpp) 中，原始 MIT 声明见 [LICENSES/fastcdc-MIT.txt](LICENSES/fastcdc-MIT.txt)。

## 独立构建

### Linux 本地构建

```bash
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release -DSTATIC_BUILD=OFF
cmake --build build/linux --target ohdiff --parallel
```

主程序位于 `build/linux/ohdiff`。要构建日志实验工具，配置时增加 `-DBUILD_LOG_TOOLS=ON` 并构建相应目标。

### OpenHarmony 交叉构建

将 `OHOS_SDK` 设为包含 `native/` 的 OpenHarmony 7.0 Release Linux SDK 目录。按目标系统的用户态 ABI，将 `ohos_abi` 设为 `armeabi-v7a`、`arm64-v8a` 或 `x86_64`。下面使用 SDK 的工具链和动态 C++ 运行库。

```bash
ohos_abi=arm64-v8a
ohos_build="build/ohos-$ohos_abi"
cmake -S . -B "$ohos_build" \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH="$ohos_abi" -DOHOS_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release -DSTATIC_BUILD=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/$ohos_build/install"
cmake --build "$ohos_build" --target ohdiff --parallel
cmake --install "$ohos_build" --component ohdiff_runtime
```

部署 `install/bin` 和 `install/lib` 到目标设备，在运行时将 `install/lib` 加入 `LD_LIBRARY_PATH`。目标环境需提供与对应 SDK 匹配的 `libc++_shared.so`。运行真实日志演示前，按下文准备匹配的日志文件以及源目录、输出目录和属主恢复权限。

## 备份与恢复

备份和恢复请使用相同 ABI 的构建。归档包含随 ABI 变化的原生类型。32 位构建的单个源文件和单层数据池各须小于 4 GiB。

下面以 Linux 构建产物为例，在主程序所在目录执行命令：

```bash
cd build/linux
```

在目标设备上使用交叉构建产物时，进入设备上 `ohdiff` 所在目录执行相同命令。

### 全量与增量备份

```bash
./ohdiff backup --backup <backup-dir> --src <source-dir>
```

初始执行会生成全量基础层；源目录发生变化后，准备好覆盖本次增量区间的操作日志，再对同一备份目录执行备份：

```bash
./ohdiff backup --backup /backups/backup1 --src /mnt/sdcard/A1
# 源目录变化并准备好本次操作日志后，再执行一次：
./ohdiff backup --backup /backups/backup1 --src /mnt/sdcard/A1 --log-file /path/to/operations.log
```

备份目录中保存各层的 `*.data` 数据和 `*.index` 索引。初始备份仍扫描源目录；后续备份使用日志定位变化，具体输入与一致性要求见[日志输入与运行前提](#日志输入与运行前提)和[日志正确性与备份边界](#日志正确性与备份边界)。

### 恢复备份

```bash
./ohdiff restore --backup <backup-dir> --dst <destination-path>
```

默认恢复最新版本的全部数据，也可以选择路径、历史层或时间点：

```bash
# 最新版本的全部数据
./ohdiff restore --backup /backups/backup1 --dst /mnt/sdcard/A1-restored

# 仅恢复指定路径
./ohdiff restore --backup /backups/backup1 --dst /mnt/sdcard/A1-restored --pick /a.txt

# 恢复到第 3 次备份时的状态
./ohdiff restore --backup /backups/backup1 --dst /mnt/sdcard/A1-restored --layers 3

# 恢复到指定时间点
./ohdiff restore --backup /backups/backup1 --dst /mnt/sdcard/A1-restored --timepoint "2024-06-29T18:08:00"
```

`--layers` 与 `--timepoint` 不能同时指定。

### 合并历史备份

使用 `--start`（`-s`）和 `--end`（`-e`）指定合并范围。例如，合并第 1 次到第 3 次备份：

```bash
./ohdiff merge --backup /backups/backup1 -s 1 -e 3
```

### 查看历史版本

```bash
./ohdiff list --backup /backups/backup1
```

### 压缩、内存与线程设置

| 选项 | 用途 |
| --- | --- |
| `--compression <level>` | 指定压缩等级，默认值为 1；`--compression off` 关闭压缩 |
| `--no-low-mem` | 关闭低内存占用模式，使用性能模式 |
| `--threads <N>` 或 `-t <N>` | 指定线程数；小于 2 时按 2 处理 |

例如：

```bash
./ohdiff backup --backup /backups/backup1 --src /mnt/sdcard/A1 --compression 2
./ohdiff backup --backup /backups/backup1 --src /mnt/sdcard/A1 --no-low-mem
./ohdiff backup --backup /backups/backup1 --src /mnt/sdcard/A1 --threads 4
```

## 日志实验工具

`tools/` 中保留两个实验工具，默认不参与构建。需要时可在配置中启用 `BUILD_LOG_TOOLS`，下面是 Linux 构建命令。

```bash
cmake -S . -B build/linux \
    -DCMAKE_BUILD_TYPE=Release -DSTATIC_BUILD=OFF -DBUILD_LOG_TOOLS=ON
cmake --build build/linux --target log_generator updater
```

交叉构建时，在 OpenHarmony 配置命令中同样加入 `-DBUILD_LOG_TOOLS=ON`。工具输出位于对应构建目录的 `tools/` 子目录。

### 日志生成器

[log_generator](tools/log_generator/README.md) 交互生成二进制操作日志，不修改真实文件或目录：

```bash
build/linux/tools/log_generator/log_generator /tmp/changes.log
```

程序启动时覆盖指定日志文件；按提示输入事件类型、路径和元数据标记，输入事件类型 `8` 退出。

### 目录修改器

[updater](tools/updater/README.md) 随机创建、删除、修改和重命名测试目录中的对象，并可同时追加事件日志：

```bash
build/linux/tools/updater/updater /tmp/test-dir 10 5 /tmp/changes.log
```

示例中变更计数占初始文件/目录节点数约 5%～10%。该工具会直接修改指定目录，应使用测试数据副本；日志路径是可选参数。

将工具生成的日志用于增量备份时，可以用 `backup --log-file` 指定日志路径，并保证其内容、顺序和备份区间与源目录状态一致。

## 参与开发人员

以下开发人员均来自上海交通大学并行与分布式系统研究所（SJTU IPADS）。

| 系统 | 开发人员 |
| --- | --- |
| ohdiff | 曾靖垚、董明凯 |
| ohdiff(origin)（基础系统） | 蔡文俊、刘佳隆、赵龙宇、董明凯 |
