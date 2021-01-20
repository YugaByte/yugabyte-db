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

#include <algorithm>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "yb/common/hybrid_time.h"
#include "yb/common/redis_protocol.pb.h"
#include "yb/common/row_mark.h"
#include "yb/common/transaction.h"

#include "yb/docdb/conflict_resolution.h"
#include "yb/docdb/cql_operation.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_util.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/docdb/pgsql_operation.h"
#include "yb/docdb/shared_lock_manager.h"
#include "yb/docdb/subdocument.h"
#include "yb/docdb/value.h"
#include "yb/docdb/value_type.h"
#include "yb/docdb/deadline_info.h"
#include "yb/docdb/docdb_types.h"
#include "yb/docdb/kv_debug.h"

#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/write_batch_formatter.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/server/hybrid_clock.h"

#include "yb/util/bitmap.h"
#include "yb/util/bytes_formatter.h"
#include "yb/util/date_time.h"
#include "yb/util/enums.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/pb_util.h"
#include "yb/util/status.h"

#include "yb/yql/cql/ql/util/errcodes.h"

using std::endl;
using std::list;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::shared_ptr;
using std::stack;
using std::vector;
using std::make_shared;

using yb::HybridTime;
using yb::FormatBytesAsStr;
using strings::Substitute;

using namespace std::placeholders;

DEFINE_test_flag(bool, docdb_sort_weak_intents_in_tests, false,
                "Sort weak intents to make their order deterministic.");
DEFINE_bool(enable_transaction_sealing, false,
            "Whether transaction sealing is enabled.");
DEFINE_test_flag(bool, fail_on_replicated_batch_idx_set_in_txn_record, false,
                 "Fail when a set of replicated batch indexes is found in txn record.");
DEFINE_int32(txn_max_apply_batch_records, 100000,
             "Max number of apply records allowed in single RocksDB batch. "
             "When a transaction's data in one tablet does not fit into specified number of "
             "records, it will be applied using multiple RocksDB write batches.");

namespace yb {
namespace docdb {

namespace {

// Slice parts with the number of slices fixed at compile time.
template <int N>
struct FixedSliceParts {
  FixedSliceParts(const std::array<Slice, N>& input) : parts(input.data()) { // NOLINT
  }

  operator SliceParts() const {
    return SliceParts(parts, N);
  }

  const Slice* parts;
};

Slice InvertedDocHt(const Slice& input, size_t* buffer) {
  memcpy(buffer, input.data(), input.size());
  for (size_t i = 0; i != kMaxWordsPerEncodedHybridTimeWithValueType; ++i) {
    buffer[i] = ~buffer[i];
  }
  return {pointer_cast<char*>(buffer), input.size()};
}

// Main intent data::
// Prefix + DocPath + IntentType + DocHybridTime -> TxnId + value of the intent
// Reverse index by txn id:
// Prefix + TxnId + DocHybridTime -> Main intent data key
//
// Expects that last entry of key is DocHybridTime.
template <int N>
void AddIntent(
    const TransactionId& transaction_id,
    const FixedSliceParts<N>& key,
    const SliceParts& value,
    rocksdb::WriteBatch* rocksdb_write_batch,
    Slice reverse_value_prefix = Slice()) {
  char reverse_key_prefix[1] = { ValueTypeAsChar::kTransactionId };
  size_t doc_ht_buffer[kMaxWordsPerEncodedHybridTimeWithValueType];
  auto doc_ht_slice = InvertedDocHt(key.parts[N - 1], doc_ht_buffer);

  std::array<Slice, 3> reverse_key = {{
      Slice(reverse_key_prefix, sizeof(reverse_key_prefix)),
      transaction_id.AsSlice(),
      doc_ht_slice,
  }};
  rocksdb_write_batch->Put(key, value);
  if (reverse_value_prefix.empty()) {
    rocksdb_write_batch->Put(reverse_key, key);
  } else {
    std::array<Slice, N + 1> reverse_value;
    reverse_value[0] = reverse_value_prefix;
    memcpy(&reverse_value[1], key.parts, sizeof(*key.parts) * N);
    rocksdb_write_batch->Put(reverse_key, reverse_value);
  }
}

// key should be valid prefix of doc key, ending with some complete pritimive value or group end.
CHECKED_STATUS ApplyIntent(RefCntPrefix key,
                           const IntentTypeSet intent_types,
                           LockBatchEntries *keys_locked) {
  // Have to strip kGroupEnd from end of key, because when only hash key is specified, we will
  // get two kGroupEnd at end of strong intent.
  size_t size = key.size();
  if (size > 0) {
    if (key.data()[0] == ValueTypeAsChar::kGroupEnd) {
      if (size != 1) {
        return STATUS_FORMAT(Corruption, "Key starting with group end: $0",
            key.as_slice().ToDebugHexString());
      }
      size = 0;
    } else {
      while (key.data()[size - 1] == ValueTypeAsChar::kGroupEnd) {
        --size;
      }
    }
  }
  key.Resize(size);
  keys_locked->push_back({key, intent_types});
  return Status::OK();
}

struct DetermineKeysToLockResult {
  LockBatchEntries lock_batch;
  bool need_read_snapshot;
};

Result<DetermineKeysToLockResult> DetermineKeysToLock(
    const std::vector<std::unique_ptr<DocOperation>>& doc_write_ops,
    const google::protobuf::RepeatedPtrField<KeyValuePairPB>& read_pairs,
    const IsolationLevel isolation_level,
    const OperationKind operation_kind,
    const RowMarkType row_mark_type,
    bool transactional_table,
    PartialRangeKeyIntents partial_range_key_intents) {
  DetermineKeysToLockResult result;
  boost::container::small_vector<RefCntPrefix, 8> doc_paths;
  boost::container::small_vector<size_t, 32> key_prefix_lengths;
  result.need_read_snapshot = false;
  for (const unique_ptr<DocOperation>& doc_op : doc_write_ops) {
    doc_paths.clear();
    IsolationLevel level;
    RETURN_NOT_OK(doc_op->GetDocPaths(GetDocPathsMode::kLock, &doc_paths, &level));
    if (isolation_level != IsolationLevel::NON_TRANSACTIONAL) {
      level = isolation_level;
    }
    IntentTypeSet strong_intent_types = GetStrongIntentTypeSet(level, operation_kind,
                                                               row_mark_type);
    if (isolation_level == IsolationLevel::SERIALIZABLE_ISOLATION &&
        operation_kind == OperationKind::kWrite &&
        doc_op->RequireReadSnapshot()) {
      strong_intent_types = IntentTypeSet({IntentType::kStrongRead, IntentType::kStrongWrite});
    }

    for (const auto& doc_path : doc_paths) {
      key_prefix_lengths.clear();
      RETURN_NOT_OK(SubDocKey::DecodePrefixLengths(doc_path.as_slice(), &key_prefix_lengths));
      // At least entire doc_path should be returned, so empty key_prefix_lengths is an error.
      if (key_prefix_lengths.empty()) {
        return STATUS_FORMAT(Corruption, "Unable to decode key prefixes from: $0",
                             doc_path.as_slice().ToDebugHexString());
      }
      // We will acquire strong lock on entire doc_path, so remove it from list of weak locks.
      key_prefix_lengths.pop_back();
      auto partial_key = doc_path;
      // Acquire weak lock on empty key for transactional tables,
      // unless specified key is already empty.
      if (doc_path.size() > 0 && transactional_table) {
        partial_key.Resize(0);
        RETURN_NOT_OK(ApplyIntent(
            partial_key, StrongToWeak(strong_intent_types), &result.lock_batch));
      }
      for (size_t prefix_length : key_prefix_lengths) {
        partial_key.Resize(prefix_length);
        RETURN_NOT_OK(ApplyIntent(
            partial_key, StrongToWeak(strong_intent_types), &result.lock_batch));
      }

      RETURN_NOT_OK(ApplyIntent(doc_path, strong_intent_types, &result.lock_batch));
    }

    if (doc_op->RequireReadSnapshot()) {
      result.need_read_snapshot = true;
    }
  }

  if (!read_pairs.empty()) {
    RETURN_NOT_OK(EnumerateIntents(
        read_pairs,
        [&result](IntentStrength strength, FullDocKey, Slice value, KeyBytes* key, LastKey) {
          RefCntPrefix prefix(key->AsSlice());
          auto intent_types = strength == IntentStrength::kStrong
              ? IntentTypeSet({IntentType::kStrongRead})
              : IntentTypeSet({IntentType::kWeakRead});
          return ApplyIntent(prefix, intent_types, &result.lock_batch);
        }, partial_range_key_intents));
  }

  return result;
}

void FilterKeysToLock(LockBatchEntries *keys_locked) {
  if (keys_locked->empty()) {
    return;
  }

  std::sort(keys_locked->begin(), keys_locked->end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.key < rhs.key;
            });

