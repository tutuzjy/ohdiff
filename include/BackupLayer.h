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

#include <type_traits>
#include <vector>
#include <checksum/checksum.h>
#include <common/defs.h>
#include <common/serialization/concurrent_hash_map.h>
#include <common/serialization/concurrent_vector.h>
#include <common/serialization/file_time_type.h>
#include <common/serialization/xxhash_128.h>
#include <common/utils.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_invoke.h>
#include <tbb/task_group.h>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>

namespace fs = std::filesystem;
/*
 * Assume only 4 billion files/dirs/chunks.
 * The last one (i.e. 2^32 - 1) CAN NOT be used as a valid index.
 */
using index_t = uint32_t;
const index_t ROOT_DIR_INDEX = 0;

struct FileMetadata {
    size_t size;                 // [Serialized] File size.
    fs::file_time_type mtime;    // [Serialized] Last modified time.
    std::string name;            // [Serialized] File name.
    index_t parent;              // [Serialized] Parent directory index.
    std::vector<index_t> chunks; // [Serialized] Chunk indices.
                                 // Use non-concurrent vector to save memory.
    uid_t uid = 0;               // [Serialized] User ID.
    gid_t gid = 0;               // [Serialized] Group ID.
    mode_t mode = 0;             // [Serialized] File mode.

    // for serialization
    template <class Archive> void serialize(Archive& ar, const unsigned int version)
    {
        ar & name;
        ar & parent;
        ar & size;
        ar & mtime;
        ar & chunks;
        ar & uid;
        ar & gid;
        ar & mode;
    }
};

struct DirectoryMetadata {
    std::string name; // [Serialized] Directory name.
    index_t parent;   // [Serialized] Parent directory index.
    uid_t uid = 0;    // [Serialized] User ID.
    gid_t gid = 0;    // [Serialized] Group ID.
    mode_t mode = 0;  // [Serialized] Directory mode.

    // for serialization
    template <class Archive> void serialize(Archive& ar, const unsigned int version)
    {
        ar & name;
        ar & parent;
        ar & uid;
        ar & gid;
        ar & mode;
    }
};

struct chunk_offset_t {
    static_assert(sizeof(layer_id_t) == 2);
    layer_id_t layer_id : 16; // [Serialized] Chunk's layer ID.
    size_t loffset : 48;      // [Serialized] Chunk local offset in backup.

    // for serialization
    template <class Archive> void serialize(Archive& ar, const unsigned int version)
    {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(this);
        ar&* ptr;
    }
};

struct Chunk {
    static const char COMPRESSED = 1;
    static const char UNCOMPRESSED = 0;
    static const char INTENTIONALLY_UNCOMPRESSED =
        -1; // 对于压缩之后反而变大的 chunk，不压缩，并标记为 INTENTIONALLY_UNCOMPRESSED

    StrongChecksum checksum;        // [Serialized] Chunk checksum.
    chunk_offset_t offset;          // [Serialized] Chunk offset in pool.
    chunk_size_t physical_size;     // [Serialized] Physical chunk size in pool.
    char compressed = UNCOMPRESSED; // [Serialized] Whether the chunk is compressed.

    // for serialization
    template <class Archive> void serialize(Archive& ar, const unsigned int version)
    {
        ar & offset;
        ar & physical_size;
        ar & checksum;
        ar & compressed;
    }

    bool operator==(const Chunk& other) const
    {
        return checksum == other.checksum;
    }
};

class ArchiveBackupLayer {
    const static uint64_t MAGIC_NUMBER = 0x9ed654a41477b8dull;

public:
    // Layer description
    std::chrono::system_clock::time_point time; // [Serialized] Layer timestamp.
    index_t id;                                 // [Serialized] Layer ID.

