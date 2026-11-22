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

#include "writer/datamov.h"

namespace datamov {
ssize_t sendfile(int fdOut, int fdIn, off64_t offset, size_t size)
{
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t sent = ::sendfile(fdOut, fdIn, &offset, remaining);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            throw std::runtime_error(fmt::format("Failed to transfer file data: {}", std::strerror(errno)));
        }
        remaining -= static_cast<size_t>(sent);
    }
    return static_cast<ssize_t>(size);
}

ssize_t write(int fd, const uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            throw std::runtime_error(fmt::format("Failed to write file data: {}", std::strerror(errno)));
        }
        offset += static_cast<size_t>(written);
    }
    return static_cast<ssize_t>(size);
}

ssize_t pread(int fd, uint8_t* buffer, size_t size, off64_t offset)
{
    size_t completed = 0;
    while (completed < size) {
        const ssize_t count = ::pread64(fd, buffer + completed, size - completed, offset + completed);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error(fmt::format("Failed to read complete file data: {}", std::strerror(errno)));
        }
        completed += static_cast<size_t>(count);
    }
    return static_cast<ssize_t>(size);
}

ssize_t decompress_write(int fd, const uint8_t* data, size_t size)
{
    using Context = std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>;
    thread_local Context context(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!context) {
        throw std::runtime_error("Failed to create decompression context");
    }
    std::vector<uint8_t> buffer(DecodedSize(data, size));
    const size_t decodedSize = ZSTD_decompressDCtx(context.get(), buffer.data(), buffer.size(), data, size);
    if (ZSTD_isError(decodedSize) || decodedSize != buffer.size()) {
        throw std::runtime_error("Failed to decompress the complete chunk");
    }
    return write(fd, buffer.data(), decodedSize);
}
} // namespace datamov
