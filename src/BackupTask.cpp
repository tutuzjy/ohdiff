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

#include <BackupLayer.h>
#include <BackupTask.h>
#include <checksum/checksum.h>
#include <common/defs.h>
#include <sys/mman.h>
#include <tbb/scalable_allocator.h>
#include <writer/Compressor.h>
#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
size_t CheckedFileSize(const struct stat& file_stat, const fs::path& path)
{
    if (!std::in_range<size_t>(file_stat.st_size)) {
        throw std::overflow_error(fmt::format("Source file '{}' exceeds this build's size limit", path.string()));
    }
    return static_cast<size_t>(file_stat.st_size);
}
} // namespace

BackupTask::BackupTask(std::shared_ptr<VirtualBackupLayer> base, fs::path src_dir, layer_id_t layer_id,
                       fs::path layer_file, int compressionLevel,
                       std::shared_ptr<ProgressHandlerInterface> progress_handler, fs::path log_file)
    : base_layer(std::move(base)), layer_file(std::move(layer_file)), layer_id(layer_id), src_dir(std::move(src_dir)),
      log_file(std::move(log_file)), compressionLevel(compressionLevel), progress_handler(std::move(progress_handler)),
      start_dir_index(base_layer->dir_index.size()), start_file_index(base_layer->file_index.size()),
      start_chunk_index(base_layer->chunks.size())
{
    // Hash all the old chunks
    const auto& old_chunks = base_layer->chunks;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, old_chunks.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
            decltype(chunk_table)::accessor acc;
            chunk_table.insert(acc, old_chunks[i]->checksum);
            acc->second = i;
        }
    });
}

void BackupTask::compare_directories_recursive(index_t dir_i, const std::shared_ptr<const TreeNode> dir,
                                               const fs::path& fs_dir, const index_t parent)
{
    auto children = dir->get_children();

    if (dir->metadataOnly) [[unlikely]] {
        onModifiedDir(dir_i, parent, fs_dir);
        /*
         * The dir isn't deleted, created (newly created or deleted dir won't go
         * into this function) and has no children but appears in the vfs, which
         * means just its metadata is modified.
         */
        if (children.empty()) {
            return;
        }
    }

    tbb::parallel_for_each(children.begin(), children.end(), [&, this](const auto& child) {
        for (auto node : child.second) {
            if (node->is_dir) {
                compare_subdir(node, dir_i, fs_dir / child.first);
            } else {
                compare_subfile(node, dir_i, fs_dir / child.first);
            }
        }
    });
}

/*
 * This function will ONLY delete renamed and not moved away files/dirs in the
 * dir recursively.
 */
void BackupTask::delete_dir_subfiles(const std::shared_ptr<const TreeNode> dir)
{
    auto children = dir->get_children();
    tbb::parallel_for_each(children.begin(), children.end(), [&, this](const auto& child) {
        tbb::parallel_for_each(child.second.begin(), child.second.end(), [&, this](const auto node) {
            if (node->is_dir) {
                delete_dir_subfiles(node);
            }
            /*
             * Here we needn't check is_created because if renamed_from
             * is not -1, then the node can't be a newly
             * created file/dir.
             */
            if (node->renamed_from != -1) {
                node->is_dir ? onDeletedDir(node->renamed_from) : onDeletedFile(node->renamed_from);
            }
        });
    });
}

void BackupTask::compare_subdir(const std::shared_ptr<const TreeNode> dir, const index_t parent, const fs::path& fs_dir)
{
    if (dir->deleted) {
        /*
         * No matter the dir is newly created or not, we should delete its
         * subfiles/subdirs recursively that are renamed from other position!
         */
        delete_dir_subfiles(dir);
        if (!dir->is_created) {
            dir->renamed_from != -1 ? onDeletedDir(dir->renamed_from)
                                    : onDeletedDir(vfs->get_index(fs::relative(fs_dir, src_dir)));
        }
    } else if (dir->is_created) {
        onAddedDir(parent, fs_dir, dir);
    } else {
        compare_directories_recursive(dir->renamed_from != -1 ? dir->renamed_from
                                                              : vfs->get_index(fs::relative(fs_dir, src_dir)),
                                      dir, fs_dir, parent);
    }
}

