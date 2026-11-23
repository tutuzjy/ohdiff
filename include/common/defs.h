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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tbb/global_control.h>

using chunk_size_t = uint32_t;
using size_t = std::size_t;
using layer_id_t = uint16_t;

const chunk_size_t CHUNK_MIN_SIZE = 4 << 10;
const chunk_size_t CHUNK_AVG_SIZE = 16 << 10;
const chunk_size_t CHUNK_MAX_SIZE = 64 << 10;
inline constexpr unsigned CHUNK_OFFSET_BITS = 48;

const char* const TEMP_FILE_SUFFIX = ".ohdiff_tmp";

const int NO_COMPRESSION_LEVEL = std::numeric_limits<int>::min();
const int DEFAULT_COMPRESSION_LEVEL = 1;

namespace performance {
namespace low {
namespace concurrent_writer {
const size_t SECTOR_NUM = 4;
const size_t SECTOR_SIZE = 1ull << 20;
} // namespace concurrent_writer
} // namespace low
namespace high {
namespace concurrent_writer {
const size_t SECTOR_NUM = 64;
const size_t SECTOR_SIZE = 1ull << 20;
} // namespace concurrent_writer
} // namespace high
} // namespace performance

class GlobalConfig {
    GlobalConfig() = default;
    GlobalConfig(const GlobalConfig&) = delete;
    GlobalConfig& operator=(const GlobalConfig&) = delete;

public:
    static inline GlobalConfig& get_instance()
    {
        static GlobalConfig instance;
        return instance;
    }

    bool performance_mode = false;
};

#define global_config GlobalConfig::get_instance()
