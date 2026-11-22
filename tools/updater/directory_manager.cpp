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

#include <string_view>
#include "directory_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "log_writer.h"

DirectoryManager::DirectoryManager(const std::string& path, const std::string& logPath)
    : rootPath(fs::canonical(path)), modifiedCount(0), rng(std::random_device{}())
{
    root.name = "";
    root.relativePath = "";
    root.isDirectory = true;
    struct stat statBuf;
    if (stat(path.c_str(), &statBuf) == 0) {
        root.uid = statBuf.st_uid;
        root.gid = statBuf.st_gid;
        root.mode = statBuf.st_mode;
    } else [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to get file status for {}. {}", __func__, rootPath.c_str(), std::strerror(errno)));
    }
    dirPtrs.push_back(&root);
    ScanDirectory(&root, rootPath);
    totalNodes = root.totalChildren + 1;
    if (totalNodes != dirPtrs.size() + filePtrs.size()) {
        throw std::runtime_error("Directory scan produced inconsistent node counts");
    }

    if (!logPath.empty()) {
        log = std::ofstream(logPath, std::ios::binary | std::ios::app);
    }
}

void DirectoryManager::SetXPercent(float upper, float lower)
{
    if (!std::isfinite(upper) || !std::isfinite(lower) || lower < 0 || lower > upper || upper > percentScale) {
        throw std::invalid_argument("Modification percentages must satisfy 0 <= lower <= upper <= 100");
    }
    xPercentUpper = upper;
    xPercentLower = lower;
}

void DirectoryManager::CreateRandomNode(bool isDirectory)
{
    Node* parent = GetRandomExistingNode(true);
    if (!parent) {
        throw std::runtime_error("No parent found for creation");
    }
    std::string newName = GenerateRandomName();
    while (parent->children.contains(newName)) {
        newName = GenerateRandomName();
    }
    CreateNode(parent, newName, isDirectory);
}

void DirectoryManager::DeleteRandomNode(bool isDirectory)
{
    constexpr int maxDeleteAttempts = 10;
    Node* target = GetRandomExistingNode(isDirectory);
    for (int attempt = 0; attempt < maxDeleteAttempts && target == &root; ++attempt) {
        target = GetRandomExistingNode(isDirectory);
    }
    if (target && target != &root) {
        DeleteNode(target);
    }
}

void DirectoryManager::RenameRandomFile()
{
    Node** target = GetRandomExistingFile();
    if (!target) {
        return;
    }
    Node* newParent = GetRandomExistingNode(true);
    if (!newParent) {
        throw std::runtime_error("No parent found for renaming");
    }
    std::string newName = GenerateRandomName();
    while (newParent->children.contains(newName) &&
           (newParent->children.at(newName).isDirectory || (*target)->parent == newParent)) {
        newName = GenerateRandomName();
    }
    RenameFile(target, newParent, newName);
}

void DirectoryManager::GenerateRandomOperation()
{
    // Relative weights for create file/dir, delete file/dir, modify, and rename.
    constexpr std::array<int, static_cast<size_t>(OperationType::OPERATION_COUNT)> WEIGHTS{20, 5, 15, 5, 20, 35};
    std::discrete_distribution<int> distribution(WEIGHTS.begin(), WEIGHTS.end());
    const auto operation = static_cast<OperationType>(distribution(rng));
    switch (operation) {
        case OperationType::CREATE_FILE:
        case OperationType::CREATE_DIR:
            CreateRandomNode(operation == OperationType::CREATE_DIR);
            break;
        case OperationType::DELETE_FILE:
        case OperationType::DELETE_DIR:
            DeleteRandomNode(operation == OperationType::DELETE_DIR);
            break;
        case OperationType::MODIFY_CONTENT: {
            Node* target = GetRandomExistingNode(false);
            if (target) {
                ModifyContent(target);
            }
            break;
        }
        case OperationType::RENAME:
            RenameRandomFile();
            break;
        default:
            break;
    }
}