  auto w = keys_locked->begin();
  for (auto it = keys_locked->begin(); ++it != keys_locked->end();) {
    if (it->key == w->key) {
      w->intent_types |= it->intent_types;
    } else {
      ++w;
      *w = *it;
    }
  }

  ++w;
  keys_locked->erase(w, keys_locked->end());
}

// Buffer for encoding DocHybridTime
class DocHybridTimeBuffer {
 public:
  DocHybridTimeBuffer() {
    buffer_[0] = ValueTypeAsChar::kHybridTime;
  }

  Slice EncodeWithValueType(const DocHybridTime& doc_ht) {
    auto end = doc_ht.EncodedInDocDbFormat(buffer_.data() + 1);
    return Slice(buffer_.data(), end);
  }

  Slice EncodeWithValueType(HybridTime ht, IntraTxnWriteId write_id) {
    return EncodeWithValueType(DocHybridTime(ht, write_id));
  }
 private:
  std::array<char, 1 + kMaxBytesPerEncodedHybridTime> buffer_;
};

}  // namespace

Result<PrepareDocWriteOperationResult> PrepareDocWriteOperation(
    const std::vector<std::unique_ptr<DocOperation>>& doc_write_ops,
    const google::protobuf::RepeatedPtrField<KeyValuePairPB>& read_pairs,
    const scoped_refptr<Histogram>& write_lock_latency,
    const IsolationLevel isolation_level,
    const OperationKind operation_kind,
    const RowMarkType row_mark_type,
    bool transactional_table,
    CoarseTimePoint deadline,
    PartialRangeKeyIntents partial_range_key_intents,
    SharedLockManager *lock_manager) {
  PrepareDocWriteOperationResult result;

  auto determine_keys_to_lock_result = VERIFY_RESULT(DetermineKeysToLock(
      doc_write_ops, read_pairs, isolation_level, operation_kind, row_mark_type,
      transactional_table, partial_range_key_intents));
  if (determine_keys_to_lock_result.lock_batch.empty()) {
    LOG(ERROR) << "Empty lock batch, doc_write_ops: " << yb::ToString(doc_write_ops)
               << ", read pairs: " << yb::ToString(read_pairs);
    return STATUS(Corruption, "Empty lock batch");
  }
  result.need_read_snapshot = determine_keys_to_lock_result.need_read_snapshot;

  FilterKeysToLock(&determine_keys_to_lock_result.lock_batch);
  const MonoTime start_time = (write_lock_latency != nullptr) ? MonoTime::Now() : MonoTime();
  result.lock_batch = LockBatch(
      lock_manager, std::move(determine_keys_to_lock_result.lock_batch), deadline);
  RETURN_NOT_OK_PREPEND(
      result.lock_batch.status(), Format("Timeout: $0", deadline - ToCoarse(start_time)));
  if (write_lock_latency != nullptr) {
    const MonoDelta elapsed_time = MonoTime::Now().GetDeltaSince(start_time);
    write_lock_latency->Increment(elapsed_time.ToMicroseconds());
  }

  return result;
}

Status SetDocOpQLErrorResponse(DocOperation* doc_op, string err_msg) {
  switch (doc_op->OpType()) {
    case DocOperation::Type::QL_WRITE_OPERATION: {
      const auto &resp = down_cast<QLWriteOperation *>(doc_op)->response();
      resp->set_status(QLResponsePB::YQL_STATUS_QUERY_ERROR);
      resp->set_error_message(err_msg);
      break;
    }
    case DocOperation::Type::PGSQL_WRITE_OPERATION: {
      const auto &resp = down_cast<PgsqlWriteOperation *>(doc_op)->response();
      resp->set_status(PgsqlResponsePB::PGSQL_STATUS_USAGE_ERROR);
      resp->set_error_message(err_msg);
      break;
    }
    default:
      return STATUS_FORMAT(InternalError,
                           "Invalid status (QLError) for doc operation %d",
                           doc_op->OpType());
  }
  return Status::OK();
}

Status ExecuteDocWriteOperation(const vector<unique_ptr<DocOperation>>& doc_write_ops,
                                CoarseTimePoint deadline,
                                const ReadHybridTime& read_time,
                                const DocDB& doc_db,
                                KeyValueWriteBatchPB* write_batch,
                                InitMarkerBehavior init_marker_behavior,
                                std::atomic<int64_t>* monotonic_counter,
                                HybridTime* restart_read_ht,
                                const string& table_name) {
  DCHECK_ONLY_NOTNULL(restart_read_ht);
  DocWriteBatch doc_write_batch(doc_db, init_marker_behavior, monotonic_counter);
  DocOperationApplyData data = {&doc_write_batch, deadline, read_time, restart_read_ht};
  for (const unique_ptr<DocOperation>& doc_op : doc_write_ops) {
    Status s = doc_op->Apply(data);
    if (s.IsQLError()) {
      string error_msg;
      if (ql::GetErrorCode(s) == ql::ErrorCode::CONDITION_NOT_SATISFIED) {
        // Generating the error message here because 'table_name'
        // is not available on the lower level - in doc_op->Apply().
        error_msg = Format("Condition on table $0 was not satisfied.", table_name);
      } else {
        error_msg =  s.message().ToBuffer();
      }

      // Ensure we set appropriate error in the response object for QL errors.
      RETURN_NOT_OK(SetDocOpQLErrorResponse(doc_op.get(), error_msg));
      continue;
    }

    RETURN_NOT_OK(s);
  }
  doc_write_batch.MoveToWriteBatchPB(write_batch);
  return Status::OK();
}

namespace {

CHECKED_STATUS NotEnoughBytes(size_t present, size_t required, const Slice& full) {
  return STATUS_FORMAT(
      Corruption, "Not enough bytes in external intents $0 while $1 expected, full: $2",
      present, required, full.ToDebugHexString());
}

CHECKED_STATUS PrepareApplyExternalIntentsBatch(
    HybridTime commit_ht,
    const Slice& original_input_value,
    rocksdb::WriteBatch* regular_batch,
    IntraTxnWriteId* write_id) {
  auto input_value = original_input_value;
  DocHybridTimeBuffer doc_ht_buffer;
  RETURN_NOT_OK(input_value.consume_byte(ValueTypeAsChar::kUuid));
  Uuid status_tablet;
  RETURN_NOT_OK(status_tablet.FromSlice(input_value.Prefix(kUuidSize)));
  input_value.remove_prefix(kUuidSize);
  RETURN_NOT_OK(input_value.consume_byte(ValueTypeAsChar::kExternalIntents));
  for (;;) {
    auto key_size = VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&input_value));
    if (key_size == 0) {
      break;
    }
    if (input_value.size() < key_size) {
      return NotEnoughBytes(input_value.size(), key_size, original_input_value);
    }
    auto output_key = input_value.Prefix(key_size);
    input_value.remove_prefix(key_size);
    auto value_size = VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&input_value));
    if (input_value.size() < value_size) {
      return NotEnoughBytes(input_value.size(), value_size, original_input_value);
    }
    auto output_value = input_value.Prefix(value_size);
    input_value.remove_prefix(value_size);
    std::array<Slice, 2> key_parts = {{
        output_key,
        doc_ht_buffer.EncodeWithValueType(commit_ht, *write_id),
    }};
    std::array<Slice, 1> value_parts = {{
        output_value,
    }};
    regular_batch->Put(key_parts, value_parts);
    ++*write_id;
  }

  return Status::OK();
}

