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

#ifndef YB_DOCDB_INTENT_AWARE_ITERATOR_H_
#define YB_DOCDB_INTENT_AWARE_ITERATOR_H_

#include <boost/optional/optional.hpp>

#include "yb/common/read_hybrid_time.h"

#include "yb/docdb/bounded_rocksdb_iterator.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/key_bytes.h"

#include "yb/rocksdb/db.h"
#include "yb/rocksdb/options.h"

namespace yb {

class DocHybridTime;
class TransactionStatusManager;

namespace docdb {

class Value;
struct Expiration;

YB_DEFINE_ENUM(ResolvedIntentState, (kNoIntent)(kInvalidPrefix)(kValid));
YB_DEFINE_ENUM(Direction, (kForward)(kBackward));
YB_DEFINE_ENUM(SeekIntentIterNeeded, (kNoNeed)(kSeek)(kSeekForward));

// Caches transaction statuses fetched by single IntentAwareIterator.
// Thread safety is not required, because IntentAwareIterator is used in a single thread only.
class TransactionStatusCache {
 public:
  TransactionStatusCache(TransactionStatusManager* txn_status_manager,
                         const ReadHybridTime& read_time,
                         CoarseTimePoint deadline)
      : txn_status_manager_(txn_status_manager), read_time_(read_time), deadline_(deadline) {}

  // Returns transaction commit time if already committed by the specified time or HybridTime::kMin
  // otherwise.
  Result<HybridTime> GetCommitTime(const TransactionId& transaction_id);

 private:
  HybridTime GetLocalCommitTime(const TransactionId& transaction_id);
  Result<HybridTime> DoGetCommitTime(const TransactionId& transaction_id);

  TransactionStatusManager* txn_status_manager_;
  ReadHybridTime read_time_;
  CoarseTimePoint deadline_;
  std::unordered_map<TransactionId, HybridTime, TransactionIdHash> cache_;
};

struct FetchKeyResult {
  Slice key;
  DocHybridTime write_time;
  bool same_transaction;
};

// Provides a way to iterate over DocDB (sub)keys with respect to committed intents transparently
// for caller. Implementation relies on intents order in RocksDB, which is determined by intent key
// format. If (sub)key A goes before/after (sub)key B, all intents for A should go before/after all
// intents for (sub)key B.
//
// When seeking to some (sub)dockey, it will ignore values for this (sub)dockey
// (except write intents written by the same transaction) with HT higher than read_time.
//
// For intents from the same transaction, intents with maximum HT would be picked -- ignoring
// read_time for filtering purposes -- and returned as key with time equal to read_time.
// Intent data format:
//   SubDocKey (no HybridTime) + IntentType + HybridTime -> TxnId + value.
// TxnId, IntentType, HybridTime are all prefixed with their respective value types.
//
// KeyBytes/Slice passed to Seek* methods should not contain hybrid time.
// HybridTime of subdoc_key in Seek* methods would be ignored.
class IntentAwareIterator {
 public:
  IntentAwareIterator(
      const DocDB& doc_db,
      const rocksdb::ReadOptions& read_opts,
      CoarseTimePoint deadline,
      const ReadHybridTime& read_time,
      const TransactionOperationContextOpt& txn_op_context);

  IntentAwareIterator(const IntentAwareIterator& other) = delete;
  void operator=(const IntentAwareIterator& other) = delete;

  // Seek to the smallest key which is greater or equal than doc_key.
  void Seek(const DocKey& doc_key);

  // Seek to specified encoded key (it is responsibility of caller to make sure it doesn't have
  // hybrid time).
  void Seek(const Slice& key);

  // Seek forward to specified encoded key (it is responsibility of caller to make sure it
  // doesn't have hybrid time). For efficiency, the method that takes a non-const KeyBytes pointer
  // avoids memory allocation by using the KeyBytes buffer to prepare the key to seek to, and may
  // append up to kMaxBytesPerEncodedHybridTime + 1 bytes of data to the buffer. The appended data
  // is removed when the method returns.
  void SeekForward(const Slice& key);
  void SeekForward(KeyBytes* key);

  // Seek past specified subdoc key (it is responsibility of caller to make sure it doesn't have
  // hybrid time).
  void SeekPastSubKey(const Slice& key);

  // Seek out of subdoc key (it is responsibility of caller to make sure it doesn't have hybrid
  // time).
  void SeekOutOfSubDoc(const Slice& key);
  // For efficiency, this overload takes a non-const KeyBytes pointer avoids memory allocation by
  // using the KeyBytes buffer to prepare the key to seek to by appending an extra byte. The
  // appended byte is removed when the method returns.
  void SeekOutOfSubDoc(KeyBytes* key_bytes);

  // Seek to last doc key.
  void SeekToLastDocKey();

  // Seek to previous SubDocKey as opposed to previous DocKey.
  void PrevSubDocKey(const KeyBytes& key_bytes);

