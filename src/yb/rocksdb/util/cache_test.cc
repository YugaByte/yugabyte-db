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

#include "yb/rocksdb/cache.h"

#include <forward_list>
#include <vector>
#include <string>
#include <iostream>
#include <gflags/gflags.h>
#include "yb/rocksdb/util/coding.h"
#include "yb/util/string_util.h"
#include "yb/rocksdb/util/testharness.h"

DECLARE_double(cache_single_touch_ratio);

namespace rocksdb {

// Conversions between numeric keys/values and the types expected by Cache.
static std::string EncodeKey(int k) {
  std::string result;
  PutFixed32(&result, k);
  return result;
}
static int DecodeKey(const Slice& k) {
  assert(k.size() == 4);
  return DecodeFixed32(k.data());
}
static void* EncodeValue(uintptr_t v) { return reinterpret_cast<void*>(v); }
static int DecodeValue(void* v) {
  return static_cast<int>(reinterpret_cast<uintptr_t>(v));
}

class CacheTest : public testing::Test {
 public:
  static CacheTest* current_;

  static void Deleter(const Slice& key, void* v) {
    current_->deleted_keys_.push_back(DecodeKey(key));
    current_->deleted_values_.push_back(DecodeValue(v));
  }

  static const int kCacheSize = 1000;
  static const int kNumShardBits = 4;

  static const int kCacheSize2 = 100;
  static const int kNumShardBits2 = 2;

  static const QueryId kTestQueryId = 1;

  std::vector<int> deleted_keys_;
  std::vector<int> deleted_values_;
  shared_ptr<Cache> cache_;
  shared_ptr<Cache> cache2_;

  CacheTest() :
      cache_(NewLRUCache(kCacheSize, kNumShardBits)),
      cache2_(NewLRUCache(kCacheSize2, kNumShardBits2)) {
    current_ = this;
  }

  ~CacheTest() {
  }

  int Lookup(shared_ptr<Cache> cache, int key, QueryId query_id = kTestQueryId) {
    Cache::Handle* handle = cache->Lookup(EncodeKey(key), query_id);
    const int r = (handle == nullptr) ? -1 : DecodeValue(cache->Value(handle));
    if (handle != nullptr) {
      cache->Release(handle);
    }
    return r;
  }

  bool LookupAndCheckInMultiTouch(shared_ptr<Cache> cache, int key,
                                  int expected_value,
                                  QueryId query_id = kTestQueryId) {
    Cache::Handle* handle = cache->Lookup(EncodeKey(key), query_id);
    if (handle != nullptr) {
      const int r = DecodeValue(cache->Value(handle));
      SubCacheType subcache_type = cache->GetSubCacheType(handle);
      cache->Release(handle);
      return (subcache_type == MULTI_TOUCH && r == expected_value);
    } else {
      return false;
    }
  }

  Status Insert(shared_ptr<Cache> cache, int key, int value, int charge = 1,
                QueryId query_id = kTestQueryId) {
    return cache->Insert(EncodeKey(key), query_id, EncodeValue(value), charge,
                         &CacheTest::Deleter);
  }

  void Erase(shared_ptr<Cache> cache, int key) {
    cache->Erase(EncodeKey(key));
  }


  int Lookup(int key, QueryId query_id = kTestQueryId) {
    return Lookup(cache_, key, query_id);
  }

  bool LookupAndCheckInMultiTouch(int key, int expected_value, QueryId query_id = kTestQueryId) {
    return LookupAndCheckInMultiTouch(cache_, key, expected_value, query_id);
  }

  Status Insert(int key, int value, int charge = 1, QueryId query_id = kTestQueryId) {
    return Insert(cache_, key, value, charge, query_id);
  }

  void Erase(int key) {
    Erase(cache_, key);
  }

  int Lookup2(int key) {
    return Lookup(cache2_, key);
  }

  Status Insert2(int key, int value, int charge = 1) {
    return Insert(cache2_, key, value, charge);
  }

