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

#include <memory>
#include <atomic>
#include <numeric>
#include <queue>
#include <BackupTask.h>
#include <MergeTask.h>
#include <tbb/parallel_scan.h>
#include <writer/datamov.h>

template <typename T>
void update_entries(index_t start_index, index_t end_index, tbb::concurrent_vector<T>& vec,
                    tbb::concurrent_hash_map<index_t, T>& modified)
{
    tbb::concurrent_hash_map<index_t, T> not_applied_modified;
    tbb::parallel_for_each(modified.begin(), modified.end(), [&](const auto& kv) {
        if (kv.first >= start_index && kv.first < end_index) {
            vec[kv.first - start_index] = std::move(kv.second);
        } else {
            not_applied_modified.insert(std::move(kv));
        }
    });
    modified.swap(not_applied_modified);
}

// 标记被删除的文件和目录到 is_deleted
// 并把成功标记的下标从 del_list 中删除
// 不负责处理目录的子元素的参数。
// 参数：
//    vec: 要标记的目录或文件列表
//    del_list: 要删除的目录或文件下标列表。成功标记的下标会被从 del_list 中删除。
//    is_deleted: 标记结果
template <typename T>
void mark_deleted_entries(index_t start_index, index_t end_index, const tbb::concurrent_vector<T>& vec,
                          tbb::concurrent_vector<index_t>& del_list, tbb::concurrent_vector<bool>& is_deleted)
{
    tbb::concurrent_vector<index_t> not_applied_del;
    not_applied_del.reserve(del_list.size());
    is_deleted.resize(vec.size(), false);
    tbb::parallel_for_each(del_list.begin(), del_list.end(), [&](index_t i) {
        if (i >= start_index && i < end_index) {
            is_deleted[i - start_index] = true;
        } else {
            not_applied_del.push_back(i);
        }
    });
    not_applied_del.shrink_to_fit();
    del_list.swap(not_applied_del);
}

// 根据 del_list 删除 vec 中的元素，返回删除的数量以及旧下标到新下标的映射
template <typename T, typename D>
std::pair<index_t, std::vector<index_t>> prune_delete_entries(tbb::concurrent_vector<T>& vec, const D& is_deleted)
{
    std::vector<index_t> new_index(vec.size());
    index_t cursor = 0;
    for (index_t i = 0; i < vec.size(); ++i) {
        if (is_deleted[i]) {
            continue;
        }
        if (cursor != i) {
            vec[cursor] = std::move(vec[i]);
        }
        new_index[i] = cursor;
        ++cursor;
    }
    index_t deleted_num = vec.size() - cursor;
    vec.resize(cursor);
    return {deleted_num, new_index};
}

// 重新调整索引
template <typename Container, typename Acc>
void remap(Container& vec,
           const Acc& accessor, // 参数是 container 元素类型，返回值是 index_t& 的函数对象
           index_t start_index, index_t end_index, const std::vector<index_t>& new_index, index_t deleted_num)
{
    tbb::parallel_for_each(vec.begin(), vec.end(), [&](auto& entry) {
        if (accessor(entry) >= start_index && accessor(entry) < end_index) {
            accessor(entry) = new_index[accessor(entry) - start_index] + start_index;
        } else if (accessor(entry) >= end_index) {
            accessor(entry) -= deleted_num;
        }
    });
}

template <typename K, typename V>
void remap(tbb::concurrent_hash_map<K, V>& map, K start_index, K end_index, const std::vector<K>& new_index,
           K deleted_num)
{
    tbb::concurrent_hash_map<K, V> new_map;
    tbb::parallel_for_each(map.begin(), map.end(), [&](auto& kv) {
        if (kv.first >= start_index && kv.first < end_index) {
            new_map.insert({new_index[kv.first - start_index] + start_index, std::move(kv.second)});
        } else if (kv.first >= end_index) {
            new_map.insert({kv.first - deleted_num, std::move(kv.second)});
        } else {
            new_map.insert(std::move(kv));
        }
    });
    map.swap(new_map);
}

