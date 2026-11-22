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

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <stdexcept>
#include <sys/sendfile.h>
#include <unistd.h>
#include <vector>
#include <zstd.h>
#include "common/defs.h"

namespace datamov {
ssize_t sendfile(int fdOut, int fdIn, off64_t offset, size_t size);

ssize_t write(int fd, const uint8_t* data, size_t size);

ssize_t pread(int fd, uint8_t* buffer, size_t size, off64_t offset);

inline size_t DecodedSize(const uint8_t* data, size_t size)
{
    const auto decodedSize = ZSTD_getFrameContentSize(data, size);
    if (decodedSize == ZSTD_CONTENTSIZE_ERROR || decodedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        decodedSize > CHUNK_MAX_SIZE) {
        throw std::runtime_error("Invalid or oversized compressed chunk");
    }
    return static_cast<size_t>(decodedSize);
}

ssize_t decompress_write(int fd, const uint8_t* data, size_t size);

inline ssize_t decompress_sendfile(int fdOut, int fdIn, off64_t offset, size_t compressedSize)
{
    if (compressedSize > ZSTD_compressBound(CHUNK_MAX_SIZE)) {
        throw std::runtime_error("Compressed chunk exceeds the size limit");
    }
    std::vector<uint8_t> buffer(compressedSize);
    pread(fdIn, buffer.data(), compressedSize, offset);
    return decompress_write(fdOut, buffer.data(), buffer.size());
}
} // namespace datamov
