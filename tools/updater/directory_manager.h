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

#ifndef DIRECTORY_MANAGER_H
#define DIRECTORY_MANAGER_H

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

enum class OperationType {
    CREATE_FILE,
    CREATE_DIR,
    DELETE_FILE,
    DELETE_DIR,
    MODIFY_CONTENT,
    RENAME,
    // UPDATE_METADATA,
    // MOVE,
    OPERATION_COUNT
};

struct Node {
    std::string name;
    fs::path relativePath;
    bool isDirectory = false;
    uid_t uid = 0;
    gid_t gid = 0;
    mode_t mode = 0;
    Node* parent;
    size_t totalChildren;
    std::map<std::string, Node> children;

    Node() : parent(nullptr), totalChildren(0) {}
};

class DirectoryManager {
public:
    DirectoryManager(const std::string& path, const std::string& logPath);
    void SetXPercent(float upper, float lower);
    void GenerateRandomOperation();
    inline bool IsDone() const
    {
        return modifiedCount >= static_cast<size_t>(totalNodes * (xPercentLower / percentScale));
    }

private:
    static constexpr float percentScale = 100.0F;
    static constexpr size_t defaultNameLength = 8;
    Node root;
    fs::path rootPath;
    size_t totalNodes = 0;
    size_t totalBytes = 0;
    size_t modifiedCount;
    float xPercentUpper = 0;
    float xPercentLower = 0;
    std::vector<Node*> dirPtrs;
    std::vector<Node*> filePtrs;
    std::mt19937 rng;
    std::ofstream log;

    void ScanDirectory(Node* node, const fs::path& path);
    void UpdateParentsTotal(Node* node, ssize_t delta);
    std::string GenerateRandomName(size_t length = defaultNameLength);
    Node* GetRandomExistingNode(bool requireDirectory);
    Node** GetRandomExistingFile();
    inline bool CheckModificationQuota(size_t delta)
    {
        const size_t allowed = static_cast<size_t>(totalNodes * (xPercentUpper / percentScale));
        return delta <= allowed && modifiedCount <= allowed - delta;
    }
    void CreateRandomNode(bool isDirectory);
    void DeleteRandomNode(bool isDirectory);
    void RenameRandomFile();
    Node NewNode(Node* parent, const std::string& name, bool isDirectory, const fs::path& fullPath);
    bool CreateNode(Node* parent, const std::string& name, bool isDirectory);
    bool DeleteNode(Node* node);
    bool ModifyContent(Node* node);
    bool RenameFile(Node** node, Node* newParent, const std::string& newName);
};

#endif // DIRECTORY_MANAGER_H
