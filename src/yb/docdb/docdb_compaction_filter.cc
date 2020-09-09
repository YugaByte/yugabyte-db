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

#include "yb/docdb/docdb_compaction_filter.h"

#include <memory>

#include <glog/logging.h>

#include "yb/rocksdb/compaction_filter.h"
#include "yb/util/string_util.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/value.h"
#include "yb/docdb/consensus_frontier.h"
#include "yb/rocksutil/yb_rocksdb.h"

using std::shared_ptr;
using std::unique_ptr;
using std::unordered_set;
using rocksdb::CompactionFilter;
using rocksdb::VectorToString;
using rocksdb::FilterDecision;

namespace yb {
namespace docdb {

// ------------------------------------------------------------------------------------------------

DocDBCompactionFilter::DocDBCompactionFilter(
    HistoryRetentionDirective retention,
    IsMajorCompaction is_major_compaction,
    const KeyBounds* key_bounds)
    : retention_(std::move(retention)),
      key_bounds_(key_bounds),
      is_major_compaction_(is_major_compaction) {
}

DocDBCompactionFilter::~DocDBCompactionFilter() {
}

FilterDecision DocDBCompactionFilter::Filter(
    int level, const Slice& key, const Slice& existing_value, std::string* new_value,
    bool* value_changed) {
  auto result = const_cast<DocDBCompactionFilter*>(this)->DoFilter(
      level, key, existing_value, new_value, value_changed);
  if (!result.ok()) {
    LOG(FATAL) << "Error filtering " << key.ToDebugString() << ": " << result.status();
  }
  if (*result != FilterDecision::kKeep) {
    VLOG(3) << "Discarding key: " << BestEffortDocDBKeyToStr(key);
  } else {
    VLOG(4) << "Keeping key: " << BestEffortDocDBKeyToStr(key);
  }
  return *result;
}

Result<FilterDecision> DocDBCompactionFilter::DoFilter(
    int level, const Slice& key, const Slice& existing_value, std::string* new_value,
    bool* value_changed) {
  const HybridTime history_cutoff = retention_.history_cutoff;

  if (!filter_usage_logged_) {
    // TODO: switch this to VLOG if it becomes too chatty.
    LOG(INFO) << "DocDB compaction filter is being used for a "
              << (is_major_compaction_ ? "major" : "minor") << " compaction"
              << ", history_cutoff=" << history_cutoff;
    filter_usage_logged_ = true;
  }

  // Remove regular keys which are not related to this RocksDB anymore (due to split of the tablet).
  if (key_bounds_ && !key_bounds_->IsWithinBounds(key)) {
    return FilterDecision::kDiscard;
  }

  // Just remove intent records from regular DB, because it was beta feature.
  // Currently intents are stored in separate DB.
  if (DecodeValueType(key) == ValueType::kObsoleteIntentPrefix) {
    return FilterDecision::kDiscard;
  }

  auto same_bytes = strings::MemoryDifferencePos(
      key.data(), prev_subdoc_key_.data(), std::min(key.size(), prev_subdoc_key_.size()));

  // The number of initial components (including document key and subkeys) that this
  // SubDocKey shares with previous one. This does not care about the hybrid_time field.
  size_t num_shared_components = sub_key_ends_.size();
  while (num_shared_components > 0 && sub_key_ends_[num_shared_components - 1] > same_bytes) {
    --num_shared_components;
  }

  sub_key_ends_.resize(num_shared_components);

  RETURN_NOT_OK(SubDocKey::DecodeDocKeyAndSubKeyEnds(key, &sub_key_ends_));
  const size_t new_stack_size = sub_key_ends_.size();

  // Remove overwrite hybrid_times for components that are no longer relevant for the current
  // SubDocKey.
  overwrite_.resize(min(overwrite_.size(), num_shared_components));
  DocHybridTime ht;
  RETURN_NOT_OK(ht.DecodeFromEnd(key));
  // We're comparing the hybrid time in this key with the stack top of overwrite_ht_ after
  // truncating the stack to the number of components in the common prefix of previous and current
  // key.
  //
  // Example (history_cutoff_ = 12):
  // --------------------------------------------------------------------------------------------
  // Key          overwrite_ht_ stack and relevant notes
  // --------------------------------------------------------------------------------------------
  // k1 T10       [MinHT]
  //
  // k1 T5        [T10]
  //
  // k1 col1 T11  [T10, T11]
  //
  // k1 col1 T7   The stack does not get truncated (shared prefix length is 2), so
  //              prev_overwrite_ht = 11. Removing this entry because 7 < 11.
  //              The stack stays at [T10, T11].
  //
  // k1 col2 T9   Truncating the stack to [T10], setting prev_overwrite_ht to 10, and therefore
  //              deciding to remove this entry because 9 < 10.
  //
  const DocHybridTime prev_overwrite_ht =
      overwrite_.empty() ? DocHybridTime::kMin : overwrite_.back().doc_ht;
  const Expiration prev_exp =
      overwrite_.empty() ? Expiration() : overwrite_.back().expiration;

  // We only keep entries with hybrid_time equal to or later than the latest time the subdocument
  // was fully overwritten or deleted prior to or at the history cutoff time. The intuition is that
  // key/value pairs that were overwritten at or before history cutoff time will not be visible at
  // history cutoff time or any later time anyway.
  //
  // Furthermore, we only need to update the overwrite hybrid_time stack in case we have decided to
  // keep the new entry. Otherwise, the current entry's hybrid time ht is less than the previous
  // overwrite hybrid_time prev_overwrite_ht, and therefore it does not provide any new information
  // about key/value pairs that follow being overwritten at a particular hybrid time. Another way to
  // explain this is to look at the logic that follows. If we don't early-exit here while ht is less
  // than prev_overwrite_ht, we'll end up adding more prev_overwrite_ht values to the overwrite
  // hybrid_time stack, and we might as well do that while handling the next key/value pair that
  // does not get cleaned up the same way as this one.
  //
  // TODO: When more merge records are supported, isTtlRow should be redefined appropriately.
  bool isTtlRow = IsMergeRecord(existing_value);
  if (ht < prev_overwrite_ht && !isTtlRow) {
    return FilterDecision::kDiscard;
  }

  // Every subdocument was fully overwritten at least at the time any of its parents was fully
  // overwritten.
  if (overwrite_.size() < new_stack_size - 1) {
    overwrite_.resize(new_stack_size - 1, {prev_overwrite_ht, prev_exp});
  }

  Expiration popped_exp = overwrite_.empty() ? Expiration() : overwrite_.back().expiration;
  // This will happen in case previous key has the same document key and subkeys as the current
  // key, and the only difference is in the hybrid_time. We want to replace the hybrid_time at the
  // top of the overwrite_ht stack in this case.
  if (overwrite_.size() == new_stack_size) {
    overwrite_.pop_back();
  }

  // Check whether current key is the same as the previous key, except for the timestamp.
  if (same_bytes != sub_key_ends_.back()) {
    within_merge_block_ = false;
  }

  // See if we found a higher hybrid time not exceeding the history cutoff hybrid time at which the
  // subdocument (including a primitive value) rooted at the current key was fully overwritten.
  // In case of ht > history_cutoff_, we just keep the parent document's highest known overwrite
  // hybrid time that does not exceed the cutoff hybrid time. In that case this entry is obviously
  // too new to be garbage-collected.
  if (ht.hybrid_time() > history_cutoff) {
    AssignPrevSubDocKey(key.cdata(), same_bytes);
    overwrite_.push_back({prev_overwrite_ht, prev_exp});
    return FilterDecision::kKeep;
  }

  // Check for CQL columns deleted from the schema. This is done regardless of whether this is a
  // major or minor compaction.
  //
  // TODO: could there be a case when there is still a read request running that uses an old schema,
  //       and we end up removing some data that the client expects to see?
  if (sub_key_ends_.size() > 1) {
    // Column ID is the first subkey in every CQL row.
    if (key[sub_key_ends_[0]]  == ValueTypeAsChar::kColumnId) {
      Slice column_id_slice(key.data() + sub_key_ends_[0] + 1, key.data() + sub_key_ends_[1]);
      auto column_id_as_int64 = VERIFY_RESULT(util::FastDecodeSignedVarInt(&column_id_slice));
      ColumnId column_id;
      RETURN_NOT_OK(ColumnId::FromInt64(column_id_as_int64, &column_id));
      if (retention_.deleted_cols->count(column_id) != 0) {
        return FilterDecision::kDiscard;
      }
    }
  }

  auto overwrite_ht = isTtlRow ? prev_overwrite_ht : max(prev_overwrite_ht, ht);

  Value value;
  Slice value_slice = existing_value;
  RETURN_NOT_OK(value.DecodeControlFields(&value_slice));
  const auto value_type = static_cast<ValueType>(
      value_slice.FirstByteOr(ValueTypeAsChar::kInvalid));
  const Expiration curr_exp(ht.hybrid_time(), value.ttl());

  // If within the merge block.
  //     If the row is a TTL row, delete it.
  //     Otherwise, replace it with the cached TTL (i.e., apply merge).
  // Otherwise,
  //     If this is a TTL row, cache TTL (start merge block).
  //     If normal row, compute its ttl and continue.

  Expiration expiration;
  if (within_merge_block_) {
    expiration = popped_exp;
  } else if (ht.hybrid_time() >= prev_exp.write_ht &&
             (curr_exp.ttl != Value::kMaxTtl || isTtlRow)) {
    expiration = curr_exp;
  } else {
    expiration = prev_exp;
  }

  overwrite_.push_back({overwrite_ht, expiration});

  if (overwrite_.size() != new_stack_size) {
    return STATUS_FORMAT(Corruption, "Overwrite size does not match new_stack_size: $0 vs $1",
                         overwrite_.size(), new_stack_size);
  }
  AssignPrevSubDocKey(key.cdata(), same_bytes);

  // If the entry has the TTL flag, delete the entry.
  if (isTtlRow) {
    within_merge_block_ = true;
    return FilterDecision::kDiscard;
  }

  // If the value expires by the time of history cutoff, it is treated as deleted and filtered out.
  bool has_expired = false;

  // Only check for expiration if the current hybrid time is at or below history cutoff.
  // The key could not have possibly expired by history_cutoff_ otherwise.
  MonoDelta true_ttl = ComputeTTL(expiration.ttl, retention_.table_ttl);
  RETURN_NOT_OK(HasExpiredTTL(true_ttl == expiration.ttl ?
                              expiration.write_ht : ht.hybrid_time(),
                              true_ttl, history_cutoff, &has_expired));
  // As of 02/2017, we don't have init markers for top level documents in QL. As a result, we can
  // compact away each column if it has expired, including the liveness system column. The init
  // markers in Redis wouldn't be affected since they don't have any TTL associated with them and
  // the TTL would default to kMaxTtl which would make has_expired false.
  if (has_expired) {
    // This is consistent with the condition we're testing for deletes at the bottom of the function
    // because ht_at_or_below_cutoff is implied by has_expired.
    if (is_major_compaction_ && !retention_.retain_delete_markers_in_major_compaction) {
      return FilterDecision::kDiscard;
    }

    // During minor compactions, expired values are written back as tombstones because removing the
    // record might expose earlier values which would be incorrect.
    *value_changed = true;
    *new_value = Value::EncodedTombstone();
  } else if (within_merge_block_) {
    *value_changed = true;

    if (expiration.ttl != Value::kMaxTtl) {
      expiration.ttl += MonoDelta::FromMicroseconds(
          overwrite_.back().expiration.write_ht.PhysicalDiff(ht.hybrid_time()));
      overwrite_.back().expiration.ttl = expiration.ttl;
    }

    *value.mutable_ttl() = expiration.ttl;
    new_value->clear();

    // We are reusing the existing encoded value without decoding/encoding it.
    value.EncodeAndAppend(new_value, &value_slice);
    within_merge_block_ = false;
  } else if (value.intent_doc_ht().is_valid() && ht.hybrid_time() < history_cutoff) {
    // Cleanup intent doc hybrid time when we don't need it anymore.
    // See https://github.com/yugabyte/yugabyte-db/issues/4535 for details.
    value.ClearIntentDocHt();

    new_value->clear();

    // We are reusing the existing encoded value without decoding/encoding it.
    value.EncodeAndAppend(new_value, &value_slice);
  }

  // If we are backfilling an index table, we want to preserve the delete markers in the table
  // until the backfill process is completed. For other normal use cases, delete markers/tombstones
  // can be cleaned up on a major compaction.
  // retention_.retain_delete_markers_in_major_compaction will be set to true until the index
  // backfill is complete.
  //
  // Tombstones at or below the history cutoff hybrid_time can always be cleaned up on full (major)
  // compactions. However, we do need to update the overwrite hybrid time stack in this case (as we
  // just did), because this deletion (tombstone) entry might be the only reason for cleaning up
  // more entries appearing at earlier hybrid times.
  return value_type == ValueType::kTombstone && is_major_compaction_ &&
                 !retention_.retain_delete_markers_in_major_compaction
             ? FilterDecision::kDiscard
             : FilterDecision::kKeep;
}

void DocDBCompactionFilter::AssignPrevSubDocKey(
    const char* data, size_t same_bytes) {
  size_t size = sub_key_ends_.back();
  prev_subdoc_key_.resize(size);
  memcpy(prev_subdoc_key_.data() + same_bytes, data + same_bytes, size - same_bytes);
}


rocksdb::UserFrontierPtr DocDBCompactionFilter::GetLargestUserFrontier() const {
  auto* consensus_frontier = new ConsensusFrontier();
  consensus_frontier->set_history_cutoff(retention_.history_cutoff);
  return rocksdb::UserFrontierPtr(consensus_frontier);
}

const char* DocDBCompactionFilter::Name() const {
  return "DocDBCompactionFilter";
}

// ------------------------------------------------------------------------------------------------

DocDBCompactionFilterFactory::DocDBCompactionFilterFactory(
    std::shared_ptr<HistoryRetentionPolicy> retention_policy, const KeyBounds* key_bounds)
    : retention_policy_(std::move(retention_policy)), key_bounds_(key_bounds) {
}

DocDBCompactionFilterFactory::~DocDBCompactionFilterFactory() {
}

unique_ptr<CompactionFilter> DocDBCompactionFilterFactory::CreateCompactionFilter(
    const CompactionFilter::Context& context) {
  return std::make_unique<DocDBCompactionFilter>(
      retention_policy_->GetRetentionDirective(),
      IsMajorCompaction(context.is_full_compaction),
      key_bounds_);
}

const char* DocDBCompactionFilterFactory::Name() const {
  return "DocDBCompactionFilterFactory";
}

// ------------------------------------------------------------------------------------------------

HistoryRetentionDirective ManualHistoryRetentionPolicy::GetRetentionDirective() {
  std::lock_guard<std::mutex> lock(deleted_cols_mtx_);
  return {history_cutoff_.load(std::memory_order_acquire),
          std::make_shared<ColumnIds>(deleted_cols_), table_ttl_.load(std::memory_order_acquire),
          ShouldRetainDeleteMarkersInMajorCompaction::kFalse};
}

void ManualHistoryRetentionPolicy::SetHistoryCutoff(HybridTime history_cutoff) {
  history_cutoff_.store(history_cutoff, std::memory_order_release);
}

void ManualHistoryRetentionPolicy::AddDeletedColumn(ColumnId col) {
  std::lock_guard<std::mutex> lock(deleted_cols_mtx_);
  deleted_cols_.insert(col);
}

void ManualHistoryRetentionPolicy::SetTableTTLForTests(MonoDelta ttl) {
  table_ttl_.store(ttl, std::memory_order_release);
}

}  // namespace docdb
}  // namespace yb