  void Erase2(int key) {
    Erase(cache2_, key);
  }
};
CacheTest* CacheTest::current_;

namespace {
void dumbDeleter(const Slice& key, void* value) { }
}  // namespace

TEST_F(CacheTest, UsageTest) {
  // cache is shared_ptr and will be automatically cleaned up.
  const uint64_t kCapacity = 100000;
  auto cache = NewLRUCache(kCapacity, 8);

  size_t usage = 0;
  char value[10] = "abcdef";
  // make sure everything will be cached
  for (int i = 1; i < 100; ++i) {
    std::string key(i, 'a');
    auto kv_size = key.size() + 5;
    ASSERT_OK(cache->Insert(key, kTestQueryId, reinterpret_cast<void*>(value),
                            kv_size, dumbDeleter));
    usage += kv_size;
    ASSERT_EQ(usage, cache->GetUsage());
  }

  // make sure the cache will be overloaded
  for (uint64_t i = 1; i < kCapacity; ++i) {
    auto key = ToString(i);
    ASSERT_OK(cache->Insert(
        key, kTestQueryId, reinterpret_cast<void*>(value), key.size() + 5, dumbDeleter));
  }

  // the usage should be close to the capacity
  ASSERT_GT(kCapacity, cache->GetUsage());
  ASSERT_LT(kCapacity * 0.95, cache->GetUsage());
}

TEST_F(CacheTest, PinnedUsageTest) {
  // cache is shared_ptr and will be automatically cleaned up.
  const uint64_t kCapacity = 100000;
  auto cache = NewLRUCache(kCapacity / FLAGS_cache_single_touch_ratio, 8);

  size_t pinned_usage = 0;
  char value[10] = "abcdef";

  std::forward_list<Cache::Handle*> unreleased_handles;

  // Add entries. Unpin some of them after insertion. Then, pin some of them
  // again. Check GetPinnedUsage().
  for (int i = 1; i < 100; ++i) {
    std::string key(i, 'a');
    auto kv_size = key.size() + 5;
    Cache::Handle* handle;
    ASSERT_OK(cache->Insert(
        key, kTestQueryId, reinterpret_cast<void*>(value), kv_size, dumbDeleter, &handle));
    pinned_usage += kv_size;
    ASSERT_EQ(pinned_usage, cache->GetPinnedUsage());
    if (i % 2 == 0) {
      cache->Release(handle);
      pinned_usage -= kv_size;
      ASSERT_EQ(pinned_usage, cache->GetPinnedUsage());
    } else {
      unreleased_handles.push_front(handle);
    }
    if (i % 3 == 0) {
      unreleased_handles.push_front(cache->Lookup(key, kTestQueryId));
      // If i % 2 == 0, then the entry was unpinned before Lookup, so pinned
      // usage increased
      if (i % 2 == 0) {
        pinned_usage += kv_size;
      }
      ASSERT_EQ(pinned_usage, cache->GetPinnedUsage());
    }
  }

  // check that overloading the cache does not change the pinned usage
  for (uint64_t i = 1; i < 2 * kCapacity; ++i) {
    auto key = ToString(i);
    ASSERT_OK(cache->Insert(
        key, kTestQueryId, reinterpret_cast<void*>(value), key.size() + 5, dumbDeleter));
  }
  ASSERT_EQ(pinned_usage, cache->GetPinnedUsage());

  // release handles for pinned entries to prevent memory leaks
  for (auto handle : unreleased_handles) {
    cache->Release(handle);
  }
}

TEST_F(CacheTest, HitAndMiss) {
  ASSERT_EQ(-1, Lookup(100));

  ASSERT_OK(Insert(100, 101));
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1,  Lookup(200));
  ASSERT_EQ(-1,  Lookup(300));

  ASSERT_OK(Insert(200, 201));
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(-1,  Lookup(300));

