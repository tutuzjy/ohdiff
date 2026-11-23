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

#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <writer/datamov.h>
#include <common/utils.h>

namespace fs = std::filesystem;

class ConcurrentWriter {
    const size_t sector_num;
    const size_t sector_size;
    const size_t ring_size;
    const size_t sector_size_log2;
    std::unique_ptr<uint8_t[]> buffer;
    std::vector<std::atomic<size_t>> filled_bytes;
    std::atomic<size_t> head = 0;
    std::atomic<size_t> tail = 0;
    std::atomic<bool> write_failed = false;
    std::mutex write_mutex;
    int fd = -1;

    size_t get_sector_i(size_t offset) const
    {
        return (offset >> sector_size_log2) & (sector_num - 1);
    }

    void WaitForSpace(size_t end)
    {
        size_t observed = tail.load(std::memory_order_acquire);
        while (end - observed > ring_size) {
            if (write_failed.load(std::memory_order_acquire)) {
                throw std::runtime_error("Concurrent pool write failed");
            }
            std::this_thread::yield();
            observed = tail.load(std::memory_order_acquire);
        }
    }

    template <bool flush> void write_back()
    {
        // Claim and advance the tail together so a recycled sector cannot be
        // mistaken for an older generation by another flushing thread.
        const std::lock_guard<std::mutex> lock(write_mutex);
        size_t offset = tail.load(std::memory_order_relaxed);
        size_t sector = get_sector_i(offset);
        const size_t required = flush ? 1 : sector_size;
        while (filled_bytes[sector].load(std::memory_order_acquire) >= required) {
            const size_t filled = filled_bytes[sector].exchange(0, std::memory_order_acquire);
            if (filled == 0) {
                return;
            }
            try {
                datamov::write(fd, buffer.get() + (offset & (ring_size - 1)), filled);
            } catch (...) {
                write_failed.store(true, std::memory_order_release);
                throw;
            }
            offset += filled;
            tail.store(offset, std::memory_order_release);
            sector = get_sector_i(offset);
        }
    }

    void write_to_sector(const uint8_t* data, size_t offset, size_t size)
    {
        const size_t sector = get_sector_i(offset);
        const size_t ringOffset = offset & (ring_size - 1);
        if (size > sector_size || size > ring_size - ringOffset) {
            throw std::out_of_range("Pool write exceeds its sector");
        }
        std::copy_n(data, size, buffer.get() + ringOffset);
        const size_t filled = filled_bytes[sector].fetch_add(size, std::memory_order_acq_rel);
        if (filled + size == sector_size) {
            write_back<false>();
        }
    }

public:
    ConcurrentWriter(fs::path file, size_t sectorCount, size_t sectorSize)
        : sector_num(sectorCount), sector_size(sectorSize), ring_size(sectorCount * sectorSize),
          sector_size_log2(std::countr_zero(sectorSize)), filled_bytes(sectorCount)
    {
        if (sectorSize == 0 || !is_power_of_2(sectorCount) || !is_power_of_2(sectorSize) ||
            sectorSize < CHUNK_MAX_SIZE) {
            throw std::invalid_argument("Invalid concurrent writer sector limits");
        }
        if (sectorCount > std::numeric_limits<size_t>::max() / sectorSize) {
            throw std::invalid_argument("Invalid concurrent writer sector limits");
        }
        buffer = std::make_unique<uint8_t[]>(ring_size);
        constexpr mode_t FILE_MODE = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        fd = open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC, FILE_MODE);
        if (fd == -1) {
            throw std::runtime_error(fmt::format("Failed to create pool {}: {}", file.string(), std::strerror(errno)));
        }
    }

    ConcurrentWriter(const ConcurrentWriter&) = delete;
    ConcurrentWriter& operator=(const ConcurrentWriter&) = delete;

    void flush()
    {
        if (write_failed.load(std::memory_order_acquire)) {
            throw std::runtime_error("Concurrent pool write failed");
        }
        write_back<true>();
    }

    ~ConcurrentWriter()
    {
        // Callers flush explicitly before publishing a pool. Destruction also releases resources on errors.
        try {
            if (!write_failed.load(std::memory_order_acquire)) {
                flush();
            }
        } catch (...) {
            // Destruction cannot propagate a second write exception during stack unwinding.
        }
        close(fd);
    }

    size_t write(const uint8_t* data, size_t size)
    {
        if (size == 0) {
            return head.load(std::memory_order_relaxed);
        }
        if (size > sector_size || data == nullptr || write_failed.load(std::memory_order_acquire)) {
            throw std::invalid_argument("Invalid concurrent pool write");
        }
        constexpr size_t maxPoolSize = static_cast<size_t>(
            std::min<uint64_t>(std::numeric_limits<size_t>::max(), (uint64_t{1} << CHUNK_OFFSET_BITS) - 1));
        size_t offset = head.load(std::memory_order_relaxed);
        do {
            if (size > maxPoolSize - offset) {
                write_failed.store(true, std::memory_order_release);
                throw std::overflow_error("Backup data pool exceeds this build's size limit");
            }
        } while (!head.compare_exchange_weak(offset, offset + size, std::memory_order_relaxed));
        const size_t end = offset + size;
        size_t cursor = offset;
        if ((cursor >> sector_size_log2) != ((end - 1) >> sector_size_log2)) {
            const size_t sectorEnd = (cursor + sector_size - 1) & ~(sector_size - 1);
            const size_t firstSize = sectorEnd - cursor;
            WaitForSpace(cursor + 1);
            write_to_sector(data, cursor, firstSize);
            cursor = sectorEnd;
            data += firstSize;
            size -= firstSize;
        }
        WaitForSpace(end);
        write_to_sector(data, cursor, size);
        return offset;
    }

    size_t size() const
    {
        return head.load(std::memory_order_relaxed);
    }
};

namespace performance::presets::concurrent_writer {
inline std::unique_ptr<ConcurrentWriter> get_instance(fs::path file)
{
    if (global_config.performance_mode) {
        return std::make_unique<ConcurrentWriter>(file, performance::high::concurrent_writer::SECTOR_NUM,
                                                  performance::high::concurrent_writer::SECTOR_SIZE);
    }
    return std::make_unique<ConcurrentWriter>(file, performance::low::concurrent_writer::SECTOR_NUM,
                                              performance::low::concurrent_writer::SECTOR_SIZE);
}
} // namespace performance::presets::concurrent_writer
