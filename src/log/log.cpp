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

#include "log/log.h"

#include <tbb/parallel_invoke.h>

#include <cstring>
#include <limits>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

VirtualFS::VirtualFS(const fs::path& src_dir, const std::shared_ptr<const VirtualBackupLayer> base_layer,
                     fs::path log_file)
{
    if (log_file.empty()) {
        struct stat dir_stat;
        if (stat(src_dir.c_str(), &dir_stat) != 0) [[unlikely]] {
            throw std::runtime_error(
                fmt::format("{}: Failed to stat directory '{}'. {}", __func__, src_dir.c_str(), std::strerror(errno)));
        }
        log_file = fmt::format("/data/log/{}.log", dir_stat.st_ino);
    }

    tbb::parallel_invoke([&]() { read_log_entry(log_file.string()); },
                         [&]() { backup_map = std::make_unique<BackupMap>(base_layer); });
    // If name represents relative path, then root's path should be empty
    root = std::make_shared<TreeNode>();
    for (const auto& entry : entries) {
        apply_log_entry(entry);
    }
}

namespace {
template <typename T> T ReadLogValue(std::istream& input)
{
    T value{};
    if (!input.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        throw std::runtime_error("Truncated operation log header");
    }
    return value;
}

std::string ReadLogPath(std::istream& input, uint16_t size)
{
    std::string path(size, '\0');
    if (!input.read(path.data(), size)) {
        throw std::runtime_error("Truncated operation log path");
    }
    return path;
}

template <typename T> void WriteLogValue(std::ostream& output, T value)
{
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
} // namespace

void WriteLogEntry(std::ostream& output, const LogEntry& entry)
{
    constexpr auto maxPathLength = std::numeric_limits<uint16_t>::max();
    if (entry.path.size() > maxPathLength || entry.oldPath.size() > maxPathLength) {
        throw std::length_error("Operation log path exceeds the 16-bit length field");
    }
    WriteLogValue(output, static_cast<uint8_t>(entry.type));
    WriteLogValue(output, static_cast<uint8_t>(entry.metadataOnly));
    WriteLogValue(output, static_cast<uint16_t>(entry.path.size()));
    WriteLogValue(output, static_cast<uint16_t>(entry.oldPath.size()));
    output.write(entry.path.data(), entry.path.size());
    output.write(entry.oldPath.data(), entry.oldPath.size());
    if (!output) {
        throw std::runtime_error("Failed to write operation log entry");
    }
}

void VirtualFS::read_log_entry(const std::string& log_entry_path)
{
    std::ifstream input(log_entry_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(fmt::format("Cannot open operation log {}", log_entry_path));
    }
    while (input.peek() != std::char_traits<char>::eof()) {
        const auto type = ReadLogValue<uint8_t>(input);
        const auto metadataOnly = ReadLogValue<uint8_t>(input);
        if (type > static_cast<uint8_t>(EventType::D_RENAME) || metadataOnly > 1) {
            throw std::runtime_error("Invalid operation log event type or metadata flag");
        }
        const auto pathSize = ReadLogValue<uint16_t>(input);
        const auto oldPathSize = ReadLogValue<uint16_t>(input);
        auto path = ReadLogPath(input, pathSize);
        auto oldPath = ReadLogPath(input, oldPathSize);
        entries.push_back({static_cast<EventType>(type), metadataOnly != 0, std::move(path), std::move(oldPath)});
    }
    if (input.bad()) {
        throw std::runtime_error("Failed to read operation log");
    }
}

std::shared_ptr<TreeNode> VirtualFS::resolve_path(const std::string& relative_path, bool is_create)
{
    std::stringstream ss(relative_path);
    std::string segment;
    auto current = root;

    while (std::getline(ss, segment, '/')) {
        if (segment.empty()) [[unlikely]] {
            continue;
        }

        if (!current->children.count(segment)) {
            auto newNode = std::make_shared<TreeNode>();
            newNode->name = segment;
            newNode->parent = current;
            current->children[segment].push_back(newNode);
        }
        current = current->children[segment].back();
    }
    /*
     * If an overwrite rename happens, the delete-old-file entry must have
     * appeared in the filesystem log.
     */
    if (current->deleted && is_create) [[unlikely]] {
        /*
         * Note that we needn't swap children here, for a new node is created
         * only when: 1. The old one is deleted and a new one with the same name
         * is created. 2. A rename happens, where the old node MUST be a file or
         * a DELETED dir.
         */
        auto newNode = std::make_shared<TreeNode>();
        newNode->name = segment;
        newNode->parent = current->parent;
        current->parent.lock()->children[segment].push_back(newNode);
        current = newNode;
    }
    return current;
}

/*
 * The TreeNode in the returned value means the last node that the oldPath
 * points to.
 */
std::tuple<index_t, fs::path, std::shared_ptr<TreeNode>> VirtualFS::resolve_old_path(
    const std::string& relative_path) const
{
    std::stringstream ss(relative_path);
    std::string segment;
    auto current = root;
    index_t last_ancestor_renamed_from = -1;
    fs::path last_ancestor_path;
    fs::path current_path;

    while (std::getline(ss, segment, '/')) {
        if (segment.empty()) [[unlikely]] {
            continue;
        }

        if (!current->children.count(segment)) {
            return {last_ancestor_renamed_from, last_ancestor_path, nullptr};
        }

        current = current->children[segment].back();
        current_path /= segment;
        last_ancestor_renamed_from = current->renamed_from == -1 ? last_ancestor_renamed_from : current->renamed_from;
        last_ancestor_path = current->renamed_from == -1 ? last_ancestor_path : current_path;
    }
    return {last_ancestor_renamed_from, last_ancestor_path, current};
}

void VirtualFS::RemoveMovedNode(const std::string& path, const std::shared_ptr<TreeNode>& node)
{
    std::stringstream input(path);
    std::string segment;
    auto current = root;
    while (std::getline(input, segment, '/')) {
        if (segment.empty()) {
            continue;
        }
        auto it = current->children.find(segment);
        if (it == current->children.end()) {
            throw std::runtime_error(fmt::format("Cannot find moved path {}", path));
        }
        if (!input.eof()) {
            current = it->second.back();
            continue;
        }
        auto& children = it->second;
        const auto removed = std::erase(children, node);
        if (removed != 1) {
            throw std::runtime_error(fmt::format("Cannot remove moved node at {}", path));
        }
    }
}

void VirtualFS::ApplyRename(const LogEntry& entry, const std::shared_ptr<TreeNode>& node)
{
    auto [ancestorIndex, ancestorPath, oldNode] = resolve_old_path(entry.oldPath);
    if (oldNode) {
        if (entry.type == EventType::D_RENAME) {
            node->children.swap(oldNode->children);
        } else {
            node->is_dir = false;
        }
        node->is_created = oldNode->is_created;
        node->metadataOnly = oldNode->metadataOnly;
        node->renamed_from = oldNode->renamed_from;
        RemoveMovedNode(entry.oldPath, oldNode);
    }
    if (node->renamed_from == static_cast<index_t>(-1) && !node->is_created) {
        node->renamed_from = backup_map->resolve_renamed_index(entry.oldPath, ancestorPath, ancestorIndex);
    }
}

void VirtualFS::ApplyFileEntry(const LogEntry& entry, const std::shared_ptr<TreeNode>& node)
{
    switch (entry.type) {
        case EventType::F_DELETE:
            node->is_dir = false;
            node->deleted = true;
            node->metadataOnly = false;
            break;
        case EventType::F_RENAME:
            ApplyRename(entry, node);
            [[fallthrough]];
        case EventType::F_MODIFY:
            if (node->is_dir) {
                node->is_dir = false;
                node->metadataOnly = true;
            }
            if (!entry.metadataOnly) {
                node->metadataOnly = false;
            }
            break;
        case EventType::F_CREATE:
            node->is_dir = false;
            node->is_created = true;
            break;
        default:
            throw std::runtime_error("Invalid file event type");
    }
}

void VirtualFS::ApplyDirectoryEntry(const LogEntry& entry, const std::shared_ptr<TreeNode>& node)
{
    switch (entry.type) {
        case EventType::D_DELETE:
            node->deleted = true;
            node->metadataOnly = false;
            break;
        case EventType::D_CREATE:
            node->is_created = true;
            break;
        case EventType::D_RENAME:
            ApplyRename(entry, node);
            [[fallthrough]];
        case EventType::D_MODIFY:
            node->metadataOnly = !node->is_created;
            break;
        default:
            throw std::runtime_error("Invalid directory event type");
    }
}

void VirtualFS::apply_log_entry(const LogEntry& entry)
{
    const bool isCreate = entry.type == EventType::F_CREATE || entry.type == EventType::D_CREATE ||
                          entry.type == EventType::F_RENAME || entry.type == EventType::D_RENAME;
    auto node = resolve_path(entry.path, isCreate);
    if (entry.type <= EventType::F_RENAME) {
        ApplyFileEntry(entry, node);
    } else {
        ApplyDirectoryEntry(entry, node);
    }
}

/*
 * This function should always return a valid index, or it will throw an
 * exception.
 */
index_t VirtualFS::BackupMap::resolve_renamed_index(const fs::path& path, const fs::path& last_ancestor_path,
                                                    const index_t last_ancestor_renamed_from)
{
    std::string err;
    decltype(VirtualBackupLayer::relative_path_index_map)::const_accessor path_acc;
    if (last_ancestor_renamed_from != -1) {
        auto unchanged_path = fs::relative(path, last_ancestor_path);
        tbb::concurrent_hash_map<index_t, fs::path>::const_accessor index_acc;
        if (base_layer->index_dirpath_map.find(index_acc, last_ancestor_renamed_from)) {
            if (base_layer->relative_path_index_map.find(path_acc,
                                                         fs::path(index_acc->second).append(unchanged_path.native()))) {
                return path_acc->second;
            } else [[unlikely]] {
                err = fmt::format("{}: Failed to find renamed path {}", __func__,
                                  (fs::path(index_acc->second).append(unchanged_path.native())).c_str());
                goto err_out;
            }
        } else [[unlikely]] {
            err = fmt::format("{}: Failed to find last_ancestor_renamed_from {}", __func__, last_ancestor_renamed_from);
            goto err_out;
        }
    } else if (base_layer->relative_path_index_map.find(path_acc, path)) {
        return path_acc->second;
    } else [[unlikely]] {
        err = fmt::format("{}: Failed to find path {}", __func__, path.c_str());
    }
err_out:
    throw std::runtime_error(err);
}
