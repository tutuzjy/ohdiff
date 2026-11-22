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
#include <checksum/checksum.h>

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/serialization.hpp>

namespace boost {
namespace serialization {
template <class Archive> void save(Archive& ar, const XXH128_hash_t& t, const unsigned int version)
{
    ar & t.high64;
    ar & t.low64;
}

template <class Archive> void load(Archive& ar, XXH128_hash_t& t, const unsigned int version)
{
    ar & t.high64;
    ar & t.low64;
}

template <class Archive> void serialize(Archive& ar, XXH128_hash_t& t, const unsigned int version)
{
    boost::serialization::split_free(ar, t, version);
}
} // namespace serialization
} // namespace boost