    // Backup index =====================================================
    tbb::concurrent_vector<FileMetadata> file_index;                         // [Serialized] All files.
    tbb::concurrent_vector<DirectoryMetadata> dir_index;                     // [Serialized] All directories.
    tbb::concurrent_vector<Chunk> chunks;                                    // [Serialized] All chunks.
    tbb::concurrent_vector<index_t> del_file_index;                          // [Serialized] All deleted files.
    tbb::concurrent_vector<index_t> del_dir_index;                           // [Serialized] All deleted directories.
    tbb::concurrent_hash_map<index_t, FileMetadata> modified_file_index;     // [Serialized] All modified files.
    tbb::concurrent_hash_map<index_t, DirectoryMetadata> modified_dir_index; // [Serialized] All modified directories.
    // ===================================================================

    uint64_t magic_number = MAGIC_NUMBER; // [Serialized] Magic number.
    size_t pool_size;                     // [Serialized] Chunks pool size.
    fs::path saved_path;                  // [Non-serialized] Archive path.

    // for serialization
    template <class Archive> void serialize(Archive& ar, const unsigned int version)
    {
        ar & time;
        ar & file_index;
        ar & dir_index;
        ar & chunks;
        ar & del_file_index;
        ar & del_dir_index;
        ar & modified_file_index;
        ar & modified_dir_index;
        ar & pool_size;
        ar & magic_number;
        ar & id;
    }

    void to_file(const fs::path& path)
    {
        std::ofstream ofs(path);
        boost::archive::binary_oarchive oa(ofs);
        oa << *this;
        saved_path = path;
    }
    void to_file()
    {
        if (saved_path.empty()) [[unlikely]] {
            throw std::runtime_error(fmt::format("{}: Layer do not have a saved path", __func__));
        }
        to_file(saved_path);
    }

    void from_file(const fs::path& path)
    {
        std::ifstream ifs(path);
        boost::archive::binary_iarchive ia(ifs);
        ia >> *this;
        saved_path = path;
        if (magic_number != MAGIC_NUMBER) [[unlikely]] {
            throw std::runtime_error(fmt::format("{}: Invalid magic number", __func__));
        }
    }

    inline bool empty() const
    {
        return dir_index.empty();
    }

    inline fs::path pool_path() const
    {
        if (saved_path.empty()) [[unlikely]] {
            throw std::runtime_error(fmt::format("{}: Layer do not have a saved path", __func__));
        }
        fs::path pool_path = saved_path;
        pool_path.replace_extension(".data");
        return pool_path;
    }

    uint8_t* pool_data = nullptr; // [Non-serialized]
    int pool_fd = -1;             // [Non-serialized]
    void open_pool();
    void mmap_pool();
    void close_pool();

    ~ArchiveBackupLayer();
};

class VirtualBackupLayer {
public:
    // Backup index ======================================
    /*
     * These two index vectors below will store the newest metadata (before
     * being deleted if the one has been deleted) after merge.
     */
    std::vector<const FileMetadata*> file_index;
    std::vector<const DirectoryMetadata*> dir_index;

    std::vector<const Chunk*> chunks;
    std::vector<index_t> del_file_index;
    std::vector<index_t> del_dir_index;

    std::vector<tbb::concurrent_vector<index_t>> subfiles;
    std::vector<tbb::concurrent_vector<index_t>> subdirs;

    struct PathHashCompare {
        static size_t hash(const fs::path& path)
        {
            return fs::hash_value(path);
        }

        static bool equal(const fs::path& lhs, const fs::path& rhs)
        {
            return lhs == rhs;
        }
    };
    tbb::concurrent_hash_map<fs::path, index_t, PathHashCompare> relative_path_index_map;
    tbb::concurrent_hash_map<index_t, fs::path> index_dirpath_map;

    inline bool empty() const
    {
        return dir_index.empty();
    }

    // Find a file or directory by path
    struct FindResult {
        bool found;
        bool is_dir;
        index_t index;
    };
    FindResult find(const fs::path& path) const;

    void reconstruct();

    // Get path of a directory, relative to the backup root
    // Note: low performance, do not use in performance-critical code
    fs::path get_dir_path(index_t dir_i) const;