// Reads all stored external intents for provided transactions and prepares batches that will apply
// them into regular db and remove from intents db.
CHECKED_STATUS PrepareApplyExternalIntents(
    ExternalTxnApplyState* apply_external_transactions,
    rocksdb::WriteBatch* regular_batch,
    rocksdb::DB* intents_db,
    rocksdb::WriteBatch* intents_batch) {
  if (apply_external_transactions->empty()) {
    return Status::OK();
  }

  KeyBytes key_prefix;
  KeyBytes key_upperbound;
  Slice key_upperbound_slice;

  auto iter = CreateRocksDBIterator(
      intents_db, &KeyBounds::kNoBounds, BloomFilterMode::DONT_USE_BLOOM_FILTER,
      /* user_key_for_filter= */ boost::none,
      rocksdb::kDefaultQueryId, /* read_filter= */ nullptr, &key_upperbound_slice);

  for (auto& apply : *apply_external_transactions) {
    key_prefix.Clear();
    key_prefix.AppendValueType(ValueType::kExternalTransactionId);
    key_prefix.AppendRawBytes(apply.first.AsSlice());

    key_upperbound = key_prefix;
    key_upperbound.AppendValueType(ValueType::kMaxByte);
    key_upperbound_slice = key_upperbound.AsSlice();

    IntraTxnWriteId& write_id = apply.second.write_id;

    iter.Seek(key_prefix);
    while (iter.Valid()) {
      const Slice input_key(iter.key());

      if (!input_key.starts_with(key_prefix.AsSlice())) {
        break;
      }

      if (regular_batch) {
        RETURN_NOT_OK(PrepareApplyExternalIntentsBatch(
            apply.second.commit_ht, iter.value(), regular_batch, &write_id));
      }
      if (intents_batch) {
        intents_batch->SingleDelete(input_key);
      }

      iter.Next();
    }
  }

  return Status::OK();
}

ExternalTxnApplyState ProcessApplyExternalTransactions(const KeyValueWriteBatchPB& put_batch) {
  ExternalTxnApplyState result;
  for (const auto& apply : put_batch.apply_external_transactions()) {
    auto txn_id = CHECK_RESULT(FullyDecodeTransactionId(apply.transaction_id()));
    auto commit_ht = HybridTime(apply.commit_hybrid_time());
    result.emplace(
        txn_id,
        ExternalTxnApplyStateData{
          .commit_ht = commit_ht
        });
  }

  return result;
}

} // namespace