void update_files_dirs(std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, layer_id_t idx)
{
    index_t start_file_index = std::accumulate(
        layers.begin(), layers.begin() + idx, 0,
        [](index_t sum, const std::shared_ptr<ArchiveBackupLayer>& layer) { return sum + layer->file_index.size(); });
    index_t start_dir_index = std::accumulate(
        layers.begin(), layers.begin() + idx, 0,
        [](index_t sum, const std::shared_ptr<ArchiveBackupLayer>& layer) { return sum + layer->dir_index.size(); });

    index_t end_file_index = start_file_index + layers[idx]->file_index.size();
    index_t end_dir_index = start_dir_index + layers[idx]->dir_index.size();

    const std::shared_ptr<ArchiveBackupLayer>& layer = layers[idx];
    tbb::parallel_invoke(
        [&]() { update_entries(start_file_index, end_file_index, layer->file_index, layer->modified_file_index); },
        [&]() { update_entries(start_dir_index, end_dir_index, layer->dir_index, layer->modified_dir_index); });
}

struct MetadataRemap {
    index_t startFile;
    index_t endFile;
    index_t startDir;
    index_t endDir;
    index_t deletedFiles = 0;
    index_t deletedDirs = 0;
    std::vector<index_t> files;
    std::vector<index_t> dirs;
};

tbb::concurrent_vector<bool> DeletedDirectories(ArchiveBackupLayer& layer, index_t start, index_t end)
{
    tbb::concurrent_vector<bool> deleted;
    mark_deleted_entries(start, end, layer.dir_index, layer.del_dir_index, deleted);
    // Parents precede children, so directory deletion propagation stays serial.
    for (index_t index = 0; index < layer.dir_index.size(); ++index) {
        const index_t parent = layer.dir_index[index].parent;
        if (!deleted[index] && parent >= start && parent < end && deleted[parent - start]) {
            deleted[index] = true;
        }
    }
    return deleted;
}

tbb::concurrent_vector<bool> DeletedFiles(ArchiveBackupLayer& layer, const MetadataRemap& mapping,
                                          const tbb::concurrent_vector<bool>& directories)
{
    tbb::concurrent_vector<bool> deleted;
    mark_deleted_entries(mapping.startFile, mapping.endFile, layer.file_index, layer.del_file_index, deleted);
    tbb::parallel_for(tbb::blocked_range<index_t>(0, layer.file_index.size()), [&](const auto& range) {
        for (index_t index = range.begin(); index != range.end(); ++index) {
            const index_t parent = layer.file_index[index].parent;
            if (!deleted[index] && parent >= mapping.startDir && parent < mapping.endDir &&
                directories[parent - mapping.startDir]) {
                deleted[index] = true;
            }
        }
    });
    return deleted;
}

MetadataRemap PruneMetadata(ArchiveBackupLayer& layer, index_t startFile, index_t startDir)
{
    MetadataRemap mapping{startFile, static_cast<index_t>(startFile + layer.file_index.size()), startDir,
                          static_cast<index_t>(startDir + layer.dir_index.size())};
    const auto directories = DeletedDirectories(layer, mapping.startDir, mapping.endDir);
    const auto files = DeletedFiles(layer, mapping, directories);
    tbb::parallel_invoke(
        [&]() { std::tie(mapping.deletedFiles, mapping.files) = prune_delete_entries(layer.file_index, files); },
        [&]() { std::tie(mapping.deletedDirs, mapping.dirs) = prune_delete_entries(layer.dir_index, directories); });
    return mapping;
}

void RemapMetadata(ArchiveBackupLayer& layer, const MetadataRemap& mapping)
{
    tbb::parallel_invoke(
        [&]() {
            remap(
                layer.file_index, [](FileMetadata& file) -> index_t& { return file.parent; }, mapping.startDir,
                mapping.endDir, mapping.dirs, mapping.deletedDirs);
        },
        [&]() {
            remap(
                layer.dir_index, [](DirectoryMetadata& dir) -> index_t& { return dir.parent; }, mapping.startDir,
                mapping.endDir, mapping.dirs, mapping.deletedDirs);
        },
        [&]() {
            remap(
                layer.del_file_index, [](index_t& index) -> index_t& { return index; }, mapping.startFile,
                mapping.endFile, mapping.files, mapping.deletedFiles);
        },
        [&]() {
            remap(
                layer.del_dir_index, [](index_t& index) -> index_t& { return index; }, mapping.startDir, mapping.endDir,
                mapping.dirs, mapping.deletedDirs);
        },
        [&]() {
            remap(layer.modified_file_index, mapping.startFile, mapping.endFile, mapping.files, mapping.deletedFiles);
        },
        [&]() {
            remap(layer.modified_dir_index, mapping.startDir, mapping.endDir, mapping.dirs, mapping.deletedDirs);
        });
}

