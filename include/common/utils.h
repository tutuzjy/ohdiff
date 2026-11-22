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
#include <algorithm>
#include <chrono>
#include <concepts>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>
#include <sys/types.h>
#include <tbb/parallel_for_each.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_reduce.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <ranges>
#include <cassert>
#include <zstd.h>
#include <fmt/format.h>
#include <fstream>
#include <common/defs.h>

namespace fs = std::filesystem;

static std::string remove_tail_slash(const std::string& path)
{
    if (path.empty()) {
        return path;
    }
    if (path.back() == '/') {
        return path.substr(0, path.size() - 1);
    }
    return path;
}

template <typename T, typename V>
concept Range = std::ranges::range<T> && std::same_as<std::remove_cvref_t<std::ranges::range_value_t<T>>, V>;

static std::chrono::system_clock::time_point parse_timepoint(const std::string& timepoint)
{
    std::tm tm = {};
    std::istringstream ss(timepoint);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) [[unlikely]] {
        throw std::runtime_error(fmt::format("{}: Invalid timepoint format: {}", __func__, timepoint));
    }
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

static std::string data_size(size_t bytes, int precision = 0)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    int unitIndex = 0;
    constexpr size_t BYTES_PER_UNIT = 1024;
    float size = bytes;

    // 如果 bytes 大于 1024 则继续转换到更高的单位
    while (size >= BYTES_PER_UNIT && unitIndex < sizeof(units) / sizeof(units[0]) - 1) {
        size /= BYTES_PER_UNIT;
        unitIndex++;
    }

    // 使用 stringstream 来格式化输出
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << size << " " << units[unitIndex];
    return oss.str();
}

static int debug_get_memory_usage(const std::string& msg)
{
    std::string line;
    std::string key;
    int value = 0;

    std::ifstream file("/proc/self/status");
    while (getline(file, line)) {
        if (line.starts_with("VmRSS:")) {
            std::istringstream iss(line);
            iss >> key >> value;
            break;
        }
    }

    std::cout << msg << ": " << value << " KB" << std::endl;
    return value;
}

namespace utils {
template <typename T> inline T* get_ptr(T& t)
{
    return &t;
}
} // namespace utils

inline constexpr bool is_power_of_2(uint64_t x)
{
    return x && !(x & (x - 1));
}

static void clean_temp_files(const fs::path& backup_dir)
{
    for (const auto& entry : fs::directory_iterator(backup_dir)) {
        if (entry.path().extension() == TEMP_FILE_SUFFIX) {
            fs::remove(entry.path());
        }
    }
}

inline std::vector<fs::path> DirectoryEntries(const fs::path& path)
{
    std::vector<fs::path> entries;
    std::transform(fs::directory_iterator(path), fs::directory_iterator(), std::back_inserter(entries),
                   [](const fs::directory_entry& entry) { return entry.path(); });
    return entries;
}

static size_t get_directory_size(const fs::path& dir_path)
{
    auto size_func = [](const fs::path& path) {
        if (fs::is_regular_file(path)) {
            return fs::file_size(path);
        } else if (fs::is_directory(path)) {
            return get_directory_size(path);
        }
        return static_cast<size_t>(0);
    };

    const auto paths = DirectoryEntries(dir_path);

    return tbb::parallel_reduce(
        tbb::blocked_range<std::vector<fs::path>::const_iterator>(paths.begin(), paths.end()), static_cast<size_t>(0),
        [&](tbb::blocked_range<std::vector<fs::path>::const_iterator> r, size_t init) -> size_t {
            for (auto it = r.begin(); it != r.end(); ++it) {
                init += size_func(*it);
            }
            return init;
        },
        std::plus<size_t>());
}