void AddPairToWriteBatch(
    const KeyValuePairPB& kv_pair,
    HybridTime hybrid_time,
    int write_id,
    ExternalTxnApplyState* apply_external_transactions,
    rocksdb::WriteBatch* regular_write_batch,
    rocksdb::WriteBatch* intents_write_batch) {
  DocHybridTimeBuffer doc_ht_buffer;
  size_t inverted_doc_ht_buffer[kMaxWordsPerEncodedHybridTimeWithValueType];

  CHECK(!kv_pair.key().empty());
  CHECK(!kv_pair.value().empty());

  bool regular_entry = kv_pair.key()[0] != ValueTypeAsChar::kExternalTransactionId;

#ifndef NDEBUG
  // Debug-only: ensure all keys we get in Raft replication can be decoded.
  if (regular_entry) {
    SubDocKey subdoc_key;
    Status s = subdoc_key.FullyDecodeFromKeyWithOptionalHybridTime(kv_pair.key());
    CHECK(s.ok())
        << "Failed decoding key: " << s.ToString() << "; "
        << "Problematic key: " << BestEffortDocDBKeyToStr(KeyBytes(kv_pair.key())) << "\n"
        << "value: " << FormatBytesAsStr(kv_pair.value());
  }
#endif

  // We replicate encoded SubDocKeys without a HybridTime at the end, and only append it here.
  // The reason for this is that the HybridTime timestamp is only picked at the time of
  // appending  an entry to the tablet's Raft log. Also this is a good way to save network
  // bandwidth.
  //
  // "Write id" is the final component of our HybridTime encoding (or, to be more precise,
  // DocHybridTime encoding) that helps disambiguate between different updates to the
  // same key (row/column) within a transaction. We set it based on the position of the write
  // operation in its write batch.

  hybrid_time = kv_pair.has_external_hybrid_time() ?
      HybridTime(kv_pair.external_hybrid_time()) : hybrid_time;
  std::array<Slice, 2> key_parts = {{
      Slice(kv_pair.key()),
      doc_ht_buffer.EncodeWithValueType(hybrid_time, write_id),
  }};
  Slice key_value = kv_pair.value();
  rocksdb::WriteBatch* batch;
  if (regular_entry) {
    batch = regular_write_batch;
  } else {
    // This entry contains external intents.
    Slice key = kv_pair.key();
    key.consume_byte();
    auto txn_id = CHECK_RESULT(DecodeTransactionId(&key));
    auto it = apply_external_transactions->find(txn_id);
    if (it != apply_external_transactions->end()) {
      // The same write operation could contain external intents and instruct us to apply them.
      CHECK_OK(PrepareApplyExternalIntentsBatch(
          it->second.commit_ht, key_value, regular_write_batch, &it->second.write_id));
      return;
    }
    batch = intents_write_batch;
    key_parts[1] = InvertedDocHt(key_parts[1], inverted_doc_ht_buffer);
  }
  constexpr size_t kNumValueParts = 1;
  batch->Put(key_parts, { &key_value, kNumValueParts });
}

// Usually put_batch contains only records that should be applied to regular DB.
// So apply_external_transactions will be empty and regular_entry will be true.
//
// But in general case on consumer side of CDC put_batch could contain various kinds of records,
// that should be applied into regular and intents db.
// They are:
// apply_external_transactions
//   The list of external transactions that should be applied.
//   For each such transaction we should lookup for existing external intents (stored in intents DB)
//   and convert them to Put command in regular_write_batch plus SingleDelete command in
//   intents_write_batch.
// write_pairs
//   Could contain regular entries, that should be stored into regular DB as is.
//   Also pair could contain external intents, that should be stored into intents DB.
//   But if apply_external_transactions contains transaction for those external intents, then
//   those intents will be applied directly to regular DB, avoiding unnecessary write to intents DB.
//   This case is very common for short running transactions.
void PrepareNonTransactionWriteBatch(
    const KeyValueWriteBatchPB& put_batch,
    HybridTime hybrid_time,
    rocksdb::DB* intents_db,
    rocksdb::WriteBatch* regular_write_batch,
    rocksdb::WriteBatch* intents_write_batch) {
  CHECK(put_batch.read_pairs().empty());

  auto apply_external_transactions = ProcessApplyExternalTransactions(put_batch);

  CHECK_OK(PrepareApplyExternalIntents(
      &apply_external_transactions, regular_write_batch, intents_db, intents_write_batch));

  for (int write_id = 0; write_id < put_batch.write_pairs_size(); ++write_id) {
    AddPairToWriteBatch(
        put_batch.write_pairs(write_id), hybrid_time, write_id, &apply_external_transactions,
        regular_write_batch, intents_write_batch);
  }
}

