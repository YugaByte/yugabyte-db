// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
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

#include <map>
#include <memory>
#include <vector>

#include "yb/rocksdb/comparator.h"
#include "yb/rocksdb/iterator.h"
#include "yb/rocksdb/slice_transform.h"
#include "yb/rocksdb/table/block_hash_index.h"
#include "yb/rocksdb/table/internal_iterator.h"
#include "yb/rocksdb/util/testharness.h"
#include "yb/rocksdb/util/testutil.h"

namespace rocksdb {

typedef std::map<std::string, std::string> Data;

class MapIterator : public InternalIterator {
 public:
  explicit MapIterator(const Data& data) : data_(data), pos_(data_.end()) {}

  bool Valid() const override { return pos_ != data_.end(); }

  void SeekToFirst() override { pos_ = data_.begin(); }

  void SeekToLast() override {
    pos_ = data_.end();
    --pos_;
  }

  void Seek(const Slice& target) override {
    pos_ = data_.find(target.ToString());
  }

  void Next() override { ++pos_; }

  void Prev() override { --pos_; }

  Slice key() const override { return pos_->first; }

  Slice value() const override { return pos_->second; }

  Status status() const override { return Status::OK(); }

 private:
  const Data& data_;
  Data::const_iterator pos_;
};

class BlockTest : public testing::Test {};

TEST_F(BlockTest, BasicTest) {
  const size_t keys_per_block = 4;
  const size_t prefix_size = 2;
  std::vector<std::string> keys = {/* block 1 */
                                   "0101", "0102", "0103", "0201",
                                   /* block 2 */
                                   "0202", "0203", "0301", "0401",
                                   /* block 3 */
                                   "0501", "0601", "0701", "0801",
                                   /* block 4 */
                                   "0802", "0803", "0804", "0805",
                                   /* block 5 */
                                   "0806", "0807", "0808", "0809", };

  Data data_entries;
  for (const auto& key : keys) {
    data_entries.insert({key, key});
  }

  Data index_entries;
  for (size_t i = 3; i < keys.size(); i += keys_per_block) {
    // simply ignore the value part
    index_entries.insert({keys[i], ""});
  }

  MapIterator data_iter(data_entries);
  MapIterator index_iter(index_entries);

  auto prefix_extractor = NewFixedPrefixTransform(prefix_size);
  std::unique_ptr<BlockHashIndex> block_hash_index(CreateBlockHashIndexOnTheFly(
      &index_iter, &data_iter, static_cast<uint32_t>(index_entries.size()),
      BytewiseComparator(), prefix_extractor));

  std::map<std::string, BlockHashIndex::RestartIndex> expected = {
      {"01xx", BlockHashIndex::RestartIndex(0, 1)},
      {"02yy", BlockHashIndex::RestartIndex(0, 2)},
      {"03zz", BlockHashIndex::RestartIndex(1, 1)},
      {"04pp", BlockHashIndex::RestartIndex(1, 1)},
      {"05ww", BlockHashIndex::RestartIndex(2, 1)},
      {"06xx", BlockHashIndex::RestartIndex(2, 1)},
      {"07pp", BlockHashIndex::RestartIndex(2, 1)},
      {"08xz", BlockHashIndex::RestartIndex(2, 3)}, };

  const BlockHashIndex::RestartIndex* index = nullptr;
  // search existed prefixes
  for (const auto& item : expected) {
    index = block_hash_index->GetRestartIndex(item.first);
    ASSERT_TRUE(index != nullptr);
    ASSERT_EQ(item.second.first_index, index->first_index);
    ASSERT_EQ(item.second.num_blocks, index->num_blocks);
  }

  // search non exist prefixes
  ASSERT_TRUE(!block_hash_index->GetRestartIndex("00xx"));
  ASSERT_TRUE(!block_hash_index->GetRestartIndex("10yy"));
  ASSERT_TRUE(!block_hash_index->GetRestartIndex("20zz"));

  delete prefix_extractor;
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
