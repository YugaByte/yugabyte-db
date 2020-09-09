//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <gflags/gflags.h>

#include "yb/util/metrics.h"
#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/util/autovector.h"
#include "yb/rocksdb/util/hash.h"
#include "yb/rocksdb/util/mutexlock.h"
#include "yb/rocksdb/util/statistics.h"

#include "yb/util/enums.h"
#include "yb/util/random_util.h"

// 0 value means that there exist no single_touch cache and
// 1 means that the entire cache is treated as a multi-touch cache.
DEFINE_double(cache_single_touch_ratio, 0.2,
              "Fraction of the cache dedicated to single-touch items");

DEFINE_bool(cache_overflow_single_touch, true,
            "Whether to enable overflow of single touch cache into the multi touch cache "
            "allocation");

namespace rocksdb {

Cache::~Cache() {
}

namespace {

// LRU cache implementation

// An entry is a variable length heap-allocated structure.
// Entries are referenced by cache and/or by any external entity.
// The cache keeps all its entries in table. Some elements
// are also stored on LRU list.
//
// LRUHandle can be in these states:
// 1. Referenced externally AND in hash table.
//  In that case the entry is *not* in the LRU. (refs > 1 && in_cache == true)
// 2. Not referenced externally and in hash table. In that case the entry is
// in the LRU and can be freed. (refs == 1 && in_cache == true)
// 3. Referenced externally and not in hash table. In that case the entry is
// in not on LRU and not in table. (refs >= 1 && in_cache == false)
//
// All newly created LRUHandles are in state 1. If you call LRUCache::Release
// on entry in state 1, it will go into state 2. To move from state 1 to
// state 3, either call LRUCache::Erase or LRUCache::Insert with the same key.
// To move from state 2 to state 1, use LRUCache::Lookup.
// Before destruction, make sure that no handles are in state 1. This means
// that any successful LRUCache::Lookup/LRUCache::Insert have a matching
// RUCache::Release (to move into state 2) or LRUCache::Erase (for state 3)
//
// LRU also supports scan resistant access by allowing it to be one of two
// caches. query_id is to detect that multiple touches from the same query will not
// upgrade the cache element from single touch LRU into the multiple touch LRU.
// query_id == kInMultiTouchId means that the handle is in the multi
// touch cache, which means that this item will be evicted only for new values
// that are accessed multiple times by different queries.
// query_id == kNoCacheQueryId means that this Handle is not going to be added
// into the cache.

struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void* value);
  LRUHandle* next_hash;
  LRUHandle* next;
  LRUHandle* prev;
  size_t charge;      // TODO(opt): Only allow uint32_t?
  size_t key_length;
  uint32_t refs;      // a number of refs to this entry
                      // cache itself is counted as 1
  bool in_cache;      // true, if this entry is referenced by the hash table
  uint32_t hash;      // Hash of key(); used for fast sharding and comparisons
  QueryId query_id;  // Query id that added the value to the cache.
  char key_data[1];   // Beginning of key

  Slice key() const {
    // For cheaper lookups, we allow a temporary Handle object
    // to store a pointer to a key in "value".
    if (next == this) {
      return *(reinterpret_cast<Slice*>(value));
    } else {
      return Slice(key_data, key_length);
    }
  }

  void Free(yb::CacheMetrics* metrics) {
    assert((refs == 1 && in_cache) || (refs == 0 && !in_cache));
    (*deleter)(key(), value);
    if (metrics != nullptr) {
      if (GetSubCacheType() == MULTI_TOUCH) {
        metrics->multi_touch_cache_usage->DecrementBy(charge);
      } else if (GetSubCacheType() == SINGLE_TOUCH) {
        metrics->single_touch_cache_usage->DecrementBy(charge);
      }
      metrics->cache_usage->DecrementBy(charge);
    }
    delete[] reinterpret_cast<char*>(this);
  }

  SubCacheType GetSubCacheType() const {
    return (query_id == kInMultiTouchId) ? MULTI_TOUCH : SINGLE_TOUCH;
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
class HandleTable {
 public:
  HandleTable() :
      length_(0), elems_(0), list_(nullptr), metrics_(nullptr) { Resize(); }

  template <typename T>
  void ApplyToAllCacheEntries(T func) {
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        auto n = h->next_hash;
        assert(h->in_cache);
        func(h);
        h = n;
      }
    }
  }

