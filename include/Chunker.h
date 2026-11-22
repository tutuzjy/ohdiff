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
#include <cstdint>
#include <vector>
#include <iostream>
#include "common/defs.h"

struct ChunkData {
    uint64_t hash;                // The gear hash value as of the end of the chunk.
    uint64_t offset;              // Starting byte position within the source.
    std::size_t length;           // Length of the chunk in bytes.
    uint8_t data[CHUNK_MAX_SIZE]; // Source bytes contained in this chunk.
};

enum class Normalization {
    /// No chunk size normalization, produces a wide range of chunk sizes.
    Level0,
    /// Level 1 normalization, in which fewer chunks are outside of the desired range.
    Level1,
    /// Level 2 normalization, where most chunks are of the desired size.
    Level2,
    /// Level 3 normalization, nearly all chunks are the desired size.
    Level3,
};

struct StreamCDC {
    std::vector<uint8_t> buffer;
    size_t capacity;
    size_t length;
    std::istream& source;
    uint64_t processed;
    size_t minSize;
    size_t avgSize;
    size_t maxSize;
    uint64_t maskS;
    uint64_t maskL;
    uint64_t maskSLs;
    uint64_t maskLLs;

    StreamCDC(std::istream& source, uint32_t minSize, uint32_t avgSize, uint32_t maxSize,
              Normalization level = Normalization::Level1);
    size_t fill_buffer();
    void drain_bytes_to_buffer(uint8_t* buffer, size_t count);

    // 调用此函数时应保证 chunk.data 有足够的空间 (maxSize)
    bool next_chunk_no_alloc(ChunkData& chunk);
};