void BackupTask::compare_subfile(const std::shared_ptr<const TreeNode> file, const index_t parent,
                                 const fs::path& fs_file)
{
    /*
     * If the node is newly created and deleted, nothing
     * should do.
     */
    if (file->deleted && file->is_created) {
        return;
    }
    if (file->renamed_from != -1) {
        /*
         * Here we needn't check is_created because if renamed_from
         * is not -1, then the node can't be a newly created
         * file.
         */
        if (file->deleted) {
            onDeletedFile(file->renamed_from);
        } else {
            file->metadataOnly ? onModifiedFileMetadata(file->renamed_from, parent, fs_file)
                               : onModifiedFileContent(file->renamed_from, parent, fs_file);
        }
    } else {
        if (!file->deleted) {
            if (file->is_created) {
                onAddedFile(parent, fs_file);
            } else {
                file->metadataOnly
                    ? onModifiedFileMetadata(vfs->get_index(fs::relative(fs_file, src_dir)), parent, fs_file)
                    : onModifiedFileContent(vfs->get_index(fs::relative(fs_file, src_dir)), parent, fs_file);
            }
        } else {
            onDeletedFile(vfs->get_index(fs::relative(fs_file, src_dir)));
        }
    }
}

index_t BackupTask::AddDirectoryMetadata(index_t parent, const fs::path& path)
{
    struct stat metadata {};
    if (stat(path.c_str(), &metadata) != 0) {
        throw std::runtime_error(fmt::format("Cannot stat directory {}: {}", path.string(), std::strerror(errno)));
    }
    auto it = incr_layer->dir_index.grow_by(1);
    *it = DirectoryMetadata{.name = path.filename(),
                            .parent = parent,
                            .uid = metadata.st_uid,
                            .gid = metadata.st_gid,
                            .mode = metadata.st_mode};
    return to_global_dir_index(it - incr_layer->dir_index.begin());
}

index_t BackupTask::onAddedDir(const index_t parent, const fs::path& path)
{
    const index_t index = AddDirectoryMetadata(parent, path);
    const auto entries = DirectoryEntries(path);
    tbb::parallel_for_each(entries.begin(), entries.end(), [&](const fs::path& entry) {
        if (fs::is_directory(entry)) {
            onAddedDir(index, entry);
        } else if (fs::is_regular_file(entry)) {
            onAddedFile(index, entry);
        }
    });
    return index;
}

index_t BackupTask::PrepareAddedDirectory(index_t parent, const fs::path& path,
                                          const std::shared_ptr<const TreeNode>& dir)
{
    if (dir->is_created) {
        return AddDirectoryMetadata(parent, path);
    }
    const auto relativePath = fs::relative(path, src_dir);
    const index_t index = dir->metadataOnly && dir->renamed_from != static_cast<index_t>(-1)
                              ? dir->renamed_from
                              : vfs->get_index(relativePath);
    if (dir->metadataOnly) {
        onModifiedDir(index, parent, path);
    }
    return index;
}

void BackupTask::HandleDeletedChild(const std::shared_ptr<TreeNode>& node, const fs::path& parentPath)
{
    if (!node->deleted || node->is_created) {
        return;
    }
    const index_t index = node->renamed_from == static_cast<index_t>(-1)
                              ? vfs->get_index(fs::relative(parentPath, src_dir) / node->name)
                              : node->renamed_from;
    if (node->is_dir) {
        delete_dir_subfiles(node);
        onDeletedDir(index);
    } else {
        onDeletedFile(index);
    }
}

void BackupTask::HandleExistingChild(index_t parent, const fs::path& path, const std::shared_ptr<const TreeNode>& node)
{
    if (node->is_dir) {
        onAddedDir(parent, path, node);
        return;
    }
    if (node->is_created) {
        onAddedFile(parent, path);
        return;
    }
    const index_t index = node->renamed_from == static_cast<index_t>(-1) ? vfs->get_index(fs::relative(path, src_dir))
                                                                         : node->renamed_from;
    if (node->metadataOnly) {
        onModifiedFileMetadata(index, parent, path);
    } else {
        onModifiedFileContent(index, parent, path);
    }
}

void BackupTask::onAddedDir(const index_t parent, const fs::path& path, const std::shared_ptr<const TreeNode> dir)
{
    const index_t index = PrepareAddedDirectory(parent, path, dir);
    const auto& children = dir->get_children();
    if (children.empty()) {
        return;
    }
    std::vector<fs::path> entries;
    const auto loadEntries = [&entries, &path]() { entries = DirectoryEntries(path); };
    const auto handleChildren = [this, &children, &path]() {
        tbb::parallel_for_each(children.begin(), children.end(), [this, &path](const auto& child) {
            tbb::parallel_for_each(child.second.begin(), child.second.end(),
                                   [this, &path](const auto& node) { HandleDeletedChild(node, path); });
        });
    };
    tbb::parallel_invoke(loadEntries, handleChildren);
    tbb::parallel_for_each(entries.begin(), entries.end(), [&](const fs::path& entry) {
        const auto child = children.find(entry.filename());
        if (child != children.end()) {
            HandleExistingChild(index, entry, child->second.back());
        }
    });
}

