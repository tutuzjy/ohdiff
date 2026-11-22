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
#include <common/defs.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include "BackupTask.h"
#include <BackupLayer.h>
#include <common/utils.h>
#include <ProgressHandler.h>

namespace fs = std::filesystem;

class Backup {
public:
    fs::path backup_dir;
    std::vector<std::shared_ptr<ArchiveBackupLayer>>
        archive_layers; // 每一层的索引。不建议使用 concurrent_vector，因为要求内存连续性。

    explicit Backup(const fs::path& backup_dir);

    // Layer 获取 ==================================================
    inline layer_id_t num_layers() const
    {
        return archive_layers.size();
    }

    // 前 n 个 layer 的时间在 time 之前
    layer_id_t num_layers_at_time(const std::chrono::system_clock::time_point& time) const
    {
        auto it = std::find_if(archive_layers.rbegin(), archive_layers.rend(),
                               [&](auto layer) { return layer->time <= time; });
        return std::distance(it, archive_layers.rend());
    }

    std::shared_ptr<VirtualBackupLayer> first_n_layers(layer_id_t n) const
    {
        if (n > archive_layers.size()) [[unlikely]] {
            throw std::out_of_range(
                fmt::format("{}: required layers ({}) exceeds total layers ({})", __func__, n, archive_layers.size()));
        }
        auto ret = BackupLayer::merge<VirtualBackupLayer>(std::span(archive_layers).first(n));
        ret->reconstruct();
        return ret;
    }

    std::shared_ptr<VirtualBackupLayer> all_layers() const
    {
        return first_n_layers(archive_layers.size());
    }
    // Layer 基本信息 ================================================
    inline size_t pool_size(layer_id_t layer_id) const
    {
        return archive_layers[layer_id]->pool_size;
    }
    inline int pool_fd(layer_id_t layer_id) const
    {
        return archive_layers[layer_id]->pool_fd;
    }
    inline uint8_t* pool_data(layer_id_t layer_id)
    {
        return archive_layers[layer_id]->pool_data;
    }
    inline void open_pools()
    {
        for (const auto& layer : archive_layers) {
            layer->open_pool();
        }
    }
    inline void mmap_pools()
    {
        for (const auto& layer : archive_layers) {
            layer->mmap_pool();
        }
    }
    inline void close_pools()
    {
        for (const auto& layer : archive_layers) {
            layer->close_pool();
        }
    }
    // 备份和还原 ================================================

    void add_layer(const fs::path& src_dir, int compress_level = NO_COMPRESSION_LEVEL,
                   std::shared_ptr<ProgressHandlerInterface> progress_handler = nullptr,
                   const fs::path& log_file = {});

    // restore first n layers
    void restore(layer_id_t n, const fs::path& restore_path, const fs::path& picked = "/",
                 std::shared_ptr<ProgressHandlerInterface> progress_handler = nullptr);
    inline void restore(const fs::path& restore_path, const fs::path& picked = "/",
                        std::shared_ptr<ProgressHandlerInterface> progress_handler = nullptr)
    {
        restore(num_layers(), restore_path, picked, std::move(progress_handler));
    }

    // 维护和优化 ================================================

    // 合并 [begin, end) 的 layers
    void merge_layers(layer_id_t begin, layer_id_t end);

    void delete_last_n_layers(layer_id_t n);
};
