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
#include <boost/serialization/serialization.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <fstream>
#include <chrono>
#include <filesystem>
namespace fs = std::filesystem;

namespace boost {
namespace serialization {
template <class Archive> void save(Archive& ar, const fs::file_time_type& t, const unsigned int version)
{
    auto duration_since_epoch = t.time_since_epoch();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration_since_epoch);
    auto count = nanoseconds.count();
    ar & count;
}

template <class Archive> void load(Archive& ar, fs::file_time_type& t, const unsigned int version)
{
    std::chrono::nanoseconds::rep count;
    ar & count;
    t = fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::nanoseconds(count)));
}

template <class Archive> void serialize(Archive& ar, fs::file_time_type& t, const unsigned int version)
{
    boost::serialization::split_free(ar, t, version);
}

template <class Archive>
void save(Archive& ar, const std::chrono::system_clock::time_point& t, const unsigned int version)
{
    auto duration_since_epoch = t.time_since_epoch();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration_since_epoch);
    auto count = nanoseconds.count();
    ar & count;
}

template <class Archive> void load(Archive& ar, std::chrono::system_clock::time_point& t, const unsigned int version)
{
    std::chrono::nanoseconds::rep count;
    ar & count;
    t = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(count)));
}

template <class Archive>
void serialize(Archive& ar, std::chrono::system_clock::time_point& t, const unsigned int version)
{
    boost::serialization::split_free(ar, t, version);
}
} // namespace serialization
} // namespace boost