  ASSERT_OK(Insert(100, 102));
  ASSERT_EQ(102, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(-1,  Lookup(300));

  ASSERT_EQ(1U, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);
}


TEST_F(CacheTest, Erase) {
  Erase(200);
  ASSERT_EQ(0U, deleted_keys_.size());

  ASSERT_OK(Insert(100, 101));
  ASSERT_OK(Insert(200, 201));
  Erase(100);
  ASSERT_EQ(-1,  Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1U, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);

  Erase(100);
  ASSERT_EQ(-1,  Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1U, deleted_keys_.size());
}

TEST_F(CacheTest, EntriesArePinned) {
  ASSERT_OK(Insert(100, 101));
  Cache::Handle* h1 = cache_->Lookup(EncodeKey(100), kTestQueryId);
  ASSERT_EQ(101, DecodeValue(cache_->Value(h1)));
  ASSERT_EQ(1U, cache_->GetUsage());

  ASSERT_OK(Insert(100, 102));
  Cache::Handle* h2 = cache_->Lookup(EncodeKey(100), kTestQueryId);
  ASSERT_EQ(102, DecodeValue(cache_->Value(h2)));
  ASSERT_EQ(0U, deleted_keys_.size());
  ASSERT_EQ(2U, cache_->GetUsage());

  cache_->Release(h1);
  ASSERT_EQ(1U, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);
  ASSERT_EQ(1U, cache_->GetUsage());

  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(1U, deleted_keys_.size());
  ASSERT_EQ(1U, cache_->GetUsage());

  cache_->Release(h2);
  ASSERT_EQ(2U, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[1]);
  ASSERT_EQ(102, deleted_values_[1]);
  ASSERT_EQ(0U, cache_->GetUsage());
}

TEST_F(CacheTest, EvictionPolicy) {
  ASSERT_OK(Insert(100, 101));
  ASSERT_OK(Insert(200, 201));

  // Frequently used entry must be kept around
  for (int i = 0; i < kCacheSize + 100; i++) {
    ASSERT_OK(Insert(1000 + i, 2000 + i));
    ASSERT_EQ(2000 + i, Lookup(1000 + i));
    ASSERT_EQ(101, Lookup(100));
  }
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1, Lookup(200));
}

TEST_F(CacheTest, EvictionPolicyRef) {
  ASSERT_OK(Insert(100, 101));
  ASSERT_OK(Insert(101, 102));
  ASSERT_OK(Insert(102, 103));
  ASSERT_OK(Insert(103, 104));
  ASSERT_OK(Insert(200, 101));
  ASSERT_OK(Insert(201, 102));
  ASSERT_OK(Insert(202, 103));
  ASSERT_OK(Insert(203, 104));
  Cache::Handle* h201 = cache_->Lookup(EncodeKey(200), kTestQueryId);
  Cache::Handle* h202 = cache_->Lookup(EncodeKey(201), kTestQueryId);
  Cache::Handle* h203 = cache_->Lookup(EncodeKey(202), kTestQueryId);
  Cache::Handle* h204 = cache_->Lookup(EncodeKey(203), kTestQueryId);
  ASSERT_OK(Insert(300, 101));
  ASSERT_OK(Insert(301, 102));
  ASSERT_OK(Insert(302, 103));
  ASSERT_OK(Insert(303, 104));

  // Insert entries much more than Cache capacity
  for (int i = 0; i < kCacheSize + 100; i++) {
    ASSERT_OK(Insert(1000 + i, 2000 + i));
  }

  // Check whether the entries inserted in the beginning
  // are evicted. Ones without extra ref are evicted and
  // those with are not.
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(-1, Lookup(101));
  ASSERT_EQ(-1, Lookup(102));
  ASSERT_EQ(-1, Lookup(103));

  ASSERT_EQ(-1, Lookup(300));
  ASSERT_EQ(-1, Lookup(301));
  ASSERT_EQ(-1, Lookup(302));
  ASSERT_EQ(-1, Lookup(303));

  ASSERT_EQ(101, Lookup(200));
  ASSERT_EQ(102, Lookup(201));
  ASSERT_EQ(103, Lookup(202));
  ASSERT_EQ(104, Lookup(203));

  // Cleaning up all the handles
  cache_->Release(h201);
  cache_->Release(h202);
  cache_->Release(h203);
  cache_->Release(h204);
}