namespace {

// Checks if the given slice points to the part of an encoded SubDocKey past all of the subkeys
// (and definitely past all the hash/range keys). The only remaining part could be a hybrid time.
inline bool IsEndOfSubKeys(const Slice& key) {
  return key[0] == ValueTypeAsChar::kGroupEnd &&
         (key.size() == 1 || key[1] == ValueTypeAsChar::kHybridTime);
}

// Enumerates weak intents generated by the given key by invoking the provided callback with each
// weak intent stored in encoded_key_buffer. On return, *encoded_key_buffer contains the
// corresponding strong intent, for which the callback has not yet been called. It is expected
// that the caller would do so.
Status EnumerateWeakIntents(
    Slice key,
    const EnumerateIntentsCallback& functor,
    KeyBytes* encoded_key_buffer,
    PartialRangeKeyIntents partial_range_key_intents) {
  static const Slice kEmptyIntentValue;

  encoded_key_buffer->Clear();
  if (key.empty()) {
    return STATUS(Corruption, "An empty slice is not a valid encoded SubDocKey");
  }

  const bool has_cotable_id = *key.cdata() == ValueTypeAsChar::kTableId;
  const bool has_pgtable_id = *key.cdata() == ValueTypeAsChar::kPgTableOid;
  {
    bool is_table_root_key = false;
    if (has_cotable_id) {
      const auto kMinExpectedSize = kUuidSize + 2;
      if (key.size() < kMinExpectedSize) {
        return STATUS_FORMAT(
            Corruption,
            "Expected an encoded SubDocKey starting with a cotable id to be at least $0 bytes long",
            kMinExpectedSize);
      }
      encoded_key_buffer->AppendRawBytes(key.cdata(), kUuidSize + 1);
      is_table_root_key = key[kUuidSize + 1] == ValueTypeAsChar::kGroupEnd;
    } else if (has_pgtable_id) {
      const auto kMinExpectedSize = sizeof(PgTableOid) + 2;
      if (key.size() < kMinExpectedSize) {
        return STATUS_FORMAT(
            Corruption,
            "Expected an encoded SubDocKey starting with a pgtable id to be at least $0 bytes long",
            kMinExpectedSize);
      }
      encoded_key_buffer->AppendRawBytes(key.cdata(), sizeof(PgTableOid) + 1);
      is_table_root_key = key[sizeof(PgTableOid) + 1] == ValueTypeAsChar::kGroupEnd;
    } else {
      is_table_root_key = *key.cdata() == ValueTypeAsChar::kGroupEnd;
    }

    encoded_key_buffer->AppendValueType(ValueType::kGroupEnd);

    if (is_table_root_key) {
      // This must be a "table root" (or "tablet root") key (no hash components, no range
      // components, but the cotable might still be there). We are not really considering the case
      // of any subkeys under the empty key, so we can return here.
      return Status::OK();
    }
  }

  // For any non-empty key we already know that the empty key intent is weak.
  RETURN_NOT_OK(functor(
      IntentStrength::kWeak, FullDocKey::kFalse, kEmptyIntentValue, encoded_key_buffer,
      LastKey::kFalse));

  auto hashed_part_size = VERIFY_RESULT(DocKey::EncodedSize(key, DocKeyPart::kUpToHash));

  // Remove kGroupEnd that we just added to generate a weak intent.
  encoded_key_buffer->RemoveLastByte();

  if (hashed_part_size != encoded_key_buffer->size()) {
    // A hash component is present. Note that if cotable id is present, hashed_part_size would
    // also include it, so we only need to append the new bytes.
    encoded_key_buffer->AppendRawBytes(
        key.cdata() + encoded_key_buffer->size(), hashed_part_size - encoded_key_buffer->size());
    key.remove_prefix(hashed_part_size);
    if (key.empty()) {
      return STATUS(Corruption, "Range key part missing, expected at least a kGroupEnd");
    }

    // Append the kGroupEnd at the end for the empty range part to make this a valid encoded DocKey.
    encoded_key_buffer->AppendValueType(ValueType::kGroupEnd);
    if (IsEndOfSubKeys(key)) {
      // This means the key ends at the hash component -- no range keys and no subkeys.
      return Status::OK();
    }

    // Generate a week intent that only includes the hash component.
    RETURN_NOT_OK(functor(
        IntentStrength::kWeak, FullDocKey(key[0] == ValueTypeAsChar::kGroupEnd), kEmptyIntentValue,
        encoded_key_buffer, LastKey::kFalse));

    // Remove the kGroupEnd we added a bit earlier so we can append some range components.
    encoded_key_buffer->RemoveLastByte();
  } else {
    // No hash component.
    key.remove_prefix(hashed_part_size);
  }

  // Range components.
  auto range_key_start = key.cdata();
  while (VERIFY_RESULT(ConsumePrimitiveValueFromKey(&key))) {
    encoded_key_buffer->AppendRawBytes(range_key_start, key.cdata() - range_key_start);
    // We always need kGroupEnd at the end to make this a valid encoded DocKey.
    encoded_key_buffer->AppendValueType(ValueType::kGroupEnd);
    if (key.empty()) {
      return STATUS(Corruption, "Range key part is not terminated with a kGroupEnd");
    }
    if (IsEndOfSubKeys(key)) {
      // This is the last range key and there are no subkeys.
      return Status::OK();
    }
    FullDocKey full_doc_key(key[0] == ValueTypeAsChar::kGroupEnd);
    if (partial_range_key_intents || full_doc_key) {
      RETURN_NOT_OK(functor(
          IntentStrength::kWeak, full_doc_key, kEmptyIntentValue, encoded_key_buffer,
          LastKey::kFalse));
    }
    encoded_key_buffer->RemoveLastByte();
    range_key_start = key.cdata();
  }

  // We still need to append the kGroupEnd byte that closes the range portion to our buffer.
  // The corresponding kGroupEnd has already been consumed from the key slice by the last call to
  // ConsumePrimitiveValueFromKey, which returned false.
  encoded_key_buffer->AppendValueType(ValueType::kGroupEnd);

  auto subkey_start = key.cdata();
  while (VERIFY_RESULT(SubDocKey::DecodeSubkey(&key))) {
    encoded_key_buffer->AppendRawBytes(subkey_start, key.cdata() - subkey_start);
    if (key.empty() || *key.cdata() == ValueTypeAsChar::kHybridTime) {
      // This was the last subkey.
      return Status::OK();
    }
    RETURN_NOT_OK(functor(
        IntentStrength::kWeak, FullDocKey::kTrue, kEmptyIntentValue, encoded_key_buffer,
        LastKey::kFalse));
    subkey_start = key.cdata();
  }

  return STATUS(
      Corruption,
      "Expected to reach the end of the key after decoding last valid subkey");
}

}  // anonymous namespace

Status EnumerateIntents(
    Slice key, const Slice& intent_value, const EnumerateIntentsCallback& functor,
    KeyBytes* encoded_key_buffer, PartialRangeKeyIntents partial_range_key_intents,
    LastKey last_key) {
  RETURN_NOT_OK(EnumerateWeakIntents(
      key, functor, encoded_key_buffer, partial_range_key_intents));
  return functor(
      IntentStrength::kStrong, FullDocKey::kTrue, intent_value, encoded_key_buffer, last_key);
}

Status EnumerateIntents(
    const google::protobuf::RepeatedPtrField<KeyValuePairPB> &kv_pairs,
    const EnumerateIntentsCallback& functor, PartialRangeKeyIntents partial_range_key_intents) {
  KeyBytes encoded_key;

  for (int index = 0; index < kv_pairs.size(); ) {
    const auto &kv_pair = kv_pairs.Get(index);
    ++index;
    CHECK(!kv_pair.key().empty());
    CHECK(!kv_pair.value().empty());
    RETURN_NOT_OK(EnumerateIntents(
        kv_pair.key(), kv_pair.value(), functor, &encoded_key, partial_range_key_intents,
        LastKey(index == kv_pairs.size())));
  }

  return Status::OK();
}

class PrepareTransactionWriteBatchHelper {
 public:
  PrepareTransactionWriteBatchHelper(const PrepareTransactionWriteBatchHelper&) = delete;
  void operator=(const PrepareTransactionWriteBatchHelper&) = delete;

