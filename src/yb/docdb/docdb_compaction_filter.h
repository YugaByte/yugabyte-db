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

#ifndef YB_DOCDB_DOCDB_COMPACTION_FILTER_H
#define YB_DOCDB_DOCDB_COMPACTION_FILTER_H

#include <atomic>
#include <memory>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "yb/gutil/thread_annotations.h"
#include "yb/util/strongly_typed_bool.h"

#include "yb/rocksdb/compaction_filter.h"
#include "yb/rocksdb/metadata.h"

#include "yb/common/schema.h"
#include "yb/common/hybrid_time.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/expiration.h"

namespace yb {
namespace docdb {

YB_STRONGLY_TYPED_BOOL(IsMajorCompaction);
YB_STRONGLY_TYPED_BOOL(ShouldRetainDeleteMarkersInMajorCompaction);

struct Expiration;

// A "directive" of how a particular compaction should retain old (overwritten or deleted) values.
struct HistoryRetentionDirective {
  // We will not keep history below this hybrid_time. The view of the database at this hybrid_time
  // is preserved, but after the compaction completes, we should not expect to be able to do
  // consistent scans at DocDB hybrid times lower than this. Those scans will result in missing
  // data. Therefore, it is really important to always set this to a value lower than or equal to
  // the lowest "read point" of any pending read operations.
  HybridTime history_cutoff;

  // Columns that were deleted at a timestamp lower than the history cutoff.
  ColumnIdsPtr deleted_cols;

  MonoDelta table_ttl;

  ShouldRetainDeleteMarkersInMajorCompaction retain_delete_markers_in_major_compaction{false};
};

// DocDB compaction filter. A new instance of this class is created for every compaction.
class DocDBCompactionFilter : public rocksdb::CompactionFilter {
 public:
  DocDBCompactionFilter(
      HistoryRetentionDirective retention,
      IsMajorCompaction is_major_compaction,
      const KeyBounds* key_bounds);

  ~DocDBCompactionFilter() override;
  rocksdb::FilterDecision Filter(
      int level, const Slice& key, const Slice& existing_value, std::string* new_value,
      bool* value_changed) override;
  const char* Name() const override;

  // This indicates we don't have a cached TTL. We need this to be different from kMaxTtl
  // and kResetTtl because a PERSIST call would lead to a cached TTL of kMaxTtl, and kResetTtl
  // indicates no TTL in Cassandra.
  const MonoDelta kNoTtl = MonoDelta::FromNanoseconds(-1);

  // This is used to provide the history_cutoff timestamp to the compaction as a field in the
  // ConsensusFrontier, so that it can be persisted in RocksDB metadata and recovered on bootstrap.
  rocksdb::UserFrontierPtr GetLargestUserFrontier() const override;

  // Returns known post-split boundaries of the tablet's key space, if applicable, and boost::none
  // otherwise.
  Slice DropKeysLessThan() const override;
  Slice DropKeysGreaterOrEqual() const override;

 private:
  // Assigns prev_subdoc_key_ from memory addressed by data. The length of key is taken from
  // sub_key_ends_ and same_bytes are reused.
  void AssignPrevSubDocKey(const char* data, size_t same_bytes);

  // Actual Filter implementation.
  Result<rocksdb::FilterDecision> DoFilter(
      int level, const Slice& key, const Slice& existing_value, std::string* new_value,
      bool* value_changed);

  const HistoryRetentionDirective retention_;
  const KeyBounds* key_bounds_;
  const IsMajorCompaction is_major_compaction_;

  std::vector<char> prev_subdoc_key_;

  // Result of DecodeDocKeyAndSubKeyEnds for prev_subdoc_key_.
  boost::container::small_vector<size_t, 16> sub_key_ends_;

