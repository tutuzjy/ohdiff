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

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include "log/log.h"

namespace {
constexpr int ARGUMENT_COUNT = 2;
constexpr int EXIT_EVENT = static_cast<int>(EventType::D_RENAME) + 1;

bool ReadConsoleEntry(LogEntry& entry)
{
    int eventType = 0;
    std::cout << "EventType (0: F_CREATE, 1: F_MODIFY, 2: F_DELETE, 3: F_RENAME, "
                 "4: D_CREATE, 5: D_MODIFY, 6: D_DELETE, 7: D_RENAME, 8: exit): ";
    if (!(std::cin >> eventType) || eventType == EXIT_EVENT) {
        return false;
    }
    if (eventType < 0 || eventType >= EXIT_EVENT) {
        throw std::invalid_argument("Invalid event type");
    }
    entry.type = static_cast<EventType>(eventType);
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cout << "File/directory path relative to the backup root: ";
    if (!std::getline(std::cin, entry.path)) {
        return false;
    }
    if (entry.type == EventType::F_RENAME || entry.type == EventType::D_RENAME) {
        std::cout << "Previous relative path: ";
        if (!std::getline(std::cin, entry.oldPath)) {
            return false;
        }
    }
    int metadataOnly = 0;
    std::cout << "Metadata only? (0: No, 1: Yes): ";
    if (!(std::cin >> metadataOnly)) {
        return false;
    }
    if (metadataOnly != 0 && metadataOnly != 1) {
        throw std::invalid_argument("Invalid metadata flag");
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    entry.metadataOnly = metadataOnly != 0;
    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc != ARGUMENT_COUNT) {
        std::cerr << "Usage: " << argv[0] << " <log path>\n";
        return 1;
    }
    try {
        std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to open log file");
        }
        LogEntry entry{};
        while (ReadConsoleEntry(entry)) {
            WriteLogEntry(output, entry);
            std::cout << "Logged: " << entry.path << '\n';
            entry = {};
        }
        if (std::cin.fail() && !std::cin.eof()) {
            throw std::runtime_error("Invalid console input");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