  // `rocksdb_write_batch` - in-out parameter is filled by this prepare.
  PrepareTransactionWriteBatchHelper(HybridTime hybrid_time,
                                     rocksdb::WriteBatch* rocksdb_write_batch,
                                     const TransactionId& transaction_id,
                                     const Slice& replicated_batches_state,
                                     IntraTxnWriteId* intra_txn_write_id)
      : hybrid_time_(hybrid_time),
        rocksdb_write_batch_(rocksdb_write_batch),
        transaction_id_(transaction_id),
        replicated_batches_state_(replicated_batches_state),
        intra_txn_write_id_(intra_txn_write_id) {
  }

  void Setup(
      IsolationLevel isolation_level,
      OperationKind kind,
      RowMarkType row_mark) {
    row_mark_ = row_mark;
    strong_intent_types_ = GetStrongIntentTypeSet(isolation_level, kind, row_mark);
  }

  // Using operator() to pass this object conveniently to EnumerateIntents.
  CHECKED_STATUS operator()(IntentStrength intent_strength, FullDocKey, Slice value_slice,
                            KeyBytes* key, LastKey last_key) {
    if (intent_strength == IntentStrength::kWeak) {
      weak_intents_[key->data()] |= StrongToWeak(strong_intent_types_);
      return Status::OK();
    }

    const auto transaction_value_type = ValueTypeAsChar::kTransactionId;
    const auto write_id_value_type = ValueTypeAsChar::kWriteId;
    const auto row_lock_value_type = ValueTypeAsChar::kRowLock;
    IntraTxnWriteId big_endian_write_id = BigEndian::FromHost32(*intra_txn_write_id_);
    std::array<Slice, 5> value = {{
        Slice(&transaction_value_type, 1),
        transaction_id_.AsSlice(),
        Slice(&write_id_value_type, 1),
        Slice(pointer_cast<char*>(&big_endian_write_id), sizeof(big_endian_write_id)),
        value_slice,
    }};
    // Store a row lock indicator rather than data (in value_slice) for row lock intents.
    if (IsValidRowMarkType(row_mark_)) {
      value.back() = Slice(&row_lock_value_type, 1);
    }

    ++*intra_txn_write_id_;

    char intent_type[2] = { ValueTypeAsChar::kIntentTypeSet,
                            static_cast<char>(strong_intent_types_.ToUIntPtr()) };

    DocHybridTimeBuffer doc_ht_buffer;

    constexpr size_t kNumKeyParts = 3;
    std::array<Slice, kNumKeyParts> key_parts = {{
        key->AsSlice(),
        Slice(intent_type, 2),
        doc_ht_buffer.EncodeWithValueType(hybrid_time_, write_id_++),
    }};

    Slice reverse_value_prefix;
    if (last_key && FLAGS_enable_transaction_sealing) {
      reverse_value_prefix = replicated_batches_state_;
    }
    AddIntent<kNumKeyParts>(
        transaction_id_, key_parts, value, rocksdb_write_batch_, reverse_value_prefix);

    return Status::OK();
  }

  void Finish() {
    char transaction_id_value_type = ValueTypeAsChar::kTransactionId;

    DocHybridTimeBuffer doc_ht_buffer;

    std::array<Slice, 2> value = {{
        Slice(&transaction_id_value_type, 1),
        transaction_id_.AsSlice(),
    }};

    if (PREDICT_TRUE(!FLAGS_TEST_docdb_sort_weak_intents_in_tests)) {
      for (const auto& intent_and_types : weak_intents_) {
        AddWeakIntent(intent_and_types, value, &doc_ht_buffer);
      }
    } else {
      // This is done in tests when deterministic DocDB state is required.
      std::vector<std::pair<KeyBuffer, IntentTypeSet>> intents_and_types(
          weak_intents_.begin(), weak_intents_.end());
      sort(intents_and_types.begin(), intents_and_types.end());
      for (const auto& intent_and_types : intents_and_types) {
        AddWeakIntent(intent_and_types, value, &doc_ht_buffer);
      }
    }
  }

 private:
  void AddWeakIntent(
      const std::pair<KeyBuffer, IntentTypeSet>& intent_and_types,
      const std::array<Slice, 2>& value,
      DocHybridTimeBuffer* doc_ht_buffer) {
    char intent_type[2] = { ValueTypeAsChar::kIntentTypeSet,
                            static_cast<char>(intent_and_types.second.ToUIntPtr()) };
    constexpr size_t kNumKeyParts = 3;
    std::array<Slice, kNumKeyParts> key = {{
        intent_and_types.first.AsSlice(),
        Slice(intent_type, 2),
        doc_ht_buffer->EncodeWithValueType(hybrid_time_, write_id_++),
    }};

    AddIntent<kNumKeyParts>(transaction_id_, key, value, rocksdb_write_batch_);
  }

  // TODO(dtxn) weak & strong intent in one batch.
  // TODO(dtxn) extract part of code knowning about intents structure to lower level.
  RowMarkType row_mark_;
  HybridTime hybrid_time_;
  rocksdb::WriteBatch* rocksdb_write_batch_;
  const TransactionId& transaction_id_;
  Slice replicated_batches_state_;
  IntentTypeSet strong_intent_types_;
  std::unordered_map<KeyBuffer, IntentTypeSet, ByteBufferHash> weak_intents_;
  IntraTxnWriteId write_id_ = 0;
  IntraTxnWriteId* intra_txn_write_id_;
};

// We have the following distinct types of data in this "intent store":
// Main intent data:
//   Prefix + SubDocKey (no HybridTime) + IntentType + HybridTime -> TxnId + value of the intent
// Transaction metadata
//   TxnId -> status tablet id + isolation level
// Reverse index by txn id
//   TxnId + HybridTime -> Main intent data key
//
// Where prefix is just a single byte prefix. TxnId, IntentType, HybridTime all prefixed with
// appropriate value type.
void PrepareTransactionWriteBatch(
    const KeyValueWriteBatchPB& put_batch,
    HybridTime hybrid_time,
    rocksdb::WriteBatch* rocksdb_write_batch,
    const TransactionId& transaction_id,
    IsolationLevel isolation_level,
    PartialRangeKeyIntents partial_range_key_intents,
    const Slice& replicated_batches_state,
    IntraTxnWriteId* write_id) {
  VLOG(4) << "PrepareTransactionWriteBatch(), write_id = " << *write_id;

  RowMarkType row_mark = GetRowMarkTypeFromPB(put_batch);

  PrepareTransactionWriteBatchHelper helper(
      hybrid_time, rocksdb_write_batch, transaction_id, replicated_batches_state, write_id);

  if (!put_batch.write_pairs().empty()) {
    if (IsValidRowMarkType(row_mark)) {
      LOG(WARNING) << "Performing a write with row lock "
                   << RowMarkType_Name(row_mark)
                   << " when only reads are expected";
    }
    helper.Setup(isolation_level, OperationKind::kWrite, row_mark);

    // We cannot recover from failures here, because it means that we cannot apply replicated
    // operation.
    CHECK_OK(EnumerateIntents(put_batch.write_pairs(), std::ref(helper),
             partial_range_key_intents));
  }

  if (!put_batch.read_pairs().empty()) {
    helper.Setup(isolation_level, OperationKind::kRead, row_mark);
    CHECK_OK(EnumerateIntents(put_batch.read_pairs(), std::ref(helper), partial_range_key_intents));
  }

  helper.Finish();
}