    // Get path of a file, relative to the backup root
    // Note: low performance, do not use in performance-critical code
    fs::path get_file_path(index_t file_i) const;

private:
    fs::path BuildParentPath(index_t parent);
    void IndexDirectory(index_t index);
    void IndexFile(index_t index);
};

namespace BackupLayer {

template <typename R, typename Destination, typename Select>
void CopyEntries(const R& layers, Destination& destination, Select select)
{
    std::vector<size_t> offsets;
    size_t size = destination.size();
    for (const auto& layer : layers) {
        offsets.push_back(size);
        size += select(*layer).size();
    }
    destination.resize(size);
    tbb::parallel_for(size_t{0}, offsets.size(), [&](size_t index) {
        const auto& source = select(*layers[index]);
        std::transform(source.begin(), source.end(), destination.begin() + offsets[index],
                       [](const auto& entry) -> typename Destination::value_type {
                           if constexpr (std::is_pointer_v<typename Destination::value_type>) {
                               return &entry;
                           } else {
                               return entry;
                           }
                       });
    });
}

template <typename R, typename Target> void CopyLayerData(const R& layers, Target& target)
{
    tbb::parallel_invoke(
        [&]() {
            CopyEntries(layers, target.file_index, [](const auto& layer) -> const auto& { return layer.file_index; });
        },
        [&]() {
            CopyEntries(layers, target.dir_index, [](const auto& layer) -> const auto& { return layer.dir_index; });
        },
        [&]() { CopyEntries(layers, target.chunks, [](const auto& layer) -> const auto& { return layer.chunks; }); },
        [&]() {
            CopyEntries(layers, target.del_file_index,
                        [](const auto& layer) -> const auto& { return layer.del_file_index; });
        },
        [&]() {
            CopyEntries(layers, target.del_dir_index,
                        [](const auto& layer) -> const auto& { return layer.del_dir_index; });
        });
}

template <typename Source, typename Destination> void ApplyOverrides(const Source& source, Destination& destination)
{
    tbb::parallel_for_each(source.begin(), source.end(), [&](const auto& entry) {
        if constexpr (std::is_pointer_v<typename Destination::value_type>) {
            if (entry.first >= destination.size()) {
                throw std::runtime_error("Modified metadata index exceeds the merged layer");
            }
            destination[entry.first] = &entry.second;
        } else {
            typename Destination::accessor accessor;
            destination.insert(accessor, entry.first);
            accessor->second = entry.second;
        }
    });
}

template <typename Target> void ApplyLayerOverrides(const ArchiveBackupLayer& source, Target& target)
{
    if constexpr (std::is_same_v<Target, ArchiveBackupLayer>) {
        tbb::parallel_invoke([&]() { ApplyOverrides(source.modified_file_index, target.modified_file_index); },
                             [&]() { ApplyOverrides(source.modified_dir_index, target.modified_dir_index); });
    } else {
        tbb::parallel_invoke([&]() { ApplyOverrides(source.modified_file_index, target.file_index); },
                             [&]() { ApplyOverrides(source.modified_dir_index, target.dir_index); });
    }
}

template <typename target_layer_t, Range<std::shared_ptr<ArchiveBackupLayer>> R>
void merge_to(const R& layers, std::shared_ptr<target_layer_t> merged_layer)
{
    if (layers.empty()) {
        return;
    }
    CopyLayerData(layers, *merged_layer);
    // Apply layers in chronological order so the newest metadata wins deterministically.
    for (const auto& layer : layers) {
        ApplyLayerOverrides(*layer, *merged_layer);
        if constexpr (std::is_same_v<target_layer_t, ArchiveBackupLayer>) {
            merged_layer->time = layer->time;
            merged_layer->pool_size += layer->pool_size;
        }
    }
}

template <typename target_layer_t, Range<std::shared_ptr<ArchiveBackupLayer>> R>
std::shared_ptr<target_layer_t> merge(const R& layers)
{
    auto merged_layer = std::make_shared<target_layer_t>();
    merge_to<target_layer_t, R>(layers, merged_layer);
    return merged_layer;
}

} // namespace BackupLayer