  // This method positions the iterator at the beginning of the DocKey found before the doc_key
  // provided.
  void PrevDocKey(const DocKey& doc_key);
  void PrevDocKey(const Slice& encoded_doc_key);

  // Fetches currently pointed key and also updates max_seen_ht to ht of this key. The key does not
  // contain the DocHybridTime but is returned separately and optionally.
  Result<FetchKeyResult> FetchKey();

  bool valid();
  Slice value();
  const ReadHybridTime& read_time() { return read_time_; }
  HybridTime max_seen_ht() { return max_seen_ht_; }

  // Iterate through Next() until a row containing a full record (non merge record) is found, or the
  // key changes.
  //
  // If a new full record with the same key is found, latest_record_ht is set to the write time of
  // that record, result_value is set to the value, and final_key is set to the key.
  //
  // If the key changes, latest_record_ht is set to the write time of the last merge record seen,
  // result_value is set to its value, and final_key is set to the key.
  CHECKED_STATUS NextFullValue(
      DocHybridTime* latest_record_ht,
      Slice* result_value,
      Slice* final_key = nullptr);

  // Finds the latest record for a particular key after the provided max_overwrite_time, returns the
  // write time of the found record, and optionally also the result value. This latest record may
  // not be a full record, but instead a merge record (e.g. a TTL row).
  CHECKED_STATUS FindLatestRecord(
      const Slice& key_without_ht,
      DocHybridTime* max_overwrite_time,
      Slice* result_value = nullptr);

  // Finds the oldest record for a particular key that is larger than the
  // specified min_hybrid_time, returns the overwrite time.
  // This record may not be a full record, but instead a merge record (e.g. a
  // TTL row).
  // Returns HybridTime::kInvalid if no such record was found.
  Result<HybridTime> FindOldestRecord(const Slice& key_without_ht,
                                      HybridTime min_hybrid_time);

  void SetUpperbound(const Slice& upperbound) {
    upperbound_ = upperbound;
  }

  void DebugDump();

 private:
  friend class IntentAwareIteratorPrefixScope;

  // Adds new value to prefix stack. The top value of this stack is used to filter returned entries.
  void PushPrefix(const Slice& prefix);

  // Removes top value from prefix stack. This iteration could became valid after popping prefix,
  // if new top prefix is a prefix of currently pointed value.
  void PopPrefix();

  Slice CurrentPrefix() const;

  // Seek forward on regular sub-iterator.
  void SeekForwardRegular(const Slice& slice);

  // Seek to latest doc key among regular and intent iterator.
  void SeekToLatestDocKeyInternal();
  // Seek to latest subdoc key among regular and intent iterator.
  void SeekToLatestSubDocKeyInternal();

  // Choose latest subkey among regular and intent iterators.
  Slice LatestSubDocKey();

  // Skips regular entries with hybrid time after read limit.
  // If `is_forward` is `false` and `iter_` is positioned to the earliest record for the current
  // key, there are two cases:
  // 1 - record has hybrid time <= read limit: does nothing.
  // 2 - record has hybrid time > read limit: will skip all records for this key, since all of
  // them are after earliest record which is after read limit. Then will act the same way on
  // previous key.
  void SkipFutureRecords(Direction direction);

  // Skips intents with hybrid time after read limit.
  void SkipFutureIntents();

  // Strong write intents which are either committed or written by the current
  // transaction (stored in txn_op_context) by considered time are considered as suitable.

  // Seek intent sub-iterator to latest suitable intent starting with seek_key_buffer_.
  // intent_iter_ will be positioned to first intent for the smallest key greater than
  // resolved_intent_sub_doc_key_encoded_.
  // If iterator already positioned far enough - does not perform seek.
  // If we already resolved intent after seek_key_prefix_, then it will be used.
  void SeekForwardToSuitableIntent();

  // Seek intent sub-iterator forward (backward) to latest suitable intent for first available
  // key. Updates resolved_intent_XXX fields.
  // intent_iter_ will be positioned to first intent for the smallest (biggest) key
  // greater (smaller) than resolved_intent_sub_doc_key_encoded_.
  template<Direction direction>
  void SeekToSuitableIntent();

  // Decodes intent at intent_iter_ position and updates resolved_intent_* fields if that intent
  // matches all following conditions:
  // 1. It is strong write intent.
  // 2. It is either (a) written at value_time by transaction associated with txn_op_context_
  // (current transaction) earlier than high_ht_ or (b) its transaction is already committed and
  // value_time = commit time is earlier than high_ht_.
  // 3. It has value_time earlier than current resolved intent if we already have resolved intent.
  //
  // Preconditions:
  // 1. If has_resolved_intent_ is true then intent should be exactly for the same key as
  // resolved_intent_key_prefix_.
  //
  // Note: For performance reasons resolved_intent_sub_doc_key_encoded_ is not updated by this
  // function and should be updated after we have found latest suitable intent for the key by
  // calling UpdateResolvedIntentSubDocKeyEncoded.
  void ProcessIntent();