void prune_files_dirs(std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, layer_id_t idx)
{
    const index_t startFile =
        std::accumulate(layers.begin(), layers.begin() + idx, index_t{0},
                        [](index_t sum, const auto& layer) { return sum + layer->file_index.size(); });
    const index_t startDir =
        std::accumulate(layers.begin(), layers.begin() + idx, index_t{0},
                        [](index_t sum, const auto& layer) { return sum + layer->dir_index.size(); });
    const auto mapping = PruneMetadata(*layers[idx], startFile, startDir);
    tbb::parallel_for_each(layers.begin() + idx, layers.end(),
                           [&](const auto& layer) { RemapMetadata(*layer, mapping); });
}

struct ChunkRemap {
    index_t start;
    index_t end;
    index_t deleted = 0;
    std::vector<index_t> indices;
};

void MarkUsedChunks(const FileMetadata& file, const ChunkRemap& mapping, std::vector<std::atomic<bool>>& usedChunks)
{
    for (index_t index : file.chunks) {
        if (index >= mapping.start && index < mapping.end) {
            usedChunks[index - mapping.start].store(true, std::memory_order_relaxed);
        }
    }
}

std::vector<std::atomic<bool>> UsedChunks(const std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, index_t idx,
                                          const ChunkRemap& mapping)
{
    std::vector<std::atomic<bool>> usedChunks(layers[idx]->chunks.size());
    tbb::parallel_for_each(layers.begin() + idx, layers.end(), [&](const auto& layer) {
        tbb::parallel_for_each(layer->file_index.begin(), layer->file_index.end(),
                               [&](const FileMetadata& file) { MarkUsedChunks(file, mapping, usedChunks); });
    });
    return usedChunks;
}

class ChunkPruner {
    struct PendingChunk {
        Chunk chunk;
        index_t index;
        bool operator<(const PendingChunk& other) const
        {
            return chunk.offset.loffset > other.chunk.offset.loffset;
        }
    };
    tbb::concurrent_vector<Chunk>& chunks;
    const std::vector<std::atomic<bool>>& usedChunks;
    std::vector<index_t> indices;
    std::priority_queue<PendingChunk> pending;
    size_t offset = 0;
    index_t cursor = 0;
    index_t index = 0;

    void Retain(index_t original, const Chunk& chunk)
    {
        if (usedChunks[original].load(std::memory_order_relaxed)) {
            indices[original] = cursor;
            chunks[cursor++] = chunk;
        }
    }

    bool DrainPending()
    {
        if (pending.empty() || pending.top().chunk.offset.loffset != offset) {
            return false;
        }
        const auto& next = pending.top();
        offset += next.chunk.physical_size;
        Retain(next.index, next.chunk);
        pending.pop();
        return true;
    }

    void ProcessCurrent()
    {
        if (index >= chunks.size()) {
            throw std::runtime_error("Chunk addresses are not continuous");
        }
        if (chunks[index].offset.loffset == 0) {
            offset = 0;
        }
        if (chunks[index].offset.loffset == offset) {
            offset += chunks[index].physical_size;
            Retain(index, chunks[index]);
        } else {
            pending.push({chunks[index], index});
        }
        ++index;
    }

public:
    ChunkPruner(tbb::concurrent_vector<Chunk>& chunks, const std::vector<std::atomic<bool>>& usedChunks)
        : chunks(chunks), usedChunks(usedChunks), indices(chunks.size())
    {
    }

    std::vector<index_t> Run()
    {
        while (index < chunks.size() || !pending.empty()) {
            if (!DrainPending()) {
                ProcessCurrent();
            }
        }
        chunks.resize(cursor);
        return std::move(indices);
    }
};

void RemapChunkReferences(ArchiveBackupLayer& layer, const ChunkRemap& mapping)
{
    const auto remapFile = [&mapping](FileMetadata& file) {
        remap(
            file.chunks, [](index_t& index) -> index_t& { return index; }, mapping.start, mapping.end, mapping.indices,
            mapping.deleted);
    };
    tbb::parallel_invoke(
        [&layer, &remapFile]() { tbb::parallel_for_each(layer.file_index.begin(), layer.file_index.end(), remapFile); },
        [&layer, &remapFile]() {
            tbb::parallel_for_each(layer.modified_file_index.begin(), layer.modified_file_index.end(),
                                   [&remapFile](auto& entry) { remapFile(entry.second); });
        });
}