void BackupTask::calculate_chunks(const fs::path& path, std::vector<index_t>& chunks)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error(
            fmt::format("{}: Failed to open file '{}'. {}", __func__, path.string(), std::strerror(errno)));
    }
    StreamCDC chunker(ifs, CHUNK_MIN_SIZE, CHUNK_AVG_SIZE, CHUNK_MAX_SIZE, Normalization::Level1);

    if (global_config.performance_mode) {
        tbb::task_group task_diff;
        // 由于并发，需要使用 tbb::concurrent_vector 临时存储 chunk index
        // tbb::concurrent_vector 保证了在并发情况下，插入元素不会导致迭代器失效
        tbb::concurrent_vector<index_t> file_chunks;
        const auto allocateChunk = []() {
            auto* data = static_cast<ChunkData*>(scalable_malloc(sizeof(ChunkData)));
            if (data == nullptr) {
                throw std::bad_alloc();
            }
            return std::shared_ptr<ChunkData>(data, scalable_free);
        };
        auto chunk = allocateChunk();
        while (chunker.next_chunk_no_alloc(*chunk)) {
            auto chk_it = file_chunks.grow_by(1);
            task_diff.run([this, chunk, chk_it]() { *chk_it = add_chunk(chunk.get()); });
            chunk = allocateChunk();
        }
        task_diff.wait();
        chunks.assign(file_chunks.begin(), file_chunks.end());
    } else {
        ChunkData chunk;
        while (chunker.next_chunk_no_alloc(chunk)) {
            chunks.emplace_back(add_chunk(&chunk)); // non-concurrent vector
        }
    }
}

index_t BackupTask::onAddedFile(const index_t g_parent_index, const fs::path& path)
{
    auto file_it = incr_layer->file_index.grow_by(1);
    struct stat file_stat;
    if (stat(path.c_str(), &file_stat) != 0) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to stat file '{}'. {}", __func__, path.string(), std::strerror(errno)));
    }
    *file_it = FileMetadata{.size = CheckedFileSize(file_stat, path),
                            .mtime = fs::last_write_time(path),
                            .name = path.filename(),
                            .parent = g_parent_index,
                            .uid = file_stat.st_uid,
                            .gid = file_stat.st_gid,
                            .mode = file_stat.st_mode};
    index_t l_file_index = file_it - incr_layer->file_index.begin();
    calculate_chunks(path, file_it->chunks);

    return to_global_file_index(l_file_index);
}

index_t BackupTask::add_chunk(ChunkData* chunk)
{
    StrongChecksum checksum = XXH3_128bits(chunk->data, chunk->length);

    ChunkTableAccessor chk_acc;
    bool inserted = chunk_table.insert(chk_acc, checksum);
    index_t g_chunk_index;
    if (!inserted) {
        g_chunk_index = chk_acc->second;
        chk_acc.release();
    } else {
        auto chk_vec_it = incr_layer->chunks.grow_by(1);
        const index_t l_chunk_index = chk_vec_it - incr_layer->chunks.begin();
        g_chunk_index = to_global_chunk_index(l_chunk_index);
        chk_acc->second = g_chunk_index;
        chk_acc.release();

        *chk_vec_it = Chunk{.checksum = checksum, .offset = {.layer_id = layer_id}};

        // Write chunk data to pool file

        if (compressionLevel != NO_COMPRESSION_LEVEL) {
            uint8_t compression_buf[compression::compress_bound(chunk->length)];
            size_t compressed_size = compression::compress({compression_buf, sizeof compression_buf},
                                                           {chunk->data, chunk->length}, compressionLevel);
            const uint8_t* to_write_data = nullptr;
            if (compressed_size < chunk->length) {
                chk_vec_it->compressed = Chunk::COMPRESSED;
                chk_vec_it->physical_size = compressed_size;
                to_write_data = compression_buf;
            } else {
                chk_vec_it->compressed = Chunk::INTENTIONALLY_UNCOMPRESSED;
                chk_vec_it->physical_size = chunk->length;
                to_write_data = chunk->data;
            }
            chk_vec_it->offset.loffset = pool_writer->write(to_write_data, chk_vec_it->physical_size);
        } else {
            chk_vec_it->compressed = Chunk::UNCOMPRESSED;
            chk_vec_it->physical_size = chunk->length;
            chk_vec_it->offset.loffset = pool_writer->write(chunk->data, chunk->length);
        }
    }
    if (progress_handler) {
        progress_handler->add_progress(chunk->length);
    }
    return g_chunk_index;
}