// ------------------------------------------------------------------------------------------------
// Standalone functions
// ------------------------------------------------------------------------------------------------

void AppendTransactionKeyPrefix(const TransactionId& transaction_id, KeyBytes* out) {
  out->AppendValueType(ValueType::kTransactionId);
  out->AppendRawBytes(transaction_id.AsSlice());
}

CHECKED_STATUS IntentToWriteRequest(
    const Slice& transaction_id_slice,
    HybridTime commit_ht,
    const Slice& reverse_index_key,
    const Slice& reverse_index_value,
    rocksdb::Iterator* intent_iter,
    rocksdb::WriteBatch* regular_batch,
    IntraTxnWriteId* write_id) {
  DocHybridTimeBuffer doc_ht_buffer;
  intent_iter->Seek(reverse_index_value);
  if (!intent_iter->Valid() || intent_iter->key() != reverse_index_value) {
    LOG(DFATAL) << "Unable to find intent: " << reverse_index_value.ToDebugHexString()
                << " for " << reverse_index_key.ToDebugHexString();
    return Status::OK();
  }
  auto intent = VERIFY_RESULT(ParseIntentKey(intent_iter->key(), transaction_id_slice));

  if (intent.types.Test(IntentType::kStrongWrite)) {
    IntraTxnWriteId stored_write_id;
    Slice intent_value;
    RETURN_NOT_OK(DecodeIntentValue(
        intent_iter->value(), transaction_id_slice, &stored_write_id, &intent_value));

    // Write id should match to one that were calculated during append of intents.
    // Doing it just for sanity check.
    DCHECK_GE(stored_write_id, *write_id)
      << "Value: " << intent_iter->value().ToDebugHexString();
    *write_id = stored_write_id;

    // Intents for row locks should be ignored (i.e. should not be written as regular records).
    if (intent_value.starts_with(ValueTypeAsChar::kRowLock)) {
      return Status::OK();
    }

    // After strip of prefix and suffix intent_key contains just SubDocKey w/o a hybrid time.
    // Time will be added when writing batch to RocksDB.
    std::array<Slice, 2> key_parts = {{
        intent.doc_path,
        doc_ht_buffer.EncodeWithValueType(commit_ht, *write_id),
    }};
    std::array<Slice, 2> value_parts = {{
        intent.doc_ht,
        intent_value,
    }};

    // Useful when debugging transaction failure.
#if defined(DUMP_APPLY)
    SubDocKey sub_doc_key;
    CHECK_OK(sub_doc_key.FullyDecodeFrom(intent.doc_path, HybridTimeRequired::kFalse));
    if (!sub_doc_key.subkeys().empty()) {
      auto txn_id = FullyDecodeTransactionId(transaction_id_slice);
      LOG(INFO) << "Apply: " << sub_doc_key.ToString()
                << ", time: " << commit_ht << ", write id: " << *write_id << ", txn: " << txn_id
                << ", value: " << intent_value.ToDebugString();
    }
#endif

    regular_batch->Put(key_parts, value_parts);
    ++*write_id;
  }

  return Status::OK();
}

template <size_t N>
void PutApplyState(
    const Slice& transaction_id_slice, HybridTime commit_ht, IntraTxnWriteId write_id,
    const std::array<Slice, N>& value_parts, rocksdb::WriteBatch* regular_batch) {
  char transaction_apply_state_value_type = ValueTypeAsChar::kTransactionApplyState;
  char group_end_value_type = ValueTypeAsChar::kGroupEnd;
  char hybrid_time_value_type = ValueTypeAsChar::kHybridTime;
  DocHybridTime doc_hybrid_time(commit_ht, write_id);
  char doc_hybrid_time_buffer[kMaxBytesPerEncodedHybridTime];
  char* doc_hybrid_time_buffer_end = doc_hybrid_time.EncodedInDocDbFormat(
      doc_hybrid_time_buffer);
  std::array<Slice, 5> key_parts = {{
      Slice(&transaction_apply_state_value_type, 1),
      transaction_id_slice,
      Slice(&group_end_value_type, 1),
      Slice(&hybrid_time_value_type, 1),
      Slice(doc_hybrid_time_buffer, doc_hybrid_time_buffer_end),
  }};
  regular_batch->Put(key_parts, value_parts);
}

ApplyTransactionState StoreApplyState(
    const Slice& transaction_id_slice, const Slice& key, IntraTxnWriteId write_id,
    HybridTime commit_ht, rocksdb::WriteBatch* regular_batch) {
  auto result = ApplyTransactionState {
    .key = key.ToBuffer(),
    .write_id = write_id,
  };
  ApplyTransactionStatePB pb;
  result.ToPB(&pb);
  pb.set_commit_ht(commit_ht.ToUint64());
  faststring encoded_pb;
  pb_util::SerializeToString(pb, &encoded_pb);
  char string_value_type = ValueTypeAsChar::kString;
  std::array<Slice, 2> value_parts = {{
    Slice(&string_value_type, 1),
    Slice(encoded_pb.data(), encoded_pb.size())
  }};
  PutApplyState(transaction_id_slice, commit_ht, write_id, value_parts, regular_batch);
  return result;
}

