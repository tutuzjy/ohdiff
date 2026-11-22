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
#include <BackupLayer.h>
#include <Chunker.h>
#include <ProgressHandler.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/parallel_for_each.h>
#include <writer/ConcurrentWriter.h>

#include <filesystem>
#include <memory>

#include "log/log.h"

class BackupTask {
    std::shared_ptr<VirtualBackupLayer> base_layer;
    std::shared_ptr<ArchiveBackupLayer> incr_layer;
    std::unique_ptr<VirtualFS> vfs;
    const fs::path layer_file;
    const layer_id_t layer_id;
    const fs::path src_dir;
    const fs::path log_file;

    const int compressionLevel;

    using ChunkTable = tbb::concurrent_hash_map<StrongChecksum, index_t>;
    using ChunkTableAccessor = ChunkTable::accessor;
    ChunkTable chunk_table;

    std::unique_ptr<ConcurrentWriter> pool_writer;
    const std::shared_ptr<ProgressHandlerInterface> progress_handler;

    index_t onAddedDir(const index_t parent_index, const fs::path& path);
    void onAddedDir(const index_t parent_index, const fs::path& path, const std::shared_ptr<const TreeNode> dir);
    index_t onAddedFile(const index_t parent_index, const fs::path& path);
    void onDeletedDir(const index_t index);
    void onDeletedFile(const index_t index);
    // Metadata change ONLY!
    void onModifiedDir(const index_t index, const index_t parent, const fs::path& path);
    void onModifiedFileMetadata(const index_t index, const index_t parent, const fs::path& path);
    void onModifiedFileContent(const index_t index, const index_t parent, const fs::path& path);

    index_t AddDirectoryMetadata(index_t parent, const fs::path& path);
    index_t PrepareAddedDirectory(index_t parent, const fs::path& path, const std::shared_ptr<const TreeNode>& dir);
    void HandleDeletedChild(const std::shared_ptr<TreeNode>& node, const fs::path& parentPath);
    void HandleExistingChild(index_t parent, const fs::path& path, const std::shared_ptr<const TreeNode>& node);
    void calculate_chunks(const fs::path& path, std::vector<index_t>& chunks);
    index_t add_chunk(ChunkData* chunk);

    void compare_directories_recursive(index_t dir_i, const std::shared_ptr<const TreeNode> dir, const fs::path& fs_dir,
                                       const index_t parent);

    void delete_dir_subfiles(const std::shared_ptr<const TreeNode> dir);

    void compare_subdir(const std::shared_ptr<const TreeNode> dir, const index_t parent, const fs::path& fs_dir);

    void compare_subfile(const std::shared_ptr<const TreeNode> file, const index_t parent, const fs::path& fs_file);

    const index_t start_dir_index;
    const index_t start_file_index;
    const index_t start_chunk_index;

    inline index_t to_global_dir_index(const index_t local_index) const
    {
        return local_index + start_dir_index;
    }
    inline index_t to_global_file_index(const index_t local_index) const
    {
        return local_index + start_file_index;
    }
    inline index_t to_global_chunk_index(const index_t local_index) const
    {
        return local_index + start_chunk_index;
    }

public:
    BackupTask(std::shared_ptr<VirtualBackupLayer> base, fs::path src_dir, layer_id_t layer_id, fs::path layer_file,
               int compressionLevel, std::shared_ptr<ProgressHandlerInterface> progress_handler = nullptr,
               fs::path log_file = {});

    std::shared_ptr<ArchiveBackupLayer> exec();
};
