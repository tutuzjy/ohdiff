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
#include <boost/serialization/vector.hpp>
#include <tbb/concurrent_vector.h>

namespace boost {
namespace serialization {
template <class Archive, class T, class A>
void save(Archive& ar, const tbb::concurrent_vector<T, A>& vec, const unsigned int version)
{
    size_t size = vec.size();
    ar & size;
    for (const auto& item : vec) {
        ar & item;
    }
}

template <class Archive, class T, class A>
void load(Archive& ar, tbb::concurrent_vector<T, A>& vec, const unsigned int version)
{
    size_t size;
    ar & size;
    vec.grow_by(size);
    for (size_t i = 0; i < size; ++i) {
        ar& vec[i];
    }
}

template <class Archive, class T, class A>
void serialize(Archive& ar, tbb::concurrent_vector<T, A>& vec, const unsigned int version)
{
    boost::serialization::split_free(ar, vec, version);
}

} // namespace serialization
} // namespace boost
