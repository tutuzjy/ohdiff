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

#include <Backup.h>
#include <BackupLayer.h>
#include <BackupTask.h>
#include <RestoreTask.h>
#include <MergeTask.h>
#include <common/defs.h>
#include <common/exceptions.h>
#include <common/utils.h>
#include <fcntl.h>
#include <Chunker.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_invoke.h>
#include <tbb/parallel_scan.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <string>

// =======================================================

Backup::Backup(const fs::path& backup_dir) : backup_dir(backup_dir)
{
    if (fs::exists(backup_dir)) {
        { // Load layers
            tbb::concurrent_vector<std::shared_ptr<ArchiveBackupLayer>> archive_layers_tmp;
            fs::directory_iterator dir_it_begin(backup_dir), dir_it_end;
            tbb::parallel_for_each(dir_it_begin, dir_it_end, [&](const auto& entry) {
                if (entry.is_regular_file() && entry.path().extension() == ".index") {
                    std::shared_ptr<ArchiveBackupLayer> layer = std::make_shared<ArchiveBackupLayer>();
                    layer->from_file(entry.path());
                    archive_layers_tmp.push_back(layer);
                }
            });
            archive_layers.assign(archive_layers_tmp.begin(), archive_layers_tmp.end());
        }

        std::sort(archive_layers.begin(), archive_layers.end(),
                  [](const auto& a, const auto& b) { return a->id < b->id; });

        // Sanity check
        for (layer_id_t i = 0; i < archive_layers.size(); i++) {
            if (archive_layers[i]->id != i) [[unlikely]] {
                throw std::runtime_error(fmt::format("{}: Backup layer ids are not continuous", __func__));
            }
            if (archive_layers[i]->pool_size > 0 && !fs::exists(archive_layers[i]->pool_path())) [[unlikely]] {
                throw std::runtime_error(fmt::format("{}: Backup layer pool file '{}' is not found", __func__,
                                                     archive_layers[i]->pool_path().string()));
            }
        }
    }
}

void Backup::add_layer(const fs::path& src_dir, int compressionLevel,
                       std::shared_ptr<ProgressHandlerInterface> progress_handler, const fs::path& log_file)
{
    if (num_layers() >= std::numeric_limits<layer_id_t>::max()) [[unlikely]] {
        throw std::out_of_range(fmt::format("{}: Number of layers exceeds limit", __func__));
    }

    // layer file name is not important, use timestamp as name
    std::string layer_file_name = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    fs::create_directories(backup_dir); // @FileSystem Modification

    BackupTask task(all_layers(), src_dir, num_layers(), backup_dir / (layer_file_name + ".index"), compressionLevel,
                    std::move(progress_handler), log_file);
    archive_layers.push_back(task.exec());
}

void Backup::restore(layer_id_t n, const fs::path& restore_path, const fs::path& picked,
                     std::shared_ptr<ProgressHandlerInterface> progress_handler)
{
    if (n > num_layers()) [[unlikely]] {
        throw std::out_of_range(
            fmt::format("{}: Number of layers ({}) to restore exceeds total layers ({})", __func__, n, num_layers()));
    }
    std::shared_ptr<VirtualBackupLayer> layer = first_n_layers(n);
    RestoreTask restorer(*this, layer, std::move(progress_handler));
    auto picked_entry = layer->find(picked);
    if (!picked_entry.found) {
        throw std::runtime_error(fmt::format("{}: Picked path '{}' not found in backup.", __func__, picked.string()));
    }

    if (picked_entry.is_dir) {
        restorer.restore_dir(restore_path, picked_entry.index);
    } else {
        fs::path file_path = restore_path;
        if (fs::is_directory(restore_path)) {
            file_path /= layer->file_index[picked_entry.index]->name;
        }
        restorer.restore_file(file_path, picked_entry.index);
    }
}

void Backup::merge_layers(layer_id_t begin, layer_id_t end)
{
    if (end > archive_layers.size()) [[unlikely]] {
        throw std::out_of_range(
            fmt::format("{}: End layer index ({}) exceeds total layers ({})", __func__, end, archive_layers.size()));
    }
    if (begin >= end) {
        return;
    }

    // 把要被合并的 Layer 提取出来
    std::vector<std::shared_ptr<ArchiveBackupLayer>> layers_to_merge;
    layers_to_merge.insert(layers_to_merge.end(), std::make_move_iterator(archive_layers.begin() + begin + 1),
                           std::make_move_iterator(archive_layers.begin() + end));
    archive_layers.erase(archive_layers.begin() + begin + 1, archive_layers.begin() + end);
    // 由于合并了 layers，需要更新后续的 id，以及 chunks 的 layer_id
    tbb::parallel_for(tbb::blocked_range<size_t>(begin + 1, archive_layers.size()), [&](const auto& range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            archive_layers[i]->id = i;
            tbb::parallel_for_each(archive_layers[i]->chunks.begin(), archive_layers[i]->chunks.end(),
                                   [i](Chunk& chunk) { chunk.offset.layer_id = i; });
        }
    });

    // 合并 layers
    BackupLayer::merge_to(layers_to_merge, archive_layers[begin]);
    update_files_dirs(archive_layers, begin);
    prune_files_dirs(archive_layers, begin);
    prune_chunks(archive_layers, begin);
    cat_prune_pool(archive_layers[begin], layers_to_merge); // @FileSystem Modification

    tbb::parallel_invoke(
        [&]() { // 删除被合并的 layers 文件和 pool 文件
            tbb::parallel_for_each(layers_to_merge.begin(), layers_to_merge.end(),
                                   [](const std::shared_ptr<ArchiveBackupLayer>& tmlayer) {
                                       fs::remove(tmlayer->saved_path);  // @FileSystem Modification
                                       fs::remove(tmlayer->pool_path()); // @FileSystem Modification
                                   });
        },
        [&]() { // 保存合并后的 layer
            tbb::parallel_for_each(archive_layers.begin() + begin, archive_layers.end(),
                                   [](const std::shared_ptr<ArchiveBackupLayer>& layer) {
                                       layer->to_file(); // @FileSystem Modification
                                   });
        });
}

void Backup::delete_last_n_layers(layer_id_t n)
{
    if (n > archive_layers.size()) {
        throw std::out_of_range("Number of layers to delete exceeds total layers");
    }
    for (layer_id_t i = 0; i < n; ++i) {
        fs::remove(archive_layers.back()->saved_path);
        fs::remove(archive_layers.back()->pool_path());
        archive_layers.pop_back();
    }
}
