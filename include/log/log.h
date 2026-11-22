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
#include <map>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "BackupLayer.h"

enum class EventType : uint8_t {
    F_CREATE,
    F_MODIFY,
    F_DELETE,
    F_RENAME,
    D_CREATE,
    D_MODIFY, // only for metadata
    D_DELETE,
    D_RENAME
};

struct LogEntry {
    EventType type;
    bool metadataOnly;
    std::string path;
    std::string oldPath; // RENAME ONLY
};

void WriteLogEntry(std::ostream& output, const LogEntry& entry);

class TreeNode {
    friend class VirtualFS;

public:
    std::string name;
    /*
     * The index where the new file/dir is renamed from.
     * Set it to -1 if the node is not renamed or the actual node is
     * newly created.
     */
    index_t renamed_from = -1;
    bool is_dir = true;
    bool deleted = false;
    /*
     * For file: only check this variable can we decide whether it is just a
     * metadata update.
     * For Directory: we still need to check whether children
     * is empty.
     */
    bool metadataOnly = false;
    bool is_created = false;

    // Return the child lookup without copying the map.
    const std::unordered_map<std::string, std::vector<std::shared_ptr<TreeNode>>>& get_children() const
    {
        return children;
    }

private:
    /*
     * Why we use vector here?
     * A file will be deleted and re-created using the same name, and there
     * exist file overwrite when we consider rename.
     */
    std::unordered_map<std::string, std::vector<std::shared_ptr<TreeNode>>> children;
    std::weak_ptr<TreeNode> parent;
};

class VirtualFS {
private:
    std::shared_ptr<TreeNode> root;
    std::vector<LogEntry> entries;

    void read_log_entry(const std::string& log_entry_path);
    std::shared_ptr<TreeNode> resolve_path(const std::string& relative_path, bool is_create);
    std::tuple<index_t, fs::path, std::shared_ptr<TreeNode>> resolve_old_path(const std::string& relative_path) const;
    void apply_log_entry(const LogEntry& entry);
    void RemoveMovedNode(const std::string& path, const std::shared_ptr<TreeNode>& node);
    void ApplyRename(const LogEntry& entry, const std::shared_ptr<TreeNode>& node);
    void ApplyFileEntry(const LogEntry& entry, const std::shared_ptr<TreeNode>& node);
    void ApplyDirectoryEntry(const LogEntry& entry, const std::shared_ptr<TreeNode>& node);

    class BackupMap {
    private:
        const std::shared_ptr<const VirtualBackupLayer> base_layer;

    public:
        explicit BackupMap(const std::shared_ptr<const VirtualBackupLayer> base_layer) : base_layer(base_layer) {}
        index_t resolve_renamed_index(const fs::path& path, const fs::path& last_ancestor_path,
                                      const index_t last_ancestor_renamed_from);
        index_t resolve_index(const fs::path& path)
        {
            decltype(VirtualBackupLayer::relative_path_index_map)::const_accessor acc;
            if (base_layer->relative_path_index_map.find(acc, path)) {
                return acc->second;
            } else {
                return -1;
            }
        }
    };
    std::unique_ptr<BackupMap> backup_map;

public:
    VirtualFS(const fs::path& src_dir, const std::shared_ptr<const VirtualBackupLayer> base_layer,
              fs::path log_file = {});
    const std::shared_ptr<const TreeNode> get_root()
    {
        return root;
    }

    /*
     * This function will only be used to get exist file/dir index.
     * It should resolve files/dirs that in renamed path correctly.
     */
    const index_t get_index(const fs::path& path) const
    {
        auto index = backup_map->resolve_index(path);
        if (index != -1) {
            return index;
        }
        auto [last_ancestor_renamed_from, last_ancestor_path, old_node] = resolve_old_path(path);
        if (old_node && old_node->renamed_from != -1) {
            return old_node->renamed_from;
        } else {
            return backup_map->resolve_renamed_index(path, last_ancestor_path, last_ancestor_renamed_from);
        }
    }
};
