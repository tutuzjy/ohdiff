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

#include <RestoreTask.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <tbb/parallel_for_each.h>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include "BackupLayer.h"
#include "common/defs.h"
#include <writer/datamov.h>

namespace {
void ValidateChunkRange(const Chunk& chunk, size_t poolSize)
{
    if (chunk.offset.loffset > poolSize || chunk.physical_size > poolSize - chunk.offset.loffset) {
        throw std::runtime_error("Chunk exceeds the backup pool");
    }
}
} // namespace

void RestoreTask::restore_file_lowmem(const FileMetadata* file_meta, int file_fd)
{
    // restore file data
    size_t offset = 0;
    for (index_t chunk_i : file_meta->chunks) {
        const Chunk* chunk_meta = base_layer->chunks[chunk_i];
        ValidateChunkRange(*chunk_meta, backup.pool_size(chunk_meta->offset.layer_id));

        size_t delta;
        if (chunk_meta->compressed == Chunk::COMPRESSED) {
            delta = datamov::decompress_sendfile(file_fd, backup.pool_fd(chunk_meta->offset.layer_id),
                                                 chunk_meta->offset.loffset, chunk_meta->physical_size);
        } else {
            delta = datamov::sendfile(file_fd, backup.pool_fd(chunk_meta->offset.layer_id), chunk_meta->offset.loffset,
                                      chunk_meta->physical_size);
        }
        if (delta > file_meta->size - offset) {
            throw std::runtime_error("Restored chunk exceeds the declared file size");
        }
        offset += delta;

        if (progress_handler) {
            progress_handler->add_progress(delta);
        }
    }
}

void RestoreTask::restore_file_mmap(const FileMetadata* file_meta, int file_fd)
{
    size_t offset = 0;
    for (index_t chunk_i : file_meta->chunks) {
        const Chunk* chunk_meta = base_layer->chunks[chunk_i];
        ValidateChunkRange(*chunk_meta, backup.pool_size(chunk_meta->offset.layer_id));
        const uint8_t* chunk_data = backup.pool_data(chunk_meta->offset.layer_id) + chunk_meta->offset.loffset;

        size_t delta;
        if (chunk_meta->compressed == Chunk::COMPRESSED) {
            delta = datamov::decompress_write(file_fd, chunk_data, chunk_meta->physical_size);
        } else {
            delta = datamov::write(file_fd, chunk_data, chunk_meta->physical_size);
        }
        if (delta > file_meta->size - offset) {
            throw std::runtime_error("Restored chunk exceeds the declared file size");
        }
        offset += delta;

        if (progress_handler) {
            progress_handler->add_progress(delta);
        }
    }
}

void RestoreTask::restore_file(const fs::path& dst_file, index_t file_i)
{
    const FileMetadata* file_meta = base_layer->file_index[file_i];

    int file_fd = open(dst_file.c_str(), O_RDWR | O_CREAT, file_meta->mode); // @FileSystem Modification.
    if (file_fd == -1) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to create file '{}'. {}", __func__, dst_file.string(), strerror(errno)));
    }
    if (ftruncate(file_fd, file_meta->size) == -1) [[unlikely]] {
        close(file_fd); // @FileSystem Modification
        throw std::runtime_error(
            fmt::format("{}: Failed to truncate file '{}'. {}", __func__, dst_file.string(), strerror(errno)));
    }

    // restore file data
    if (file_meta->size > 0) [[likely]] {
        if (global_config.performance_mode) {
            restore_file_mmap(file_meta, file_fd);
        } else {
            restore_file_lowmem(file_meta, file_fd);
        }
    }
    close(file_fd);                                                                   // @FileSystem Modification
    fs::last_write_time(dst_file, file_meta->mtime);                                  // @FileSystem Modification
    if (chown(dst_file.c_str(), file_meta->uid, file_meta->gid) == -1) [[unlikely]] { // @FileSystem Modification
        throw std::runtime_error(
            fmt::format("{}: Failed to set file ownership '{}'. {}", __func__, dst_file.string(), strerror(errno)));
    }
    if (chmod(dst_file.c_str(), file_meta->mode) == -1)
        [[unlikely]] { // @FileSystem Modification. 前面的 open 虽然指定了创建文件的
                       // mode，但是文件本身存在的情况下，open 不能改变文件的 mode，所以这里需要再次设置一次。
        throw std::runtime_error(
            fmt::format("{}: Failed to set file mode '{}'. {}", __func__, dst_file.string(), strerror(errno)));
    }
}

void RestoreTask::restore_dir(const fs::path& dst_dir, index_t dir_i)
{
    const DirectoryMetadata* dir_meta = base_layer->dir_index[dir_i];
    fs::create_directory(dst_dir);                                                 // @FileSystem Modification
    if (chown(dst_dir.c_str(), dir_meta->uid, dir_meta->gid) == -1) [[unlikely]] { // @FileSystem Modification
        throw std::runtime_error(
            fmt::format("{}: Failed to set directory ownership '{}'. {}", __func__, dst_dir.string(), strerror(errno)));
    }
    if (chmod(dst_dir.c_str(), dir_meta->mode) == -1) [[unlikely]] { // @FileSystem Modification
        throw std::runtime_error(
            fmt::format("{}: Failed to set directory mode '{}'. {}", __func__, dst_dir.string(), strerror(errno)));
    }

    const auto& subfiles = base_layer->subfiles[dir_i];
    const auto& subdirs = base_layer->subdirs[dir_i];

    tbb::parallel_invoke(
        [&]() {
            tbb::parallel_for_each(subfiles.begin(), subfiles.end(), [&](index_t file_i) {
                const FileMetadata* file_meta = base_layer->file_index[file_i];
                fs::path file_path = dst_dir / file_meta->name;
                restore_file(file_path, file_i);
            });
        },
        [&]() {
            tbb::parallel_for_each(subdirs.begin(), subdirs.end(), [&](index_t subdir_i) {
                restore_dir(dst_dir / base_layer->dir_index[subdir_i]->name, subdir_i);
            });
        });
}