std::string DirectoryManager::GenerateRandomName(size_t length)
{
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const std::string_view characters(charset);
    std::uniform_int_distribution<size_t> dist(0, characters.size() - 1);
    std::string str(length, 0);
    for (size_t i = 0; i < length; ++i) {
        str[i] = charset[dist(rng)];
    }
    return str;
}

Node* DirectoryManager::GetRandomExistingNode(bool requireDirectory)
{
    const auto& nodePtrs = requireDirectory ? dirPtrs : filePtrs;
    if (nodePtrs.empty()) {
        return nullptr;
    }
    std::uniform_int_distribution<size_t> dist(0, nodePtrs.size() - 1);
    return nodePtrs[dist(rng)];
}

Node** DirectoryManager::GetRandomExistingFile()
{
    if (filePtrs.empty()) {
        return nullptr;
    }
    std::uniform_int_distribution<size_t> dist(0, filePtrs.size() - 1);
    return &filePtrs[dist(rng)];
}

Node DirectoryManager::NewNode(Node* parent, const std::string& name, bool isDirectory, const fs::path& fullPath)
{
    Node node;
    node.name = name;
    node.relativePath = fs::path(parent->relativePath).append(name);
    node.parent = parent;
    node.isDirectory = isDirectory;
    struct stat statBuf;
    if (stat(fullPath.c_str(), &statBuf) == 0) {
        node.uid = statBuf.st_uid;
        node.gid = statBuf.st_gid;
        node.mode = statBuf.st_mode;
    } else [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to get status for {}. {}", __func__, fullPath.c_str(), std::strerror(errno)));
    }

    AppendLogEntry(node.isDirectory ? EventType::D_CREATE : EventType::F_CREATE, false, node.relativePath.string(), "",
                   log);

    return node;
}

bool DirectoryManager::CreateNode(Node* parent, const std::string& name, bool isDirectory)
{
    fs::path fullPath = rootPath / parent->relativePath;
    fullPath.append(name);
    size_t delta = 1;
    if (!CheckModificationQuota(delta)) {
        return false;
    }

    if (!isDirectory) {
        // Create a file
        std::ofstream ofs(fullPath);
        if (!ofs) [[unlikely]] {
            throw std::runtime_error(
                fmt::format("{}: Failed to create file {}. {}", __func__, fullPath.c_str(), std::strerror(errno)));
        }
    } else {
        // Create a directory
        if (!fs::create_directory(fullPath)) [[unlikely]] {
            throw std::runtime_error(
                fmt::format("{}: Failed to create directory {}. {}", __func__, fullPath.c_str(), std::strerror(errno)));
        }
    }

    parent->children[name] = NewNode(parent, name, isDirectory, fullPath);

    auto& nodePtrs = isDirectory ? dirPtrs : filePtrs;
    nodePtrs.push_back(&parent->children[name]);
    UpdateParentsTotal(parent, delta);
    modifiedCount++;
    return true;
}

bool DirectoryManager::DeleteNode(Node* node)
{
    size_t delta = node->isDirectory ? node->totalChildren + 1 : 1;
    if (!CheckModificationQuota(delta)) {
        return false;
    }

    fs::path path = rootPath / node->relativePath;
    bool success = node->isDirectory ? fs::remove_all(path) : fs::remove(path);
    if (!success) {
        return false;
    }

    AppendLogEntry(node->isDirectory ? EventType::D_DELETE : EventType::F_DELETE, false, node->relativePath.string(),
                   "", log);

    // Update data structures
    std::function<void(Node*)> recursiveRemove = [&](Node* n) {
        for (auto& child : n->children) {
            if (child.second.isDirectory) {
                recursiveRemove(&child.second);
                auto it = std::remove(dirPtrs.begin(), dirPtrs.end(), &child.second);
                dirPtrs.erase(it, dirPtrs.end());
            } else {
                auto it = std::remove(filePtrs.begin(), filePtrs.end(), &child.second);
                filePtrs.erase(it, filePtrs.end());
            }
        }
    };
    recursiveRemove(node);
    auto& nodePtrs = node->isDirectory ? dirPtrs : filePtrs;
    auto it = std::remove(nodePtrs.begin(), nodePtrs.end(), node);
    nodePtrs.erase(it, nodePtrs.end());

    UpdateParentsTotal(node->parent, -static_cast<ssize_t>(delta));
    node->parent->children.erase(node->name);
    modifiedCount += delta;

    return true;
}