TEST_F(CacheTest, ErasedHandleState) {
  // insert a key and get two handles
  ASSERT_OK(Insert(100, 1000));
  Cache::Handle* h1 = cache_->Lookup(EncodeKey(100), kTestQueryId);
  Cache::Handle* h2 = cache_->Lookup(EncodeKey(100), kTestQueryId);
  ASSERT_EQ(h1, h2);
  ASSERT_EQ(DecodeValue(cache_->Value(h1)), 1000);
  ASSERT_EQ(DecodeValue(cache_->Value(h2)), 1000);

  // delete the key from the cache
  Erase(100);
  // can no longer find in the cache
  ASSERT_EQ(-1, Lookup(100));

  // release one handle
  cache_->Release(h1);
  // still can't find in cache
  ASSERT_EQ(-1, Lookup(100));

  cache_->Release(h2);
}

TEST_F(CacheTest, EvictionPolicyAllSingleTouch) {
  FLAGS_cache_single_touch_ratio = 1;
  const int kCapacity = 100;
  auto cache = NewLRUCache(kCapacity, 0, true);
  Status s;
  for (int i = 0; i < kCapacity; i++) {
    ASSERT_OK(Insert(cache, i, i + 1, kTestQueryId));
  }

  // Check that all values are in the cache and are all single touch.
  for (int i = 0; i < kCapacity; i++) {
    ASSERT_EQ(i + 1, Lookup(cache, i, kTestQueryId));
    ASSERT_FALSE(LookupAndCheckInMultiTouch(cache, i, i + 1));
  }
  ASSERT_EQ(kCapacity, cache->GetUsage());

  // Returning the flag back.
  FLAGS_cache_single_touch_ratio = 0.2;
}

TEST_F(CacheTest, EvictionPolicyNoSingleTouch) {
  FLAGS_cache_single_touch_ratio = 0;
  const int kCapacity = 100;
  auto cache = NewLRUCache(kCapacity , 0, true);
  Status s;
  for (int i = 0; i < kCapacity; i++) {
    ASSERT_OK(Insert(cache, i, i + 1, kTestQueryId));
  }

  // Check that all values are in the cache and are all multi touch.
  for (int i = 0; i < kCapacity; i++) {
    ASSERT_EQ(i + 1, Lookup(cache, i, kTestQueryId));
    ASSERT_TRUE(LookupAndCheckInMultiTouch(cache, i, i + 1));
  }
  ASSERT_EQ(kCapacity, cache->GetUsage());

  // Returning the flag back.
  FLAGS_cache_single_touch_ratio = 0.2;
}

TEST_F(CacheTest, MultiTouch) {
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_FALSE(LookupAndCheckInMultiTouch(100, -1));

  // Use two different query ids.
  QueryId qid1 = 1000;
  QueryId qid2 = 1001;

  ASSERT_OK(Insert(100, 101, 1, qid1));
  ASSERT_FALSE(LookupAndCheckInMultiTouch(100, 101, qid1));
  ASSERT_OK(Insert(100, 101, 1, qid2));
  ASSERT_TRUE(LookupAndCheckInMultiTouch(100, 101, qid2));
}