  ~HandleTable() {
    ApplyToAllCacheEntries([this](LRUHandle* h) {
      if (h->refs == 1) {
        h->Free(metrics_.get());
      }
    });
    delete[] list_;
  }

  LRUHandle* Lookup(const Slice& key, uint32_t hash) const {
    return *FindPointer(key, hash);
  }

  void SetMetrics(shared_ptr<yb::CacheMetrics> metrics) { metrics_ = metrics; }

  // Checks if the newly created handle is a candidate to be inserted into the multi touch cache.
  // It checks to see if the same value is in the multi touch cache, or if it is in the single
  // touch cache, checks to see if the query ids are different.
  SubCacheType GetSubCacheTypeCandidate(LRUHandle* h) {
    if (h->GetSubCacheType() == MULTI_TOUCH) {
      return MULTI_TOUCH;
    }

    LRUHandle* val = Lookup(h->key(), h->hash);
    if (val != nullptr && (val->GetSubCacheType() == MULTI_TOUCH || val->query_id != h->query_id)) {
      h->query_id = kInMultiTouchId;
      return MULTI_TOUCH;
    }
    return SINGLE_TOUCH;
  }

  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        Resize();
      }
    }
    return old;
  }

  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;
  shared_ptr<yb::CacheMetrics> metrics_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) const {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr &&
           ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  void Resize() {
    uint32_t new_length = 16;
    while (new_length < elems_ * 1.5) {
      new_length *= 2;
    }
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    LRUHandle* h;
    LRUHandle* next;
    LRUHandle** ptr;
    uint32_t hash;
    for (uint32_t i = 0; i < length_; i++) {
      h = list_[i];
      while (h != nullptr) {
        next = h->next_hash;
        hash = h->hash;
        ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// Sub-cache of the LRUCache that is used to track different LRU pointers, capacity and usage.
class LRUSubCache {
 public:
  LRUSubCache();
  ~LRUSubCache();

  // Accessors.
  size_t Usage() const {
    return usage_;
  }

  size_t LRU_Usage() const {
    return lru_usage_;
  }

  LRUHandle& LRU_Head() {
    return lru_;
  }

  // Checks if the head of the LRU linked list is pointing to itself,
  // meaning that LRU list is empty.
  bool IsLRUEmpty() const {
    return lru_.next == &lru_;
  }

  size_t GetPinnedUsage() const {
    assert(usage_ >= lru_usage_);
    return usage_ - lru_usage_;
  }

  void DecrementUsage(const size_t charge) {
    assert(usage_ >= charge);
    usage_ -= charge;
  }

  void IncrementUsage(const size_t charge) {
    assert(usage_ + charge > 0);
    usage_ += charge;
  }

  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle *e);

 private:
  // Dummy heads of single-touch and multi-touch LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // LRU contains items which can be evicted, ie referenced only by cache.
  LRUHandle lru_;

  // Memory size for entries residing in the cache.
  // Includes entries in the LRU list and referenced by callers and thus not eligible for cleanup.
  size_t usage_;

  // Memory size for entries residing only in the LRU list
  size_t lru_usage_;
};

LRUSubCache::LRUSubCache() : usage_(0), lru_usage_(0) {
  // Make empty circular linked list
  lru_.next = &lru_;
  lru_.prev = &lru_;
}

LRUSubCache::~LRUSubCache() {}

// Remove the handle from the LRU list of the sub cache.
void LRUSubCache::LRU_Remove(LRUHandle* e) {
  assert(e->next != nullptr);
  assert(e->prev != nullptr);
  e->next->prev = e->prev;
  e->prev->next = e->next;
  e->prev = e->next = nullptr;
  lru_usage_ -= e->charge;
}

// Append to the LRU header of the sub cache.
void LRUSubCache::LRU_Append(LRUHandle *e) {
  assert(e->next == nullptr);
  assert(e->next == nullptr);
  e->next = &lru_;
  e->prev = lru_.prev;
  e->prev->next = e;
  e->next->prev = e;
  lru_usage_ += e->charge;
}

class LRUHandleDeleter {
 public:
  explicit LRUHandleDeleter(yb::CacheMetrics* metrics) : metrics_(metrics) {}

  void Add(LRUHandle* handle) {
    handles_.push_back(handle);
  }

  size_t TotalCharge() const {
    size_t result = 0;
    for (LRUHandle* handle : handles_) {
      result += handle->charge;
    }
    return result;
  }

  ~LRUHandleDeleter() {
    for (LRUHandle* handle : handles_) {
      handle->Free(metrics_);
    }
  }

 private:
  yb::CacheMetrics* metrics_;
  autovector<LRUHandle*> handles_;
};

// A single shard of sharded cache.
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  // if current usage is more than new capacity, the function will attempt to
  // free the needed space
  void SetCapacity(size_t capacity);

  void SetMetrics(shared_ptr<yb::CacheMetrics> metrics) {
    metrics_ = metrics;
    table_.SetMetrics(metrics);
  }

  // Set the flag to reject insertion if cache if full.
  void SetStrictCapacityLimit(bool strict_capacity_limit);

  // Like Cache methods, but with an extra "hash" parameter.
  Status Insert(const Slice& key, uint32_t hash, const QueryId query_id,
                void* value, size_t charge, void (*deleter)(const Slice& key, void* value),
                Cache::Handle** handle, Statistics* statistics);
  Cache::Handle* Lookup(const Slice& key, uint32_t hash, const QueryId query_id,
                        Statistics* statistics = nullptr);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  size_t Evict(size_t required);

  // Although in some platforms the update of size_t is atomic, to make sure
  // GetUsage() and GetPinnedUsage() work correctly under any platform, we'll
  // protect them with mutex_.

  size_t GetUsage() const {
    MutexLock l(&mutex_);
    return single_touch_sub_cache_.Usage() + multi_touch_sub_cache_.Usage();
  }

  size_t GetPinnedUsage() const {
    MutexLock l(&mutex_);
    return single_touch_sub_cache_.GetPinnedUsage() + multi_touch_sub_cache_.GetPinnedUsage();
  }

  void ApplyToAllCacheEntries(void (*callback)(void*, size_t),
                              bool thread_safe);

  std::pair<size_t, size_t> TEST_GetIndividualUsages() {
    return make_pair<size_t, size_t>(
        single_touch_sub_cache_.Usage(), multi_touch_sub_cache_.Usage());
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* e);

  // Returns the correct SubCache based on the input argument.
  LRUSubCache* GetSubCache(const SubCacheType subcache_type);
  LRUSubCache single_touch_sub_cache_;
  LRUSubCache multi_touch_sub_cache_;

  size_t total_capacity_;
  size_t multi_touch_capacity_;

  // Just reduce the reference count by 1.
  // Return true if last reference
  bool Unref(LRUHandle* e);

  // Returns the capacity of the subcache.
  // For multi touch cache it is the same as its initial allocation.
  // For single touch cache it is the amount of space left in the entire cache.
  size_t GetSubCacheCapacity(const SubCacheType subcache_type);

  // Free some space following strict LRU policy until enough space
  // to hold (usage_ + charge) is freed or the lru list is empty.
  // This function is not thread safe - it needs to be executed while
  // holding the mutex_
  void EvictFromLRU(size_t charge, LRUHandleDeleter* deleted, SubCacheType subcache_type);

  void DecrementUsage(const SubCacheType subcache_type, const size_t charge);

  // Checks if the corresponding subcache contains space.
  bool HasFreeSpace(const SubCacheType subcache_type);

  size_t TotalUsage() const {
    return single_touch_sub_cache_.Usage() + multi_touch_sub_cache_.Usage();
  }

  // Whether to reject insertion if cache reaches its full capacity.
  bool strict_capacity_limit_ = false;

  // mutex_ protects the following state.
  // We don't count mutex_ as the cache's internal state so semantically we
  // don't mind mutex_ invoking the non-const actions.
  mutable port::Mutex mutex_;

  HandleTable table_;

  shared_ptr<yb::CacheMetrics> metrics_;
};

LRUCache::LRUCache() {}

LRUCache::~LRUCache() {}

bool LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  return e->refs == 0;
}

LRUSubCache* LRUCache::GetSubCache(const SubCacheType subcache_type) {
  if (FLAGS_cache_single_touch_ratio == 0) {
    return &multi_touch_sub_cache_;
  } else if (FLAGS_cache_single_touch_ratio == 1) {
    return &single_touch_sub_cache_;
  }
  return (subcache_type == SubCacheType::MULTI_TOUCH) ? &multi_touch_sub_cache_ :
                                                        &single_touch_sub_cache_;
}

void LRUCache::DecrementUsage(const SubCacheType subcache_type, const size_t charge) {
  GetSubCache(subcache_type)->DecrementUsage(charge);
}

// Call deleter and free

void LRUCache::ApplyToAllCacheEntries(void (*callback)(void*, size_t),
                                      bool thread_safe) {
  if (thread_safe) {
    mutex_.Lock();
  }
  table_.ApplyToAllCacheEntries([callback](LRUHandle* h) {
    callback(h->value, h->charge);
  });
  if (thread_safe) {
    mutex_.Unlock();
  }
}

void LRUCache::LRU_Remove(LRUHandle* e) {
  GetSubCache(e->GetSubCacheType())->LRU_Remove(e);
}

void LRUCache::LRU_Append(LRUHandle* e) {
  // Make "e" newest entry by inserting just before lru_
  GetSubCache(e->GetSubCacheType())->LRU_Append(e);
}

size_t LRUCache::GetSubCacheCapacity(const SubCacheType subcache_type) {
  switch (subcache_type) {
    case SINGLE_TOUCH :
      if (strict_capacity_limit_ || !FLAGS_cache_overflow_single_touch) {
        return total_capacity_ - multi_touch_capacity_;
      }
      return total_capacity_ - multi_touch_sub_cache_.Usage();
    case MULTI_TOUCH :
      return multi_touch_capacity_;
  }
  FATAL_INVALID_ENUM_VALUE(SubCacheType, subcache_type);
}

void LRUCache::EvictFromLRU(const size_t charge,
                            LRUHandleDeleter* deleted,
                            const SubCacheType subcache_type) {
  LRUSubCache* sub_cache =  GetSubCache(subcache_type);
  const size_t capacity = GetSubCacheCapacity(subcache_type);
  while (sub_cache->Usage() + charge > capacity && !sub_cache->IsLRUEmpty())  {
    LRUHandle* old = sub_cache->LRU_Head().next;
    assert(old->in_cache);
    assert(old->refs == 1);  // LRU list contains elements which may be evicted
    sub_cache->LRU_Remove(old);
    table_.Remove(old->key(), old->hash);
    old->in_cache = false;
    Unref(old);
    sub_cache->DecrementUsage(old->charge);
    deleted->Add(old);
  }
}

void LRUCache::SetCapacity(size_t capacity) {
  LRUHandleDeleter last_reference_list(metrics_.get());

  {
    MutexLock l(&mutex_);
    multi_touch_capacity_ = round((1 - FLAGS_cache_single_touch_ratio) * capacity);
    total_capacity_ = capacity;
    EvictFromLRU(0, &last_reference_list, MULTI_TOUCH);
    EvictFromLRU(0, &last_reference_list, SINGLE_TOUCH);
  }
}

void LRUCache::SetStrictCapacityLimit(bool strict_capacity_limit) {
  MutexLock l(&mutex_);
  // Allow setting strict capacity limit only when there are no elements in the cache.
  // This is because we disable overflowing single touch cache when strict_capacity_limit_ is true.
  // We cannot ensure that single touch cache has not already overflown when the cache already has
  // elements in it.
  assert(TotalUsage() == 0 || !FLAGS_cache_overflow_single_touch);
  strict_capacity_limit_ = strict_capacity_limit;
}

Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash, const QueryId query_id,
                                Statistics* statistics)  {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    assert(e->in_cache);
    // Since the entry is now referenced externally, cannot be evicted, so remove from LRU.
    if (e->refs == 1) {
      LRU_Remove(e);
    }
    // Increase the number of references and move to state 1. (in cache and not in LRU)
    e->refs++;

    // Now the handle will be added to the multi touch pool only if it exists.
    if (FLAGS_cache_single_touch_ratio < 1 && e->GetSubCacheType() != MULTI_TOUCH &&
        e->query_id != query_id) {
      {
        LRUHandleDeleter multi_touch_eviction_list(metrics_.get());
        EvictFromLRU(e->charge, &multi_touch_eviction_list, MULTI_TOUCH);
      }
      // Cannot have any single touch elements in this case.
      assert(FLAGS_cache_single_touch_ratio != 0);
      if (!strict_capacity_limit_ ||
          multi_touch_sub_cache_.Usage() - multi_touch_sub_cache_.LRU_Usage() + e->charge <=
          multi_touch_capacity_) {
        e->query_id = kInMultiTouchId;
        single_touch_sub_cache_.DecrementUsage(e->charge);
        multi_touch_sub_cache_.IncrementUsage(e->charge);
       if (metrics_) {
         metrics_->multi_touch_cache_usage->IncrementBy(e->charge);
         metrics_->single_touch_cache_usage->DecrementBy(e->charge);
       }
      }
    }
    if (statistics != nullptr) {
      // overall cache hit
      RecordTick(statistics, BLOCK_CACHE_HIT);
      // total bytes read from cache
      RecordTick(statistics, BLOCK_CACHE_BYTES_READ, e->charge);
      if (e->GetSubCacheType() == SubCacheType::SINGLE_TOUCH) {
        RecordTick(statistics, BLOCK_CACHE_SINGLE_TOUCH_HIT);
        RecordTick(statistics, BLOCK_CACHE_SINGLE_TOUCH_BYTES_READ, e->charge);
      } else if (e->GetSubCacheType() == SubCacheType::MULTI_TOUCH) {
        RecordTick(statistics, BLOCK_CACHE_MULTI_TOUCH_HIT);
        RecordTick(statistics, BLOCK_CACHE_MULTI_TOUCH_BYTES_READ, e->charge);
      }
    }
  } else {
    if (statistics != nullptr) {
      RecordTick(statistics, BLOCK_CACHE_MISS);
    }
  }

  if (metrics_ != nullptr) {
    metrics_->lookups->Increment();
    bool was_hit = (e != nullptr);
    if (was_hit) {
      metrics_->cache_hits->Increment();
    } else {
      metrics_->cache_misses->Increment();
    }
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

bool LRUCache::HasFreeSpace(const SubCacheType subcache_type) {
  switch(subcache_type) {
    case SINGLE_TOUCH :
      if (strict_capacity_limit_ || !FLAGS_cache_overflow_single_touch) {
        return single_touch_sub_cache_.Usage() <= (total_capacity_ - multi_touch_capacity_);
      }
      return TotalUsage() <= total_capacity_;
    case MULTI_TOUCH : return multi_touch_sub_cache_.Usage() <= multi_touch_capacity_;
  }
  FATAL_INVALID_ENUM_VALUE(SubCacheType, subcache_type);
}

void LRUCache::Release(Cache::Handle* handle) {
  if (handle == nullptr) {
    return;
  }
  LRUHandle* e = reinterpret_cast<LRUHandle*>(handle);
  bool last_reference = false;
  {
    MutexLock l(&mutex_);
    LRUSubCache* sub_cache = GetSubCache(e->GetSubCacheType());
    last_reference = Unref(e);
    if (last_reference) {
      sub_cache->DecrementUsage(e->charge);
    }
    if (e->refs == 1 && e->in_cache) {
      // The item is still in cache, and nobody else holds a reference to it
      if (!HasFreeSpace(e->GetSubCacheType())) {
        // The LRU list must be empty since the cache is full.
        assert(sub_cache->IsLRUEmpty());
        // take this opportunity and remove the item
        table_.Remove(e->key(), e->hash);
        e->in_cache = false;
        Unref(e);
        sub_cache->DecrementUsage(e->charge);
        last_reference = true;
      } else {
        // put the item on the list to be potentially freed.
        LRU_Append(e);
      }
    }
  }

  // free outside of mutex
  if (last_reference) {
    e->Free(metrics_.get());
  }
}

size_t LRUCache::Evict(size_t required) {
  LRUHandleDeleter evicted(metrics_.get());
  {
    MutexLock l(&mutex_);
    EvictFromLRU(required, &evicted, SINGLE_TOUCH);
    if (required > evicted.TotalCharge()) {
      EvictFromLRU(required, &evicted, MULTI_TOUCH);
    }
  }
  return evicted.TotalCharge();
}

Status LRUCache::Insert(const Slice& key, uint32_t hash, const QueryId query_id,
                        void* value, size_t charge, void (*deleter)(const Slice& key, void* value),
                        Cache::Handle** handle, Statistics* statistics) {
  // Don't use the cache if disabled by the caller using the special query id.
  if (query_id == kNoCacheQueryId) {
    return Status::OK();
  }
  // Allocate the memory here outside of the mutex
  // If the cache is full, we'll have to release it
  // It shouldn't happen very often though.
  LRUHandle* e = reinterpret_cast<LRUHandle*>(
                    new char[sizeof(LRUHandle) - 1 + key.size()]);
  Status s;
  LRUHandleDeleter last_reference_list(metrics_.get());

  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->refs = (handle == nullptr
                 ? 1
                 : 2);  // One from LRUCache, one for the returned handle
  e->next = e->prev = nullptr;
  e->in_cache = true;
  // Adding query id to the handle.
  e->query_id = query_id;
  memcpy(e->key_data, key.data(), key.size());

  {
    MutexLock l(&mutex_);
    // Free the space following strict LRU policy until enough space
    // is freed or the lru list is empty.
    // Check if there is a single touch cache.
    SubCacheType subcache_type;
    if (FLAGS_cache_single_touch_ratio == 0) {
      e->query_id = kInMultiTouchId;
      subcache_type = MULTI_TOUCH;
    } else if (FLAGS_cache_single_touch_ratio == 1) {
      // If there is no multi touch cache, default to single cache.
      subcache_type = SINGLE_TOUCH;
    } else {
      subcache_type = table_.GetSubCacheTypeCandidate(e);
    }
    EvictFromLRU(charge, &last_reference_list, subcache_type);
    LRUSubCache* sub_cache = GetSubCache(subcache_type);
    // If the cache no longer has any more space in the given pool.
    if (strict_capacity_limit_ &&
        sub_cache->Usage() - sub_cache->LRU_Usage() + charge > GetSubCacheCapacity(subcache_type)) {
      if (handle == nullptr) {
        last_reference_list.Add(e);
      } else {
        delete[] reinterpret_cast<char*>(e);
        *handle = nullptr;
      }
      s = STATUS(Incomplete, "Insert failed due to LRU cache being full.");
    } else {
      // insert into the cache
      // note that the cache might get larger than its capacity if not enough
      // space was freed
      LRUHandle* old = table_.Insert(e);
      sub_cache->IncrementUsage(e->charge);
      if (old != nullptr) {
        old->in_cache = false;
        if (Unref(old)) {
          DecrementUsage(old->GetSubCacheType(), old->charge);
          // old is on LRU because it's in cache and its reference count
          // was just 1 (Unref returned 0)
          LRU_Remove(old);
          last_reference_list.Add(old);
        }
      }
      // No external reference, so put it in LRU to be potentially evicted.
      if (handle == nullptr) {
        LRU_Append(e);
      } else {
        *handle = reinterpret_cast<Cache::Handle*>(e);
      }
      if (subcache_type == MULTI_TOUCH && FLAGS_cache_single_touch_ratio != 0) {
        // Evict entries from single touch cache if the total size increases. This can happen if
        // single touch entries has overflown and we insert entries directly into the multi touch
        // cache without it going through the single touch cache.
        EvictFromLRU(0, &last_reference_list, SINGLE_TOUCH);
      }
      s = Status::OK();
    }
    if (statistics != nullptr) {
      if (s.ok()) {
        RecordTick(statistics, BLOCK_CACHE_ADD);
        RecordTick(statistics, BLOCK_CACHE_BYTES_WRITE, charge);
        if (subcache_type == SubCacheType::SINGLE_TOUCH) {
          RecordTick(statistics, BLOCK_CACHE_SINGLE_TOUCH_ADD);
          RecordTick(statistics, BLOCK_CACHE_SINGLE_TOUCH_BYTES_WRITE, charge);
        } else if (subcache_type == SubCacheType::MULTI_TOUCH) {
          RecordTick(statistics, BLOCK_CACHE_MULTI_TOUCH_ADD);
          RecordTick(statistics, BLOCK_CACHE_MULTI_TOUCH_BYTES_WRITE, charge);
        }
      } else {
        RecordTick(statistics, BLOCK_CACHE_ADD_FAILURES);
      }
    }
    if (metrics_ != nullptr) {
      if (subcache_type == MULTI_TOUCH) {
        metrics_->multi_touch_cache_usage->IncrementBy(charge);
      } else {
        metrics_->single_touch_cache_usage->IncrementBy(charge);
      }
      metrics_->cache_usage->IncrementBy(charge);
    }
  }

  return s;
}

void LRUCache::Erase(const Slice& key, uint32_t hash) {
  LRUHandle* e;
  bool last_reference = false;
  {
    MutexLock l(&mutex_);
    e = table_.Remove(key, hash);
    if (e != nullptr) {
      last_reference = Unref(e);
      if (last_reference) {
        DecrementUsage(e->GetSubCacheType(), e->charge);
      }
      if (last_reference && e->in_cache) {
        LRU_Remove(e);
      }
      e->in_cache = false;
    }
  }
  // mutex not held here
  // last_reference will only be true if e != nullptr
  if (last_reference) {
    e->Free(metrics_.get());
  }
}

static int kNumShardBits = 4;          // default values, can be overridden

class ShardedLRUCache : public Cache {
 private:
  LRUCache* shards_;
  port::Mutex id_mutex_;
  port::Mutex capacity_mutex_;
  uint64_t last_id_;
  size_t num_shard_bits_;
  size_t capacity_;
  bool strict_capacity_limit_;
  shared_ptr<yb::CacheMetrics> metrics_;

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  uint32_t Shard(uint32_t hash) {
    // Note, hash >> 32 yields hash in gcc, not the zero we expect!
    return (num_shard_bits_ > 0) ? (hash >> (32 - num_shard_bits_)) : 0;
  }

  bool IsValidQueryId(const QueryId query_id) {
    return query_id >= 0 || query_id == kInMultiTouchId || query_id == kNoCacheQueryId;
  }

 public:
  ShardedLRUCache(size_t capacity, int num_shard_bits,
                  bool strict_capacity_limit)
      : last_id_(0),
        num_shard_bits_(num_shard_bits),
        capacity_(capacity),
        strict_capacity_limit_(strict_capacity_limit),
        metrics_(nullptr) {
    int num_shards = 1 << num_shard_bits_;
    shards_ = new LRUCache[num_shards];
    const size_t per_shard = (capacity + (num_shards - 1)) / num_shards;
    for (int s = 0; s < num_shards; s++) {
      shards_[s].SetStrictCapacityLimit(strict_capacity_limit);
      shards_[s].SetCapacity(per_shard);
    }
  }

  virtual ~ShardedLRUCache() {
    delete[] shards_;
  }

  void SetCapacity(size_t capacity) override {
    int num_shards = 1 << num_shard_bits_;
    const size_t per_shard = (capacity + (num_shards - 1)) / num_shards;
    MutexLock l(&capacity_mutex_);
    for (int s = 0; s < num_shards; s++) {
      shards_[s].SetCapacity(per_shard);
    }
    capacity_ = capacity;
  }

  virtual Status Insert(const Slice& key, const QueryId query_id, void* value, size_t charge,
                        void (*deleter)(const Slice& key, void* value),
                        Handle** handle, Statistics* statistics) override {
    DCHECK(IsValidQueryId(query_id));
    // Queries with no cache query ids are not cached.
    if (query_id == kNoCacheQueryId) {
      return Status::OK();
    }
    const uint32_t hash = HashSlice(key);
    return shards_[Shard(hash)].Insert(key, hash, query_id, value, charge, deleter,
                                       handle, statistics);
  }

  size_t Evict(size_t bytes_to_evict) override {
    auto num_shards = 1ULL << num_shard_bits_;
    size_t total_evicted = 0;
    // Start at random shard.
    auto index = Shard(yb::RandomUniformInt<uint32_t>());
    for (size_t i = 0; bytes_to_evict > total_evicted && i != num_shards; ++i) {
      total_evicted += shards_[index].Evict(bytes_to_evict - total_evicted);
      index = (index + 1) & (num_shards - 1);
    }
    return total_evicted;
  }

  Handle* Lookup(const Slice& key, const QueryId query_id, Statistics* statistics) override {
    DCHECK(IsValidQueryId(query_id));
    if (query_id == kNoCacheQueryId) {
      return nullptr;
    }
    const uint32_t hash = HashSlice(key);
    return shards_[Shard(hash)].Lookup(key, hash, query_id, statistics);
  }

  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shards_[Shard(h->hash)].Release(handle);
  }

  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shards_[Shard(hash)].Erase(key, hash);
  }

  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }

  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }

  size_t GetCapacity() const override { return capacity_; }

  bool HasStrictCapacityLimit() const override {
    return strict_capacity_limit_;
  }

  size_t GetUsage() const override {
    // We will not lock the cache when getting the usage from shards.
    int num_shards = 1 << num_shard_bits_;
    size_t usage = 0;
    for (int s = 0; s < num_shards; s++) {
      usage += shards_[s].GetUsage();
    }
    return usage;
  }

  size_t GetUsage(Handle* handle) const override {
    return reinterpret_cast<LRUHandle*>(handle)->charge;
  }

  size_t GetPinnedUsage() const override {
    // We will not lock the cache when getting the usage from shards.
    int num_shards = 1 << num_shard_bits_;
    size_t usage = 0;
    for (int s = 0; s < num_shards; s++) {
      usage += shards_[s].GetPinnedUsage();
    }
    return usage;
  }

  SubCacheType GetSubCacheType(Handle* e) const override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(e);
    return h->GetSubCacheType();
  }

  void DisownData() override {
    shards_ = nullptr;
  }

  virtual void ApplyToAllCacheEntries(void (*callback)(void*, size_t),
                                      bool thread_safe) override {
    int num_shards = 1 << num_shard_bits_;
    for (int s = 0; s < num_shards; s++) {
      shards_[s].ApplyToAllCacheEntries(callback, thread_safe);
    }
  }

  virtual void SetMetrics(const scoped_refptr<yb::MetricEntity>& entity) override {
    int num_shards = 1 << num_shard_bits_;
    metrics_ = std::make_shared<yb::CacheMetrics>(entity);
    for (int s = 0; s < num_shards; s++) {
      shards_[s].SetMetrics(metrics_);
    }
  }

  virtual std::vector<std::pair<size_t,size_t>> TEST_GetIndividualUsages() override {
    std::vector<std::pair<size_t, size_t>> cache_sizes;
    cache_sizes.reserve(1 << num_shard_bits_);

    for (int i = 0; i < 1 << num_shard_bits_; ++i) {
      cache_sizes.emplace_back(shards_[i].TEST_GetIndividualUsages());
    }
    return cache_sizes;
  }
};

}  // end anonymous namespace

shared_ptr<Cache> NewLRUCache(size_t capacity) {
  return NewLRUCache(capacity, kNumShardBits, false);
}

shared_ptr<Cache> NewLRUCache(size_t capacity, int num_shard_bits) {
  return NewLRUCache(capacity, num_shard_bits, false);
}

shared_ptr<Cache> NewLRUCache(size_t capacity, int num_shard_bits,
                              bool strict_capacity_limit) {
  if (num_shard_bits >= 20) {
    return nullptr;  // the cache cannot be sharded into too many fine pieces
  }
  return std::make_shared<ShardedLRUCache>(capacity, num_shard_bits,
                                           strict_capacity_limit);
}

}  // namespace rocksdb
