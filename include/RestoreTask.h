/*
 * Copyright (c) 2026 ohdiff contributors.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <Backup.h>
#include <filesystem>
#include <memory>
#include "common/defs.h"
#include <BackupLayer.h>

namespace fs = std::filesystem;

class RestoreTask {
    Backup& backup;
    std::shared_ptr<const VirtualBackupLayer> base_layer;
    const std::shared_ptr<ProgressHandlerInterface> progress_handler;

public:
    RestoreTask(Backup& backup, std::shared_ptr<const VirtualBackupLayer> layer,
                std::shared_ptr<ProgressHandlerInterface> progress_handler = nullptr)
        : backup(backup), base_layer(layer), progress_handler(std::move(progress_handler))
    {
        if (global_config.performance_mode) {
            backup.mmap_pools();
        } else {
            backup.open_pools();
        }
    }
    ~RestoreTask()
    {
        backup.close_pools();
    }

private:
    void restore_file_lowmem(const FileMetadata* file_meta, int file_fd);
    void restore_file_mmap(const FileMetadata* file_meta, int file_fd);

public:
    void restore_dir(const fs::path& dst_dir, index_t dir_i);
    void restore_file(const fs::path& dst_file, index_t file_i);
};