void BackupTask::onDeletedDir(const index_t index)
{
    incr_layer->del_dir_index.push_back(index);
}

void BackupTask::onDeletedFile(const index_t index)
{
    incr_layer->del_file_index.push_back(index);
}

void BackupTask::onModifiedDir(const index_t index, const index_t parent, const fs::path& path)
{
    struct stat dir_stat;
    if (stat(path.c_str(), &dir_stat) != 0) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to stat directory '{}'. {}", __func__, path.string(), std::strerror(errno)));
    }
    auto dm = DirectoryMetadata{
        .name = path.filename(),
        .parent = parent,
        .uid = dir_stat.st_uid,
        .gid = dir_stat.st_gid,
        .mode = dir_stat.st_mode,
    };
    tbb::concurrent_hash_map<index_t, DirectoryMetadata>::accessor acc;
    if (incr_layer->modified_dir_index.insert(acc, index)) {
        acc->second = dm;
    } else [[unlikely]] {
        throw std::runtime_error(fmt::format("{}: Failed to insert modified directory index {}: "
                                             "index has exist.",
                                             __func__, index));
    }
}

void BackupTask::onModifiedFileMetadata(const index_t index, const index_t parent, const fs::path& path)
{
    struct stat file_stat;
    if (stat(path.c_str(), &file_stat) != 0) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to stat file '{}'. {}", __func__, path.string(), std::strerror(errno)));
    }
    auto fm = *(base_layer->file_index[index]);
    fm.name = path.filename();
    fm.parent = parent;
    fm.gid = file_stat.st_gid;
    fm.uid = file_stat.st_uid;
    fm.mode = file_stat.st_mode;
    tbb::concurrent_hash_map<index_t, FileMetadata>::accessor acc;
    if (incr_layer->modified_file_index.insert(acc, index)) {
        acc->second = fm;
    } else [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to insert modified file index {}: index has exist.", __func__, index));
    }
}

void BackupTask::onModifiedFileContent(const index_t index, const index_t parent, const fs::path& path)
{
    struct stat file_stat;
    if (stat(path.c_str(), &file_stat) != 0) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to stat file '{}'. {}", __func__, path.string(), std::strerror(errno)));
    }
    auto fm = FileMetadata{.size = CheckedFileSize(file_stat, path),
                           .mtime = fs::last_write_time(path),
                           .name = path.filename(),
                           .parent = parent,
                           .uid = file_stat.st_uid,
                           .gid = file_stat.st_gid,
                           .mode = file_stat.st_mode};
    calculate_chunks(path, fm.chunks);
    tbb::concurrent_hash_map<index_t, FileMetadata>::accessor acc;
    if (incr_layer->modified_file_index.insert(acc, index)) {
        acc->second = fm;
    } else [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to insert modified file index {}: index has exist.", __func__, index));
    }
}

std::shared_ptr<ArchiveBackupLayer> BackupTask::exec()
{
    incr_layer = std::make_shared<ArchiveBackupLayer>();
    incr_layer->id = layer_id;
    incr_layer->saved_path = layer_file;
    incr_layer->time = std::chrono::system_clock::now();

    // Create new chunk file
    auto tmp_pool_path = incr_layer->pool_path().concat(TEMP_FILE_SUFFIX);
    pool_writer = performance::presets::concurrent_writer::get_instance(tmp_pool_path); // @FileSystem Modification
    // Find all added, deleted and modified files and directories
    if (!base_layer || base_layer->empty()) {
        index_t root = onAddedDir(-1, src_dir);
        if (root != ROOT_DIR_INDEX) {
            throw std::runtime_error("Unexpected root directory index");
        }
    } else {
        vfs = std::make_unique<VirtualFS>(src_dir, base_layer, log_file);
        compare_directories_recursive(ROOT_DIR_INDEX, vfs->get_root(), src_dir, -1);
    }

    incr_layer->pool_size = pool_writer->size();
    pool_writer->flush();
    pool_writer.reset(); // flush @FileSystem Modification

    // Write index to file
    incr_layer->to_file();                              // @FileSystem Modification
    fs::rename(tmp_pool_path, incr_layer->pool_path()); // @FileSystem Modification

    return incr_layer;
}