bool DirectoryManager::ModifyContent(Node* node)
{
    size_t delta = 1;
    if (!CheckModificationQuota(delta)) {
        return false;
    }

    fs::path path = rootPath / node->relativePath;

    std::string content = GenerateRandomName(4096);

    std::ofstream ofs(path, std::ios::app);
    if (!ofs) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to open file {}. {}", __func__, path.c_str(), std::strerror(errno)));
    }
    ofs << content << std::endl;

    AppendLogEntry(EventType::F_MODIFY, false, node->relativePath.string(), "", log);

    modifiedCount += delta;
    return true;
}

bool DirectoryManager::RenameFile(Node** target, Node* newParent, const std::string& newName)
{
    fs::path oldPath = rootPath / (*target)->relativePath;
    fs::path newPath = rootPath / newParent->relativePath;
    newPath.append(newName);
    size_t delta = 1;

    if (!CheckModificationQuota(delta)) {
        return false;
    }

    try {
        fs::rename(oldPath, newPath);
        AppendLogEntry(EventType::F_RENAME, true, (fs::path(newParent->relativePath).append(newName)).string(),
                       (*target)->relativePath.string(), log);
        UpdateParentsTotal((*target)->parent, -static_cast<ssize_t>(delta));
        modifiedCount += delta;

        if (newParent->children.count(newName)) {
            Node* oldFile = &newParent->children[newName];
            filePtrs.erase(std::remove(filePtrs.begin(), filePtrs.end(), oldFile), filePtrs.end());
            delta -= 1;
        }

        newParent->children[newName] = std::move(**target);
        *target = &newParent->children[newName];
        (*target)->parent->children.erase((*target)->name);
        (*target)->name = newName;
        (*target)->relativePath = fs::path(newParent->relativePath).append(newName);
        (*target)->parent = newParent;

        UpdateParentsTotal(newParent, delta);
        return true;
    } catch (...) {
        throw std::runtime_error(fmt::format("{}: Failed to rename {} to {}. {}", __func__, oldPath.c_str(),
                                             newPath.c_str(), std::strerror(errno)));
    }
}

void DirectoryManager::ScanDirectory(Node* parent, const fs::path& path)
{
    for (const auto& entry : fs::directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        bool isDirectory = entry.is_directory();

        {
            Node child;
            child.name = name;
            child.relativePath = fs::path(parent->relativePath).append(name);
            child.isDirectory = isDirectory;
            child.parent = parent;

            struct stat statBuf;
            if (stat(entry.path().c_str(), &statBuf) == 0) {
                child.uid = statBuf.st_uid;
                child.gid = statBuf.st_gid;
                child.mode = statBuf.st_mode;
                totalBytes += isDirectory ? 0 : statBuf.st_size;
            } else [[unlikely]] {
                throw std::runtime_error(fmt::format("{}: Failed to get file status for {}. {}", __func__,
                                                     entry.path().c_str(), std::strerror(errno)));
            }

            parent->children[name] = std::move(child);
        }

        if (isDirectory) {
            dirPtrs.push_back(&parent->children[name]);
            ScanDirectory(&parent->children[name], entry.path());
            parent->totalChildren += parent->children[name].totalChildren + 1;
        } else {
            filePtrs.push_back(&parent->children[name]);
            parent->totalChildren += 1;
        }
    }
}

void DirectoryManager::UpdateParentsTotal(Node* node, ssize_t delta)
{
    if (delta == 0) [[unlikely]] {
        return;
    }
    while (node) {
        node->totalChildren += delta;
        node = node->parent;
    }
}