TEST_F(CacheTest, EvictionPolicyMultiTouch) {
  QueryId qid1 = 1000;
  QueryId qid2 = 1001;
  QueryId qid3 = 1002;
  ASSERT_OK(Insert(100, 101, 1, qid1));
  ASSERT_OK(Insert(100, 101, 1, qid2));
  ASSERT_TRUE(LookupAndCheckInMultiTouch(100, 101, qid2));
  ASSERT_OK(Insert(200, 201, 1, qid3));
  ASSERT_FALSE(LookupAndCheckInMultiTouch(200, 201, qid3));

  // We are adding a single element (key: 100, value: 101) to multi touch cache.
  // Even if we overload the cache with single touch items, multi touch item should not be evicted.
  for (int i = 0; i < kCacheSize + 100; i++) {
    ASSERT_OK(Insert(1000 + i, 2000 + i));
    ASSERT_EQ(2000 + i, Lookup(1000 + i));
  }
  ASSERT_TRUE(LookupAndCheckInMultiTouch(100, 101, qid2));
  ASSERT_EQ(-1, Lookup(200));
  ASSERT_LT(kCacheSize * FLAGS_cache_single_touch_ratio, cache_->GetUsage());
}

TEST_F(CacheTest, HeavyEntries) {
  // Add a bunch of light and heavy entries and then count the combined
  // size of items still in the cache, which must be approximately the
  // same as the total capacity.
  const int kLight = 1;
  const int kHeavy = 10;
  int added = 0;
  int index = 0;
  while (added < 2*kCacheSize) {
    const int weight = (index & 1) ? kLight : kHeavy;
    ASSERT_OK(Insert(index, 1000 + index, weight));
    added += weight;
    index++;
  }

  int cached_weight = 0;
  for (int i = 0; i < index; i++) {
    const int weight = (i & 1 ? kLight : kHeavy);
    int r = Lookup(i);
    if (r >= 0) {
      cached_weight += weight;
      ASSERT_EQ(1000 + i, r);
    }
  }
  ASSERT_LE(cached_weight, kCacheSize + kCacheSize/10);
}

TEST_F(CacheTest, NewId) {
  uint64_t a = cache_->NewId();
  uint64_t b = cache_->NewId();
  ASSERT_NE(a, b);
}


class Value {
 private:
  size_t v_;
 public:
  explicit Value(size_t v) : v_(v) { }

  ~Value() { std::cout << v_ << " is destructed\n"; }
};

namespace {
void deleter(const Slice& key, void* value) {
  delete static_cast<Value *>(value);
}
}  // namespace

TEST_F(CacheTest, SetCapacity) {
  // test1: increase capacity
  // lets create a cache with capacity 10.
  std::shared_ptr<Cache> cache = NewLRUCache(10, 0);
  std::vector<Cache::Handle*> handles(16);

  // Insert 8 entries, but not releasing.
  for (size_t i = 0; i < 8; i++) {
    std::string key = ToString(i + 1);
    Status s = cache->Insert(key, kTestQueryId, new Value(i + 1), 1, &deleter, &handles[i]);
    ASSERT_TRUE(s.ok());
  }
  ASSERT_EQ(10U, cache->GetCapacity());
  ASSERT_EQ(8U, cache->GetUsage());
  cache->SetCapacity(20);
  ASSERT_EQ(20U, cache->GetCapacity());
  ASSERT_EQ(8U, cache->GetUsage());

  // test2: decrease capacity
  // insert 8 more elements to cache, then release 8,
  // then decrease capacity to 15.
  // and usage should be 15 from all singles
  for (size_t i = 8; i < 16; i++) {
    std::string key = ToString(i+1);
    Status s = cache->Insert(key, kTestQueryId, new Value(i + 1), 1, &deleter, &handles[i]);
    ASSERT_TRUE(s.ok());
  }

  ASSERT_EQ(20U, cache->GetCapacity());
  ASSERT_EQ(16U, cache->GetUsage());
  for (size_t i = 0; i < 8; i++) {
    cache->Release(handles[i]);
  }
  ASSERT_EQ(20U, cache->GetCapacity());
  ASSERT_EQ(16U, cache->GetUsage());
  cache->SetCapacity(15);
  ASSERT_EQ(15, cache->GetCapacity());
  ASSERT_EQ(15, cache->GetUsage());

  // release remaining 8 to keep valgrind happy
  for (size_t i = 8; i < 16; i++) {
    cache->Release(handles[i]);
  }
}