  // A stack of highest hybrid_times lower than or equal to history_cutoff_ at which parent
  // subdocuments of the key that has just been processed, or the subdocument / primitive value
  // itself stored at that key itself, were fully overwritten or deleted. A full overwrite of a
  // parent document is considered a full overwrite of all its subdocuments at every level for the
  // purpose of this definition. Therefore, the following inequalities hold:
  //
  // overwrite_ht_[0] <= ... <= overwrite_ht_[N - 1] <= history_cutoff_
  //
  // The following example shows contents of RocksDB being compacted, as well as the state of the
  // overwrite_ht_ stack and how it is being updated at each step. history_cutoff_ is 25 in this
  // example.
  //
  // RocksDB key/value                    | overwrite_ht_ | Filter logic
  // ----------------------------------------------------------------------------------------------
  // doc_key1 HT(30) -> {}                | [MinHT]       | Always keeping the first entry
  // doc_key1 HT(20) -> DEL               | [20]          | 20 >= MinHT, keeping the entry
  //                                      |               | ^^    ^^^^^
  //                                      | Note: we're comparing the hybrid_time in this key with
  //                                      | the previous stack top of overwrite_ht_.
  //                                      |               |
  // doc_key1 HT(10) -> {}                | [20]          | 10 < 20, deleting the entry
  // doc_key1 subkey1 HT(35) -> "value4"  | [20, 20]      | 35 >= 20, keeping the entry
  //                     ^^               |      ^^       |
  //                      \----------------------/ HT(35) is higher than history_cutoff_, so we're
  //                                      |        just duplicating the top value on the stack
  //                                      |        HT(20) this step.
  //                                      |               |
  // doc_key1 subkey1 HT(23) -> "value3"  | [20, 23]      | 23 >= 20, keeping the entry
  //                                      |      ^^       |
  //                                      |      Now we have actually found a hybrid_time that is
  //                                      |      <= history_cutoff_, so we're replacing the stack
  //                                      |      top with that hybrid_time.
  //                                      |               |
  // doc_key1 subkey1 HT(21) -> "value2"  | [20, 23]      | 21 < 23, deleting the entry
  // doc_key1 subkey1 HT(15) -> "value1"  | [20, 23]      | 15 < 23, deleting the entry

  struct OverwriteData {
    DocHybridTime doc_ht;
    Expiration expiration;
  };

  std::vector<OverwriteData> overwrite_;

  // We use this to only log a message that the filter is being used once on the first call to
  // the Filter function.
  bool filter_usage_logged_ = false;
  bool within_merge_block_ = false;
};

// A strategy for deciding how the history of old database operations should be retained during
// compactions. We may implement this differently in production and in tests.
class HistoryRetentionPolicy {
 public:
  virtual ~HistoryRetentionPolicy() = default;
  virtual HistoryRetentionDirective GetRetentionDirective() = 0;
};

class DocDBCompactionFilterFactory : public rocksdb::CompactionFilterFactory {
 public:
  explicit DocDBCompactionFilterFactory(
      std::shared_ptr<HistoryRetentionPolicy> retention_policy, const KeyBounds* key_bounds);
  ~DocDBCompactionFilterFactory() override;
  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context& context) override;
  const char* Name() const override;

 private:
  std::shared_ptr<HistoryRetentionPolicy> retention_policy_;
  const KeyBounds* key_bounds_;
};

// A history retention policy that can be configured manually. Useful in tests. This class is
// useful for testing and is thread-safe.
class ManualHistoryRetentionPolicy : public HistoryRetentionPolicy {
 public:
  HistoryRetentionDirective GetRetentionDirective() override;

  void SetHistoryCutoff(HybridTime history_cutoff);

  void AddDeletedColumn(ColumnId col);

  void SetTableTTLForTests(MonoDelta ttl);

 private:
  std::atomic<HybridTime> history_cutoff_{HybridTime::kMin};

  std::mutex deleted_cols_mtx_;
  ColumnIds deleted_cols_ GUARDED_BY(deleted_cols_mtx_);

  std::atomic<MonoDelta> table_ttl_{MonoDelta::kMax};
};

}  // namespace docdb
}  // namespace yb

#endif  // YB_DOCDB_DOCDB_COMPACTION_FILTER_H
