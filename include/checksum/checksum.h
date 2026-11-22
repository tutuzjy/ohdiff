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
#include <functional>
#include <ostream>
#include <fmt/format.h>

#include "xxhash.h"

using WeakChecksum = uint64_t;
using StrongChecksum = XXH128_hash_t;

static std::ostream& operator<<(std::ostream& os, const XXH128_hash_t& checksum)
{
    os << std::hex << checksum.high64 << checksum.low64;
    return os;
}

static bool operator==(const XXH128_hash_t& lhs, const XXH128_hash_t& rhs)
{
    return lhs.high64 == rhs.high64 && lhs.low64 == rhs.low64;
}

static bool operator!=(const XXH128_hash_t& lhs, const XXH128_hash_t& rhs)
{
    return !(lhs == rhs);
}

template <> struct std::hash<XXH128_hash_t> {
    std::size_t operator()(const XXH128_hash_t& key) const
    {
        return static_cast<std::size_t>(key.low64); // 低 64 位给 hash 表用应该足够了。
    }
};

template <> struct fmt::formatter<XXH128_hash_t> : fmt::formatter<std::string_view> {
    auto format(XXH128_hash_t p, fmt::format_context& ctx) const
    {
        return fmt::formatter<std::string_view>::format(fmt::format("{:016x}{:016x}", p.high64, p.low64), ctx);
    }
};