TEST_F(CacheTest, SetStrictCapacityLimit) {
  std::shared_ptr<Cache> cache = NewLRUCache(10, 0, true);
  std::vector<Cache::Handle*> handles(2);
  Status s;

  for (size_t i = 0; i < 2; i++) {
    std::string key = ToString(i + 1);
    s = cache->Insert(key, kTestQueryId, new Value(i + 1), 1, &deleter, &handles[i]);
    ASSERT_TRUE(s.ok());
    ASSERT_NE(nullptr, handles[i]);
  }

  Cache::Handle* handle;
  std::string extra_key = "extra";
  Value* extra_value = new Value(0);

  s = cache->Insert(extra_key, kTestQueryId, extra_value, 1, &deleter, &handle);
  ASSERT_TRUE(s.IsIncomplete());
  ASSERT_EQ(nullptr, handle);
  // test insert without handle
  s = cache->Insert(extra_key, kTestQueryId, extra_value, 1, &deleter);
  ASSERT_TRUE(s.IsIncomplete());
  ASSERT_EQ(2, cache->GetUsage());

  for (size_t i = 0; i < 2; i++) {
    cache->Release(handles[i]);
  }
}

TEST_F(CacheTest, OverCapacity) {
  size_t cache_size = 50;
  size_t single_touch_cache_size = cache_size;

  // a LRUCache with n entries and one shard only
  std::shared_ptr<Cache> cache = NewLRUCache(cache_size, 0);

  std::vector<Cache::Handle*> handles(cache_size + 1);

  // Insert cache_size + 1 entries, but not releasing.
  for (size_t i = 0; i < single_touch_cache_size + 1; i++) {
    std::string key = ToString(i + 1);
    Status s = cache->Insert(key, kTestQueryId, new Value(i + 1), 1, &deleter, &handles[i]);
    ASSERT_TRUE(s.ok());
  }

  // Guess what's in the cache now?
  for (size_t i = 0; i < single_touch_cache_size + 1; i++) {
    std::string key = ToString(i + 1);
    auto h = cache->Lookup(key, kTestQueryId);
    std::cout << key << (h?" found\n":" not found\n");
    ASSERT_TRUE(h != nullptr);
    if (h) cache->Release(h);
  }

  // the cache is over capacity since nothing could be evicted
  ASSERT_EQ(single_touch_cache_size + 1U, cache->GetUsage());
  for (size_t i = 0; i < single_touch_cache_size + 1; i++) {
    cache->Release(handles[i]);
  }

  // cache is under capacity now since elements were released
  ASSERT_EQ(single_touch_cache_size, cache->GetUsage());

  // element 0 is evicted and the rest is there
  // This is consistent with the LRU policy since the element 0
  // was released first
  for (size_t i = 0; i < single_touch_cache_size + 1; i++) {
    std::string key = ToString(i + 1);
    auto h = cache->Lookup(key, kTestQueryId);
    if (h) {
      ASSERT_NE(i, 0U);
      cache->Release(h);
    } else {
      ASSERT_EQ(i, 0U);
    }
  }
}

namespace {
std::vector<std::pair<int, int>> callback_state;
void callback(void* entry, size_t charge) {
  callback_state.push_back({DecodeValue(entry), static_cast<int>(charge)});
}
};

TEST_F(CacheTest, ApplyToAllCacheEntiresTest) {
  std::vector<std::pair<int, int>> inserted;
  callback_state.clear();

  for (int i = 0; i < 10; ++i) {
    ASSERT_OK(Insert(i, i * 2, i + 1));
    inserted.push_back({i * 2, i + 1});
  }
  cache_->ApplyToAllCacheEntries(callback, true);

  sort(inserted.begin(), inserted.end());
  sort(callback_state.begin(), callback_state.end());
  ASSERT_TRUE(inserted == callback_state);
}

