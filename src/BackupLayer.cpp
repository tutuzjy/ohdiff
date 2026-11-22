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
#include <fcntl.h>
#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stack>

void ArchiveBackupLayer::open_pool()
{
    if (pool_fd == -1) {
        pool_fd = open(pool_path().c_str(), O_RDONLY);
        if (pool_fd == -1) [[unlikely]] {
            throw std::runtime_error(fmt::format("{}: Failed to open pool file. {}", __func__, strerror(errno)));
        }
    }
}

void ArchiveBackupLayer::mmap_pool()
{
    if (pool_size == 0) {
        pool_data = nullptr;
        return;
    }

    if (pool_fd == -1) {
        open_pool();
    }

    if (pool_data == nullptr) {
        struct stat pool_file_info;
        if (fstat(pool_fd, &pool_file_info) == -1) [[unlikely]] {
            close(pool_fd);
            throw std::runtime_error(fmt::format("{}: Failed to get pool file size. {}", __func__, strerror(errno)));
        }

        if (pool_file_info.st_size != pool_size) [[unlikely]] { // sanity check
            throw std::runtime_error(fmt::format("{}: Pool file size mismatch", __func__));
        }

        pool_data = static_cast<uint8_t*>(mmap(nullptr, pool_size, PROT_READ, MAP_SHARED, pool_fd, 0));
        if (pool_data == MAP_FAILED) [[unlikely]] {
            throw std::runtime_error(fmt::format("{}: Failed to map pool file. {}", __func__, strerror(errno)));
        }
    }
}

void ArchiveBackupLayer::close_pool()
{
    if (pool_data != nullptr) {
        munmap(pool_data, pool_size);
    }
    if (pool_fd != -1) {
        close(pool_fd);
    }
    pool_data = nullptr;
    pool_fd = -1;
}

ArchiveBackupLayer::~ArchiveBackupLayer()
{
    close_pool();
}

// ================================================

namespace {
template <typename Indices> std::vector<char> DeletedFlags(size_t size, const Indices& indices)
{
    std::vector<char> flags(size, 0);
    // Duplicate deletion indices may occur, so mark them without concurrent writes.
    for (index_t index : indices) {
        if (index >= size) {
            throw std::runtime_error("Deleted metadata index exceeds the layer");
        }
        flags[index] = 1;
    }
    return flags;
}
} // namespace

fs::path VirtualBackupLayer::BuildParentPath(index_t parent)
{
    std::vector<index_t> missing;
    fs::path path;
    for (size_t depth = 0; parent != ROOT_DIR_INDEX; ++depth) {
        if (parent >= dir_index.size() || depth >= dir_index.size()) {
            throw std::runtime_error("Invalid directory parent chain");
        }
        decltype(index_dirpath_map)::const_accessor accessor;
        if (index_dirpath_map.find(accessor, parent)) {
            path = accessor->second;
            break;
        }
        missing.push_back(parent);
        parent = dir_index[parent]->parent;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        path /= dir_index[*it]->name;
        index_dirpath_map.emplace(*it, path);
        relative_path_index_map.emplace(path, *it);
    }
    return path;
}

void VirtualBackupLayer::IndexDirectory(index_t index)
{
    const auto parent = dir_index[index]->parent;
    if (parent >= subdirs.size()) {
        throw std::runtime_error("Directory parent index exceeds the layer");
    }
    subdirs[parent].push_back(index);
    fs::path path = BuildParentPath(parent) / dir_index[index]->name;
    index_dirpath_map.emplace(index, path);
    relative_path_index_map.emplace(path, index);
}

void VirtualBackupLayer::IndexFile(index_t index)
{
    const auto parent = file_index[index]->parent;
    if (parent >= subfiles.size()) {
        throw std::runtime_error("File parent index exceeds the layer");
    }
    subfiles[parent].push_back(index);
    decltype(index_dirpath_map)::const_accessor accessor;
    if (index_dirpath_map.find(accessor, parent)) {
        relative_path_index_map.emplace(accessor->second / file_index[index]->name, index);
    } else if (parent == ROOT_DIR_INDEX) {
        relative_path_index_map.emplace(file_index[index]->name, index);
    } else {
        throw std::runtime_error(fmt::format("Cannot find parent directory {}", parent));
    }
}

void VirtualBackupLayer::reconstruct()
{
    const auto fileDeleted = DeletedFlags(file_index.size(), del_file_index);
    const auto dirDeleted = DeletedFlags(dir_index.size(), del_dir_index);
    subdirs.clear();
    subfiles.clear();
    index_dirpath_map.clear();
    relative_path_index_map.clear();
    subdirs.resize(dir_index.size());
    subfiles.resize(dir_index.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(1, dir_index.size()), [&](const auto& range) {
        for (size_t index = range.begin(); index != range.end(); ++index) {
            if (!dirDeleted[index]) {
                IndexDirectory(index);
            }
        }
    });
    tbb::parallel_for(tbb::blocked_range<size_t>(0, file_index.size()), [&](const auto& range) {
        for (size_t index = range.begin(); index != range.end(); ++index) {
            if (!fileDeleted[index]) {
                IndexFile(index);
            }
        }
    });
}

VirtualBackupLayer::FindResult VirtualBackupLayer::find(const fs::path& path) const
{
    if (empty()) {
        return FindResult();
    }
    fs::path normal = path.lexically_normal();
    index_t idx = ROOT_DIR_INDEX;
    bool is_dir = true;

    for (const fs::path& part : normal) {
        if (normal.is_absolute() && part == normal.root_path() || part.empty()) {
            continue;
        }
        if (!is_dir) {
            return FindResult(); // 如果已经找到文件，但还有 part，说明路径不完整
        }

        auto it = std::find_if(subdirs[idx].begin(), subdirs[idx].end(),
                               [&](index_t i) { return dir_index[i]->name == part; });
        if (it != subdirs[idx].end()) {
            idx = *it;
            continue;
        }

        it = std::find_if(subfiles[idx].begin(), subfiles[idx].end(),
                          [&](index_t i) { return file_index[i]->name == part; });
        if (it != subfiles[idx].end()) {
            is_dir = false;
            idx = *it;
            continue;
        }

        return FindResult();
    }

    FindResult result;
    result.found = true;
    result.is_dir = is_dir;
    result.index = idx;
    return result;
}

fs::path VirtualBackupLayer::get_dir_path(index_t dir_i) const
{
    fs::path path("");
    for (index_t i = dir_i; i != ROOT_DIR_INDEX; i = dir_index[i]->parent) {
        path = dir_index[i]->name / path;
    }
    return path;
}

fs::path VirtualBackupLayer::get_file_path(index_t file_i) const
{
    return get_dir_path(file_index[file_i]->parent) / file_index[file_i]->name;
}
