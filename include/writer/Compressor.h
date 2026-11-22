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
#include <string>
#include <sys/types.h>
#include <tbb/parallel_for_each.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <zstd.h>
#include <fmt/format.h>
#include <cassert>
#include <span>

namespace compression {

static thread_local ZSTD_CCtx* cctx;
static thread_local ZSTD_DCtx* dctx;

inline static void init_cctx()
{
    if (!cctx) [[unlikely]] {
        cctx = ZSTD_createCCtx();
    }
    if (!cctx) [[unlikely]] {
        throw std::runtime_error("Failed to create ZSTD_CCtx");
    }
}

inline static void init_dctx()
{
    if (!dctx) [[unlikely]] {
        dctx = ZSTD_createDCtx();
    }
    if (!dctx) [[unlikely]] {
        throw std::runtime_error("Failed to create ZSTD_DCtx");
    }
}

static ssize_t compress(std::span<uint8_t> dst, std::span<const uint8_t> src, int compressionLevel = 1)
{
    init_cctx();
    size_t compressed_size = ZSTD_compressCCtx(cctx, dst.data(), dst.size(), src.data(), src.size(), compressionLevel);
    if (ZSTD_isError(compressed_size)) [[unlikely]] {
        throw std::runtime_error(
            fmt::format("{}: Failed to compress data: {}", __func__, ZSTD_getErrorName(compressed_size)));
    }

    return compressed_size;
}

inline size_t compress_bound(size_t src_size)
{
    return ZSTD_compressBound(src_size);
}

}; // namespace compression
