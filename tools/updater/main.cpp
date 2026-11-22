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

#include <iostream>

#include "directory_manager.h"

int main(int argc, char* argv[])
{
    constexpr int requiredArgumentCount = 4;
    constexpr int optionalLogArgumentCount = 5;
    if (argc != requiredArgumentCount && argc != optionalLogArgumentCount) {
        std::cerr << "Usage: " << argv[0] << " <directory> <upper_percent> <lower_percent> [log_path]\n";
        return 1;
    }
    try {
        const std::string directoryPath = argv[1];
        const float upper = std::stof(argv[2]);
        const float lower = std::stof(argv[3]);
        const std::string logPath = argc == optionalLogArgumentCount ? argv[4] : "";
        DirectoryManager manager(directoryPath, logPath);
        manager.SetXPercent(upper, lower);
        while (!manager.IsDone()) {
            manager.GenerateRandomOperation();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