Result<ApplyTransactionState> PrepareApplyIntentsBatch(
    const TransactionId& transaction_id,
    HybridTime commit_ht,
    const KeyBounds* key_bounds,
    const ApplyTransactionState* apply_state,
    rocksdb::WriteBatch* regular_batch,
    rocksdb::DB* intents_db,
    rocksdb::WriteBatch* intents_batch) {
  SCHECK_EQ((regular_batch != nullptr) + (intents_batch != nullptr), 1, InvalidArgument,
            "Exactly one write batch should be non-null, either regular or intents");

  // regular_batch or intents_batch could be null. In this case we don't fill apply batch for
  // appropriate DB.

  KeyBytes txn_reverse_index_prefix;
  Slice transaction_id_slice = transaction_id.AsSlice();
  AppendTransactionKeyPrefix(transaction_id, &txn_reverse_index_prefix);
  txn_reverse_index_prefix.AppendValueType(ValueType::kMaxByte);
  Slice key_prefix = txn_reverse_index_prefix.AsSlice();
  key_prefix.remove_suffix(1);
  const Slice reverse_index_upperbound = txn_reverse_index_prefix.AsSlice();

  auto reverse_index_iter = CreateRocksDBIterator(
      intents_db, &KeyBounds::kNoBounds, BloomFilterMode::DONT_USE_BLOOM_FILTER, boost::none,
      rocksdb::kDefaultQueryId, nullptr /* read_filter */, &reverse_index_upperbound);

  BoundedRocksDbIterator intent_iter;

  // If we don't have regular_batch, it means that we are just removing intents, i.e. when a
  // transaction has been aborted. We don't need the intent iterator in that case, because the
  // reverse index iterator is sufficient.
  if (regular_batch) {
    intent_iter = CreateRocksDBIterator(
        intents_db, key_bounds, BloomFilterMode::DONT_USE_BLOOM_FILTER, boost::none,
        rocksdb::kDefaultQueryId);
  }

  reverse_index_iter.Seek(key_prefix);

  DocHybridTimeBuffer doc_ht_buffer;

  const auto& log_prefix = intents_db->GetOptions().log_prefix;

  IntraTxnWriteId write_id = 0;
  if (apply_state) {
    reverse_index_iter.Seek(apply_state->key);
    write_id = apply_state->write_id;
    if (regular_batch) {
      // This sanity check is invalid for remove case, because .SST file could be deleted.
      LOG_IF(DFATAL, !reverse_index_iter.Valid() || reverse_index_iter.key() != apply_state->key)
          << "Continue from wrong key: " << Slice(apply_state->key).ToDebugString() << ", txn: "
          << transaction_id << ", position: "
          << (reverse_index_iter.Valid() ? reverse_index_iter.key().ToDebugString() : "<INVALID>")
          << ", write id: " << write_id;
    }
  }

  const uint64_t max_records = FLAGS_txn_max_apply_batch_records;
  const uint64_t write_id_limit = write_id + max_records;
  while (reverse_index_iter.Valid()) {
    const Slice key_slice(reverse_index_iter.key());

    if (!key_slice.starts_with(key_prefix)) {
      break;
    }

    VLOG(4) << log_prefix << "Apply reverse index record to ["
            << (regular_batch ? "R" : "") << (intents_batch ? "I" : "")
            << "]: " << EntryToString(reverse_index_iter, StorageDbType::kIntents);

    // If the key ends at the transaction id then it is transaction metadata (status tablet,
    // isolation level etc.).
    if (key_slice.size() > txn_reverse_index_prefix.size()) {
      auto reverse_index_value = reverse_index_iter.value();
      if (!reverse_index_value.empty() && reverse_index_value[0] == ValueTypeAsChar::kBitSet) {
        CHECK(!FLAGS_TEST_fail_on_replicated_batch_idx_set_in_txn_record);
        reverse_index_value.remove_prefix(1);
        RETURN_NOT_OK(OneWayBitmap::Skip(&reverse_index_value));
      }

      // Value of reverse index is a key of original intent record, so seek it and check match.
      if (regular_batch &&
          (!key_bounds || key_bounds->IsWithinBounds(reverse_index_iter.value()))) {
        // We store apply state only if there are some more intents left.
        // So doing this check here, instead of right after write_id was incremented.
        if (write_id >= write_id_limit) {
          return StoreApplyState(
              transaction_id_slice, key_slice, write_id, commit_ht, regular_batch);
        }
        RETURN_NOT_OK(IntentToWriteRequest(
            transaction_id_slice, commit_ht, key_slice, reverse_index_value,
            &intent_iter, regular_batch, &write_id));
      }

      if (intents_batch) {
        intents_batch->SingleDelete(reverse_index_value);
      }
    }

    if (intents_batch) {
      if (intents_batch->Count() >= max_records) {
        return ApplyTransactionState {
          .key = key_slice.ToBuffer(),
          .write_id = write_id,
        };
      }

      intents_batch->SingleDelete(key_slice);
    }

    reverse_index_iter.Next();
  }

  if (apply_state && regular_batch) {
    char tombstone_value_type = ValueTypeAsChar::kTombstone;
    std::array<Slice, 1> value_parts = {{Slice(&tombstone_value_type, 1)}};
    PutApplyState(transaction_id_slice, commit_ht, write_id, value_parts, regular_batch);
  }

  return ApplyTransactionState {};
}

std::string ApplyTransactionState::ToString() const {
  return Format("{ key: $0 write_id: $1 }", Slice(key).ToDebugString(), write_id);
}

void CombineExternalIntents(
    const TransactionId& txn_id,
    ExternalIntentsProvider* provider) {
  // External intents are stored in the following format:
  // key: kExternalTransactionId, txn_id
  // value: size(intent1_key), intent1_key, size(intent1_value), intent1_value, size(intent2_key)...
  // where size is encoded as varint.

  docdb::KeyBytes buffer;
  buffer.AppendValueType(ValueType::kExternalTransactionId);
  buffer.AppendRawBytes(txn_id.AsSlice());
  provider->SetKey(buffer.AsSlice());
  buffer.Clear();
  buffer.AppendValueType(ValueType::kUuid);
  buffer.AppendRawBytes(provider->InvolvedTablet().AsSlice());
  buffer.AppendValueType(ValueType::kExternalIntents);
  while (auto key_value = provider->Next()) {
    buffer.AppendUInt64AsVarInt(key_value->first.size());
    buffer.AppendRawBytes(key_value->first);
    buffer.AppendUInt64AsVarInt(key_value->second.size());
    buffer.AppendRawBytes(key_value->second);
  }
  buffer.AppendUInt64AsVarInt(0);
  provider->SetValue(buffer.AsSlice());
}

}  // namespace docdb
}  // namespace yb