  void UpdateResolvedIntentSubDocKeyEncoded();

  // Seeks to the appropriate intent-prefix and returns the associated
  // DocHybridTime.
  Result<DocHybridTime> FindMatchingIntentRecordDocHybridTime(const Slice& key_without_ht);

  // Returns the DocHybridTime associated with the current regular record
  // pointed to, if it matches the key that is passed as the argument.
  // If the current record does not match the passed key, invalid hybrid time
  // is returned.
  Result<DocHybridTime> GetMatchingRegularRecordDocHybridTime(
      const Slice& key_without_ht);

  // Whether current entry is regular key-value pair.
  bool IsEntryRegular(bool descending = false);

  // Set the exclusive upperbound of the intent iterator to the current SubDocKey of the regular
  // iterator. This is necessary to avoid RocksDB iterator from scanning over the deleted intents
  // beyond the current regular key unnecessarily.
  CHECKED_STATUS SetIntentUpperbound();

  // Resets the exclusive upperbound of the intent iterator to the beginning of the transaction
  // metadata and reverse index region.
  void ResetIntentUpperbound();

  void SeekIntentIterIfNeeded();

  // Does initial steps for prev doc key/sub doc key seek.
  // Returns true if prepare succeed.
  bool PreparePrev(const Slice& key);

  bool SatisfyBounds(const Slice& slice);

  bool ResolvedIntentFromSameTransaction() const {
    return intent_dht_from_same_txn_ != DocHybridTime::kMin;
  }

  DocHybridTime GetIntentDocHybridTime() const {
    return ResolvedIntentFromSameTransaction() ? intent_dht_from_same_txn_
                                               : resolved_intent_txn_dht_;
  }

  // Returns true if iterator currently points to some record.
  bool HasCurrentEntry();

  // Update seek_intent_iter_needed_, seek_key_prefix_ and seek_key_buffer_ to seek forward
  // for key + suffix.
  // If use_suffix_for_prefix then suffix is used in seek_key_prefix_, otherwise it will match key.
  void UpdatePlannedIntentSeekForward(
      const Slice& key, const Slice& suffix, bool use_suffix_for_prefix = true);

  bool NextRegular(Direction direction);

  const ReadHybridTime read_time_;
  const std::string encoded_read_time_read_;
  const std::string encoded_read_time_local_limit_;
  const std::string encoded_read_time_global_limit_;

  // The encoded hybrid time to use to filter records in regular RocksDB. This is the maximum of
  // read_time and local_limit (in terms of hybrid time comparison), and this slice points to
  // one of the strings above.
  Slice encoded_read_time_regular_limit_;

  const TransactionOperationContextOpt txn_op_context_;
  docdb::BoundedRocksDbIterator intent_iter_;
  docdb::BoundedRocksDbIterator iter_;
  // iter_valid_ is true if and only if iter_ is positioned at key which matches top prefix from
  // the stack and record time satisfies read_time_ criteria.
  bool iter_valid_ = false;
  Status status_;
  HybridTime max_seen_ht_ = HybridTime::kMin;

  // Upperbound for seek. If we see regular or intent record past this bound, it will be ignored.
  Slice upperbound_;

  // Exclusive upperbound of the intent key.
  KeyBytes intent_upperbound_keybytes_;
  Slice intent_upperbound_;

  // Following fields contain information related to resolved suitable intent.
  ResolvedIntentState resolved_intent_state_ = ResolvedIntentState::kNoIntent;
  // SubDocKey (no HT).
  KeyBytes resolved_intent_key_prefix_;
  // DocHybridTime of resolved_intent_sub_doc_key_encoded_ is set to commit time or intent time in
  // case of intent is written by current transaction (stored in txn_op_context_).
  DocHybridTime resolved_intent_txn_dht_;
  DocHybridTime intent_dht_from_same_txn_ = DocHybridTime::kMin;
  KeyBytes resolved_intent_sub_doc_key_encoded_;
  KeyBytes resolved_intent_value_;

  std::vector<Slice> prefix_stack_;
  TransactionStatusCache transaction_status_cache_;

  bool skip_future_records_needed_ = false;
  bool skip_future_intents_needed_ = false;
  SeekIntentIterNeeded seek_intent_iter_needed_ = SeekIntentIterNeeded::kNoNeed;

  // Reusable buffer to prepare seek key to avoid reallocating temporary buffers in critical paths.
  KeyBytes seek_key_buffer_;
  Slice seek_key_prefix_;
};

class IntentAwareIteratorPrefixScope {
 public:
  IntentAwareIteratorPrefixScope(const Slice& prefix, IntentAwareIterator* iterator)
      : iterator_(iterator) {
    iterator->PushPrefix(prefix);
  }

  ~IntentAwareIteratorPrefixScope() {
    iterator_->PopPrefix();
  }

 private:
  IntentAwareIterator* iterator_;
};

} // namespace docdb
} // namespace yb

#endif // YB_DOCDB_INTENT_AWARE_ITERATOR_H_