void prune_chunks(std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, index_t idx)
{
    const index_t start = std::accumulate(layers.begin(), layers.begin() + idx, index_t{0},
                                          [](index_t sum, const auto& layer) { return sum + layer->chunks.size(); });
    const auto originalSize = layers[idx]->chunks.size();
    ChunkRemap mapping{start, static_cast<index_t>(start + originalSize)};
    const auto usedChunks = UsedChunks(layers, idx, mapping);
    mapping.indices = ChunkPruner(layers[idx]->chunks, usedChunks).Run();
    mapping.deleted = originalSize - layers[idx]->chunks.size();
    tbb::parallel_for_each(layers.begin() + idx, layers.end(),
                           [&](const auto& layer) { RemapChunkReferences(*layer, mapping); });
}

// Concatenate chunks still referenced by some file, discard the others
void concatenate_chunks(const std::unique_ptr<ConcurrentWriter>& writer, tbb::concurrent_vector<Chunk>& chunks,
                        const std::vector<std::shared_ptr<ArchiveBackupLayer>>& to_merge_layers,
                        std::shared_ptr<ArchiveBackupLayer> merged_layer)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, chunks.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
            Chunk& chunk = chunks[i];
            // The first layer is not in to_merge_layers, it's the base of
            // merged_layer, and the other layers are stacked on it, so we
            // have to retract chunks of the first layer from merged_layer
            const auto& chunk_layer = chunk.offset.layer_id == merged_layer->id
                                          ? merged_layer
                                          : to_merge_layers[chunk.offset.layer_id - merged_layer->id - 1];
            if (!(chunk.offset.loffset + chunk.physical_size <= chunk_layer->pool_size)) [[unlikely]] {
                throw std::runtime_error(fmt::format("{}: Chunk exceeds pool size.", __func__));
            }
            // Maybe we can use zero copy here? However sendfile doesn't
            // work in multithreaded environment, splice is possible, but
            // one of the fd must be a pipe. Mmap may also be one of the
            // solutions, but remember to handle any alignment issue.
            uint8_t chunk_content[chunk.physical_size];
            datamov::pread(chunk_layer->pool_fd, chunk_content, chunk.physical_size, chunk.offset.loffset);
            chunk.offset.loffset = writer->write(chunk_content, chunk.physical_size); // @FileSystem Modification
            chunk.offset.layer_id = merged_layer->id;
        }
    });
}

// Merge the pool files and remove obsolete chunks
void cat_prune_pool(const std::shared_ptr<ArchiveBackupLayer>& merged_layer,
                    const std::vector<std::shared_ptr<ArchiveBackupLayer>>& to_merge_layers)
{
    for (const std::shared_ptr<ArchiveBackupLayer>& tmlayer : to_merge_layers) {
        tmlayer->open_pool();
    }
    merged_layer->open_pool();

    // Create a new file to store the merged pool temporarily, and replace it
    // later
    fs::path merge_path = merged_layer->pool_path().concat(TEMP_FILE_SUFFIX);
    auto writer = performance::presets::concurrent_writer::get_instance(merge_path); // @FileSystem Modification

    // Compute the total size of the merged pool and the starting offset of each
    // chunk, which can be done in parallel

    // Concatenate the chunks according to the offset, it can be done in
    // parallel
    concatenate_chunks(writer, merged_layer->chunks, to_merge_layers, merged_layer);
    merged_layer->pool_size = writer->size();
    writer->flush();
    writer.reset(); // flush @FileSystem Modification
    // Remove the original pool file first and rename the temporary pool file.
    // We have to remove the original file first, otherwise rename will take a
    // long time.
    merged_layer->close_pool();                // @FileSystem Modification
    unlink(merged_layer->pool_path().c_str()); // @FileSystem Modification

    // If error occurs, the temporary pool file will not be removed, however one
    // layer's pool file is removed. We need to recover it with the temporary
    // pool file.
    if (rename(merge_path.c_str(), merged_layer->pool_path().c_str()) != 0) // @FileSystem Modification
        [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to rename temporary pool file. {}", __func__, strerror(errno)));
    }

    for (const std::shared_ptr<ArchiveBackupLayer>& tmlayer : to_merge_layers) {
        tmlayer->close_pool(); // @FileSystem Modification
    }
}