void AssertCacheSizes(Cache *cache, size_t single_touch_count, size_t multi_touch_count) {
  std::vector<pair<size_t, size_t>> usages = cache->TEST_GetIndividualUsages();
  ASSERT_EQ(usages.size(), 1);
  ASSERT_EQ(usages[0].first, single_touch_count);
  ASSERT_EQ(usages[0].second, multi_touch_count);
}

CHECKED_STATUS InsertIntoCache(std::shared_ptr<Cache>& cache, int key, int value,
                       int query_id = CacheTest::kTestQueryId, int charge = 1,
                       Cache::Handle **handle = nullptr) {
  return cache->Insert(ToString(key), query_id, new Value(value), charge, &deleter, handle);
}

TEST_F(CacheTest, OverFlowTest) {
  FLAGS_cache_single_touch_ratio = 0.2;
  size_t cache_size = 10;
  std::shared_ptr<Cache> cache = NewLRUCache(cache_size, 0);

  // Insert single touch entries until the max.
  for (int i = 0; i <= 9; i++) {
    ASSERT_OK(InsertIntoCache(cache, i /* key */, i /* value */));
  }
  AssertCacheSizes(cache.get(), 10, 0);

  // Insert a new element and make sure that the size is still the same.
  {
    ASSERT_OK(InsertIntoCache(cache, 10 /* key */, 10 /* value */));

    // Make sure that entry '0' is not present.
    Cache::Handle* h = cache->Lookup(ToString(0), 100 /* query_id */);
    ASSERT_EQ(h, nullptr);
  }
  AssertCacheSizes(cache.get(), 10, 0);

  // Push the min number of elements to multi-touch cache.
  for (int i = 1; i <= 2; ++i) {
    Cache::Handle* h = cache->Lookup(ToString(i), 100 /* query_id */);
    cache->Release(h);
  }
  AssertCacheSizes(cache.get(), 8, 2);

  // Perform Lookups for elements in the single-touch cache to move them to multi-touch cache.
  for (int i = 3; i <= 8; ++i) {
    Cache::Handle* h = cache->Lookup(ToString(i), 100 /* query_id */);
    cache->Release(h);
  }
  AssertCacheSizes(cache.get(), 2, 8);

  // Perform more lookup and make sure that the size of multi-touch doesn't grow.
  {
    Cache::Handle* h = cache->Lookup(ToString(9), 100 /* query_id */);
    cache->Release(h);
    AssertCacheSizes(cache.get(), 1, 8);
  }
  {
    Cache::Handle* h = cache->Lookup(ToString(10), 100 /* query_id */);
    cache->Release(h);
    AssertCacheSizes(cache.get(), 0, 8);
  }

  // Perform 2 more insertions to make sure we can insert again.
  for (int i = 11; i <= 12; ++i) {
    ASSERT_OK(InsertIntoCache(cache, i /* key */, i /* value */));
  }
  AssertCacheSizes(cache.get(), 2, 8);

  // Make sure that direct insertion into the multi-touch cache works properly.
  FLAGS_cache_single_touch_ratio = 0.4;
  cache_size = 5;
  cache = NewLRUCache(cache_size, 0);

  for (int i = 1; i <= 5; ++i) {
    ASSERT_OK(InsertIntoCache(cache, i /* key */, i /* value */));
  }
  Cache::Handle *h;
  ASSERT_OK(InsertIntoCache(cache, 9 /* key */, 9 /* value */, CacheTest::kTestQueryId, 1, &h));

  // Insert elements directly into the multi-touch pool
  for (int i = 6; i <= 8; ++i) {
    ASSERT_OK(InsertIntoCache(cache, i /* key */, i /* value */, -1 /* query_id */));
  }
  cache->Release(h);
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
