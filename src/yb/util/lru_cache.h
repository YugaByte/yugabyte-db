// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_UTIL_LRU_CACHE_H
#define YB_UTIL_LRU_CACHE_H

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>

namespace yb {

// The cache that stores at most specified number of recently added entries.
template <class Value>
class LRUCache {
 private:
  class IdTag;

  using Impl = boost::multi_index_container<
      Value,
      boost::multi_index::indexed_by<
        boost::multi_index::sequenced<>,
        boost::multi_index::hashed_unique<
          boost::multi_index::tag<IdTag>,
          boost::multi_index::identity<Value>
        >
      >
  >;

 public:
  typedef typename Impl::const_iterator const_iterator;

  explicit LRUCache(size_t capacity) : capacity_(capacity) {}

  // Insert entry in cache.
  void insert(const Value& value) {
    auto p = impl_.push_front(value);

    if (!p.second) {
      impl_.relocate(impl_.begin(), p.first);
    } else if (impl_.size() > capacity_) {
      impl_.pop_back();
    }
  }

  void Insert(const Value& value) {
    insert(value);
  }

  // Erase entry from cache. Returns number of removed entries.
  template <class Key>
  size_t erase(const Key& key) {
    return impl_.template get<IdTag>().erase(key);
  }

  // Erase entry from cache. Returns true if entry was removed.
  template <class Key>
  size_t Erase(const Key& key) {
    return erase(key);
  }

  const_iterator begin() const {
    return impl_.begin();
  }

  const_iterator end() const {
    return impl_.end();
  }

 private:
  const size_t capacity_;
  Impl impl_;
};

} // namespace yb

#endif // YB_UTIL_LRU_CACHE_H
