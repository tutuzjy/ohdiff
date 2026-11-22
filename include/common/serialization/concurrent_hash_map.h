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
#include <tbb/concurrent_hash_map.h>

#include <boost/serialization/split_free.hpp>
#include <boost/serialization/utility.hpp>

namespace boost {
namespace serialization {

template <class Archive, class Key, class T, class HashCompare, class A>
void save(Archive& ar, const tbb::concurrent_hash_map<Key, T, HashCompare, A>& map, const unsigned int version)
{
    size_t size = map.size();
    ar & size;
    for (auto it = map.begin(); it != map.end(); ++it) {
        ar & it->first;
        ar & it->second;
    }
}

template <class Archive, class Key, class T, class HashCompare, class A>
void load(Archive& ar, tbb::concurrent_hash_map<Key, T, HashCompare, A>& map, const unsigned int version)
{
    size_t size;
    ar & size;
    for (size_t i = 0; i < size; ++i) {
        Key key;
        T value;
        ar & key;
        ar & value;
        map.insert(std::make_pair(key, value));
    }
}

template <class Archive, class Key, class T, class HashCompare, class A>
void serialize(Archive& ar, tbb::concurrent_hash_map<Key, T, HashCompare, A>& map, const unsigned int version)
{
    boost::serialization::split_free(ar, map, version);
}

} // namespace serialization
} // namespace boost
