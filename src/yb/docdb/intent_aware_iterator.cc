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

#include "yb/docdb/intent_aware_iterator.h"

#include <future>
#include <thread>
#include <boost/optional/optional_io.hpp>

#include "yb/common/doc_hybrid_time.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/docdb/conflict_resolution.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/value.h"

using namespace std::literals;

DEFINE_bool(transaction_allow_rerequest_status_in_tests, true,
            "Allow rerequest transaction status when try again is received.");

namespace yb {
namespace docdb {

namespace {

void GetIntentPrefixForKeyWithoutHt(const Slice& key, KeyBytes* out) {
  out->Clear();
  // Since caller guarantees that key_bytes doesn't have hybrid time, we can simply prepend
  // kIntentPrefix in order to get prefix for all related intents.
  out->Reserve(key.size() + 1);
  out->AppendValueType(ValueType::kIntentPrefix);
  out->AppendRawBytes(key);
}

KeyBytes GetIntentPrefixForKeyWithoutHt(const Slice& key) {
  KeyBytes result;
  GetIntentPrefixForKeyWithoutHt(key, &result);
  return result;
}

void AppendEncodedDocHt(const Slice& encoded_doc_ht, KeyBytes* key_bytes) {
  key_bytes->AppendValueType(ValueType::kHybridTime);
  key_bytes->AppendRawBytes(encoded_doc_ht);
}

} // namespace

// For locally committed transactions returns commit time if committed at specified time or
// HybridTime::kMin otherwise. For other transactions returns HybridTime::kInvalid.
HybridTime TransactionStatusCache::GetLocalCommitTime(const TransactionId& transaction_id) {
  const HybridTime local_commit_time = txn_status_manager_->LocalCommitTime(transaction_id);
  return local_commit_time.is_valid()
      ? local_commit_time <= read_time_.global_limit ? local_commit_time : HybridTime::kMin
      : local_commit_time;
}

Result<HybridTime> TransactionStatusCache::GetCommitTime(const TransactionId& transaction_id) {
  auto it = cache_.find(transaction_id);
  if (it != cache_.end()) {
    return it->second;
  }

  auto result = DoGetCommitTime(transaction_id);
  if (result.ok()) {
    cache_.emplace(transaction_id, *result);
  }
  return result;
}

Result<HybridTime> TransactionStatusCache::DoGetCommitTime(const TransactionId& transaction_id) {
  HybridTime local_commit_time = GetLocalCommitTime(transaction_id);
  if (local_commit_time.is_valid()) {
    return local_commit_time;
  }

  TransactionStatusResult txn_status;
  for(;;) {
    std::promise<Result<TransactionStatusResult>> txn_status_promise;
    auto future = txn_status_promise.get_future();
    auto callback = [&txn_status_promise](Result<TransactionStatusResult> result) {
      txn_status_promise.set_value(std::move(result));
    };
    txn_status_manager_->RequestStatusAt(
        {&transaction_id, read_time_.read, read_time_.global_limit, read_time_.serial_no,
              callback});
    future.wait();
    auto txn_status_result = future.get();
    if (txn_status_result.ok()) {
      txn_status = std::move(*txn_status_result);
      break;
    }
    LOG(WARNING)
        << "Failed to request transaction " << yb::ToString(transaction_id) << " status: "
        <<  txn_status_result.status();
    if (!txn_status_result.status().IsTryAgain()) {
      return std::move(txn_status_result.status());
    }
    DCHECK(FLAGS_transaction_allow_rerequest_status_in_tests);
    // TODO(dtxn) In case of TryAgain error status we need to re-request transaction status.
    // Temporary workaround is to sleep for 0.05s and re-request.
    std::this_thread::sleep_for(50ms);
  }
  VLOG(4) << "Transaction_id " << transaction_id << " at " << read_time_
          << ": status: " << TransactionStatus_Name(txn_status.status)
          << ", status_time: " << txn_status.status_time;
  // There could be case when transaction was committed and applied between previous call to
  // GetLocalCommitTime, in this case coordinator does not know transaction and will respond
  // with ABORTED status. So we recheck whether it was committed locally.
  if (txn_status.status == TransactionStatus::ABORTED) {
    local_commit_time = GetLocalCommitTime(transaction_id);
    return local_commit_time.is_valid() ? local_commit_time : HybridTime::kMin;
  } else {
    return txn_status.status == TransactionStatus::COMMITTED ? txn_status.status_time
        : HybridTime::kMin;
  }
}

namespace {

struct DecodeStrongWriteIntentResult {
  Slice intent_prefix;
  Slice intent_value;
  DocHybridTime value_time;
  IntentType intent_type;

  // Whether this intent from the same transaction as specified in context.
  bool same_transaction = false;

  std::string ToString() const {
    return Format("{ intent_prefix: $0 intent_value: $1 value_time: $2 same_transaction: $3 "
                  "intent_type: $4 }",
                  intent_prefix.ToDebugHexString(), intent_value.ToDebugHexString(), value_time,
                  same_transaction, yb::ToString(intent_type));
  }
};

std::ostream& operator<<(std::ostream& out, const DecodeStrongWriteIntentResult& result) {
  return out << result.ToString();
}

// Decodes intent based on intent_iterator and its transaction commit time if intent is a strong
// write intent and transaction is already committed at specified time or it is current transaction.
// Returns HybridTime::kMin as value_time otherwise.
// For current transaction returns intent record hybrid time as value_time.
// Consumes intent from value_slice leaving only value itself.
Result<DecodeStrongWriteIntentResult> DecodeStrongWriteIntent(
    TransactionOperationContext txn_op_context, rocksdb::Iterator* intent_iter,
    TransactionStatusCache* transaction_status_cache) {
  DocHybridTime intent_ht;
  DecodeStrongWriteIntentResult result;
  RETURN_NOT_OK(DecodeIntentKey(
      intent_iter->key(), &result.intent_prefix, &result.intent_type, &intent_ht));
  if (IsStrongWriteIntent(result.intent_type)) {
    result.intent_value = intent_iter->value();
    auto txn_id = VERIFY_RESULT(DecodeTransactionIdFromIntentValue(&result.intent_value));
    result.same_transaction = txn_id == txn_op_context.transaction_id;
    if (result.intent_value.size() < 1 + sizeof(IntraTxnWriteId) ||
        result.intent_value[0] != ValueTypeAsChar::kWriteId) {
      return STATUS_FORMAT(
          Corruption, "Write id is missing in $0", intent_iter->value().ToDebugHexString());
    }
    result.intent_value.consume_byte();
    IntraTxnWriteId in_txn_write_id = BigEndian::Load32(result.intent_value.data());
    result.intent_value.remove_prefix(sizeof(IntraTxnWriteId));
    if (result.same_transaction) {
      result.value_time = intent_ht;
    } else {
      auto commit_ht = VERIFY_RESULT(transaction_status_cache->GetCommitTime(txn_id));
      result.value_time = DocHybridTime(
          commit_ht, commit_ht != HybridTime::kMin ? in_txn_write_id : 0);
      VLOG(4) << "Transaction id: " << txn_id << ", value time: " << result.value_time
              << ", value: " << result.intent_value.ToDebugHexString();
    }
  } else {
    result.value_time = DocHybridTime::kMin;
  }
  return result;
}

// Given that key is well-formed DocDB encoded key, checks if it is an intent key for the same key
// as intent_prefix. If key is not well-formed DocDB encoded key, result could be true or false.
bool IsIntentForTheSameKey(const Slice& key, const Slice& intent_prefix) {
  return key.starts_with(intent_prefix)
      && key.size() > intent_prefix.size()
      && key[intent_prefix.size()] == ValueTypeAsChar::kIntentType;
}

std::string DebugDumpKeyToStr(const Slice &key) {
  SubDocKey key_decoded;
  CHECK(key_decoded.FullyDecodeFrom(key).ok());
  return key.ToDebugHexString() + " (" + key_decoded.ToString() + ")";
}

std::string DebugDumpKeyToStr(const KeyBytes &key) {
  return DebugDumpKeyToStr(key.AsSlice());
}

bool DebugHasHybridTime(const Slice& subdoc_key_encoded) {
  SubDocKey subdoc_key;
  CHECK(subdoc_key.FullyDecodeFromKeyWithOptionalHybridTime(subdoc_key_encoded).ok());
  return subdoc_key.has_hybrid_time();
}

} // namespace

IntentAwareIterator::IntentAwareIterator(
    rocksdb::DB* rocksdb,
    const rocksdb::ReadOptions& read_opts,
    const ReadHybridTime& read_time,
    const TransactionOperationContextOpt& txn_op_context)
    : read_time_(read_time),
      encoded_read_time_local_limit_(
          DocHybridTime(read_time_.local_limit, kMaxWriteId).EncodedInDocDbFormat()),
      encoded_read_time_global_limit_(
          DocHybridTime(read_time_.global_limit, kMaxWriteId).EncodedInDocDbFormat()),
      txn_op_context_(txn_op_context),
      transaction_status_cache_(
          txn_op_context ? &txn_op_context->txn_status_manager : nullptr, read_time) {
  VLOG(4) << "IntentAwareIterator, read_time: " << read_time
          << ", txp_op_context: " << txn_op_context_;
  if (txn_op_context.is_initialized()) {
    intent_iter_ = docdb::CreateRocksDBIterator(rocksdb,
                                                docdb::BloomFilterMode::DONT_USE_BLOOM_FILTER,
                                                boost::none,
                                                rocksdb::kDefaultQueryId,
                                                nullptr /* file_filter */,
                                                &intent_upperbound_);
  }
  iter_.reset(rocksdb->NewIterator(read_opts));
}

void IntentAwareIterator::Seek(const DocKey &doc_key) {
  Seek(doc_key.Encode());
}

void IntentAwareIterator::Seek(const Slice& key) {
  VLOG(4) << "Seek(" << SubDocKey::DebugSliceToString(key) << ")";
  DOCDB_DEBUG_SCOPE_LOG(
      key.ToDebugString(),
      std::bind(&IntentAwareIterator::DebugDump, this));
  if (!status_.ok()) {
    return;
  }

  ROCKSDB_SEEK(iter_.get(), key);
  skip_future_records_needed_ = true;
  if (intent_iter_) {
    status_ = SetIntentUpperbound();
    if (!status_.ok()) {
      return;
    }
    ROCKSDB_SEEK(intent_iter_.get(), GetIntentPrefixForKeyWithoutHt(key));
    SeekForwardToSuitableIntent();
  }
}

void IntentAwareIterator::SeekForward(const Slice& key) {
  KeyBytes key_bytes;
  // Reserve space for key plus kMaxBytesPerEncodedHybridTime + 1 bytes for SeekForward() below to
  // avoid extra realloc while appending the read time.
  key_bytes.Reserve(key.size() + kMaxBytesPerEncodedHybridTime + 1);
  key_bytes.AppendRawBytes(key);
  SeekForward(&key_bytes);
}

void IntentAwareIterator::SeekForward(KeyBytes* key_bytes) {
  VLOG(4) << "SeekForward(" << SubDocKey::DebugSliceToString(*key_bytes) << ")";
  DOCDB_DEBUG_SCOPE_LOG(
      SubDocKey::DebugSliceToString(*key_bytes),
      std::bind(&IntentAwareIterator::DebugDump, this));
  if (!status_.ok()) {
    return;
  }

  const size_t key_size = key_bytes->size();
  AppendEncodedDocHt(encoded_read_time_global_limit_, key_bytes);
  SeekForwardRegular(*key_bytes);
  key_bytes->Truncate(key_size);
  if (intent_iter_ && status_.ok()) {
    status_ = SetIntentUpperbound();
    if (!status_.ok()) {
      return;
    }
    GetIntentPrefixForKeyWithoutHt(*key_bytes, &seek_key_buffer_);
    SeekForwardToSuitableIntent(seek_key_buffer_);
  }
}

void IntentAwareIterator::SeekPastSubKey(const Slice& key) {
  VLOG(4) << "SeekPastSubKey(" << SubDocKey::DebugSliceToString(key) << ")";
  if (!status_.ok()) {
    return;
  }

  docdb::SeekPastSubKey(key, iter_.get());
  skip_future_records_needed_ = true;
  if (intent_iter_ && status_.ok()) {
    status_ = SetIntentUpperbound();
    if (!status_.ok()) {
      return;
    }
    KeyBytes intent_prefix = GetIntentPrefixForKeyWithoutHt(key);
    // Skip all intents for subdoc_key.
    intent_prefix.mutable_data()->push_back(ValueTypeAsChar::kIntentType + 1);
    SeekForwardToSuitableIntent(intent_prefix);
  }
}

void IntentAwareIterator::SeekOutOfSubDoc(KeyBytes* key_bytes) {
  VLOG(4) << "SeekOutOfSubDoc(" << SubDocKey::DebugSliceToString(*key_bytes) << ")";
  if (!status_.ok()) {
    return;
  }

  docdb::SeekOutOfSubKey(key_bytes, iter_.get());
  if (intent_iter_ && status_.ok()) {
    status_ = SetIntentUpperbound();
    if (!status_.ok()) {
      return;
    }
    GetIntentPrefixForKeyWithoutHt(*key_bytes, &seek_key_buffer_);
    // See comment for SubDocKey::AdvanceOutOfSubDoc.
    seek_key_buffer_.AppendValueType(ValueType::kMaxByte);
    SeekForwardToSuitableIntent(seek_key_buffer_);
  }
}

void IntentAwareIterator::SeekOutOfSubDoc(const Slice& key) {
  KeyBytes key_bytes;
  // Reserve space for key + 1 byte for docdb::SeekOutOfSubKey() above to avoid extra realloc while
  // appending kMaxByte.
  key_bytes.Reserve(key.size() + 1);
  key_bytes.AppendRawBytes(key);
  SeekOutOfSubDoc(&key_bytes);
}

void IntentAwareIterator::SeekToLastDocKey() {
  if (intent_iter_) {
    // TODO (dtxn): Implement SeekToLast when inten intents are present. Since part of the
    // is made of intents, we may have to avoid that. This is needed when distributed txns are fully
    // supported.
    return;
  }
  iter_->SeekToLast();
  if (!iter_->Valid()) {
    return;
  }
  // Seek to the first rocksdb kv-pair for this row.
  rocksdb::Slice rocksdb_key(iter_->key());
  DocKey doc_key;
  status_ = doc_key.DecodeFrom(&rocksdb_key);
  if (!status_.ok()) {
    return;
  }
  Seek(doc_key.Encode());
}

void IntentAwareIterator::PrevDocKey(const DocKey& doc_key) {
  Seek(doc_key);
  if (!status_.ok()) {
    return;
  }
  if (!iter_->Valid()) {
    SeekToLastDocKey();
    return;
  }
  iter_->Prev();
  if (!iter_->Valid()) {
    iter_valid_ = false; // TODO(dtxn) support reverse scan with read restart
    return;
  }
  Slice key_slice = iter_->key();
  DocKey prev_key;
  status_ = prev_key.DecodeFrom(&key_slice);
  if (!status_.ok()) {
    return;
  }
  Seek(prev_key);
}

bool IntentAwareIterator::valid() {
  if (skip_future_records_needed_) {
    SkipFutureRecords();
    skip_future_records_needed_ = false;
  }
  if (skip_future_intents_needed_) {
    SkipFutureIntents();
    skip_future_intents_needed_ = false;
  }
  return !status_.ok() || iter_valid_ || resolved_intent_state_ == ResolvedIntentState::kValid;
}

bool IntentAwareIterator::IsEntryRegular() {
  if (PREDICT_FALSE(!iter_valid_)) {
    return false;
  }
  if (resolved_intent_state_ == ResolvedIntentState::kValid) {
    return iter_->key().compare(resolved_intent_sub_doc_key_encoded_) < 0;
  }
  return true;
}

Result<Slice> IntentAwareIterator::FetchKey(DocHybridTime* doc_ht) {
  RETURN_NOT_OK(status_);
  Slice result;
  DocHybridTime doc_ht_seen;
  if (IsEntryRegular()) {
    result = iter_->key();
    doc_ht_seen = VERIFY_RESULT(DocHybridTime::DecodeFromEnd(&result));
    DCHECK(result.ends_with(ValueTypeAsChar::kHybridTime)) << result.ToDebugString();
    result.remove_suffix(1);
  } else {
    DCHECK_EQ(ResolvedIntentState::kValid, resolved_intent_state_);
    result = resolved_intent_sub_doc_key_;
    doc_ht_seen = resolved_intent_txn_dht_;
  }
  if (doc_ht != nullptr) {
    *doc_ht = doc_ht_seen;
  }
  max_seen_ht_.MakeAtLeast(doc_ht_seen.hybrid_time());
  VLOG(4) << "Fetched key " << SubDocKey::DebugSliceToString(result)
          << ", with time: " << doc_ht_seen
          << ", while read bounds are: " << read_time_;
  return result;
}

Slice IntentAwareIterator::value() {
  if (IsEntryRegular()) {
    return iter_->value();
  } else {
    DCHECK_EQ(ResolvedIntentState::kValid, resolved_intent_state_);
    return resolved_intent_value_;
  }
}

void IntentAwareIterator::SeekForwardRegular(const Slice& slice) {
  VLOG(4) << "SeekForwardRegular(" << SubDocKey::DebugSliceToString(slice) << ")";
  docdb::SeekForward(slice, iter_.get());
  skip_future_records_needed_ = true;
}

void IntentAwareIterator::ProcessIntent() {
  auto decode_result = DecodeStrongWriteIntent(
      txn_op_context_.get(), intent_iter_.get(), &transaction_status_cache_);
  if (!decode_result.ok()) {
    status_ = decode_result.status();
    return;
  }
  VLOG(4) << "Intent decode: " << DebugIntentKeyToString(intent_iter_->key())
          << " => " << intent_iter_->value().ToDebugHexString() << ", result: " << *decode_result;
  DOCDB_DEBUG_LOG(
      "resolved_intent_txn_dht_: $0 value_time: $1 read_time: $2",
      resolved_intent_txn_dht_.ToString(),
      decode_result->value_time.ToString(),
      read_time_.ToString());
  auto real_time = decode_result->same_transaction ? intent_dht_from_same_txn_
                                                   : resolved_intent_txn_dht_;
  if (decode_result->value_time > real_time &&
      (decode_result->same_transaction ||
       decode_result->value_time.hybrid_time() <= read_time_.global_limit)) {
    if (resolved_intent_state_ == ResolvedIntentState::kNoIntent) {
      resolved_intent_key_prefix_.Reset(decode_result->intent_prefix);
      auto prefix = prefix_stack_.empty() ? Slice() : prefix_stack_.back();
      status_ = decode_result->intent_prefix.consume_byte(ValueTypeAsChar::kIntentPrefix);
      if (!status_.ok()) {
        status_ = status_.CloneAndPrepend("Bad intent prefix");
        return;
      }
      resolved_intent_state_ =
          decode_result->intent_prefix.starts_with(prefix) ? ResolvedIntentState::kValid
          : ResolvedIntentState::kInvalidPrefix;
    }
    if (decode_result->same_transaction) {
      intent_dht_from_same_txn_ = decode_result->value_time;
      resolved_intent_txn_dht_ = DocHybridTime(read_time_.read, kMaxWriteId);
    } else {
      resolved_intent_txn_dht_ = decode_result->value_time;
    }
    resolved_intent_value_.Reset(decode_result->intent_value);
  }
}

void IntentAwareIterator::UpdateResolvedIntentSubDocKeyEncoded() {
  resolved_intent_sub_doc_key_ = resolved_intent_key_prefix_;
  status_ = resolved_intent_sub_doc_key_.consume_byte(ValueTypeAsChar::kIntentPrefix);
  if (!status_.ok()) {
    return;
  }
  resolved_intent_sub_doc_key_encoded_.Reset(resolved_intent_sub_doc_key_);
  resolved_intent_sub_doc_key_encoded_.AppendValueType(ValueType::kHybridTime);
  resolved_intent_sub_doc_key_encoded_.AppendHybridTime(resolved_intent_txn_dht_);
  VLOG(4) << "Resolved intent SubDocKey: "
          << DebugDumpKeyToStr(resolved_intent_sub_doc_key_encoded_);
}

void IntentAwareIterator::SeekForwardToSuitableIntent(const KeyBytes &intent_key_prefix) {
  DOCDB_DEBUG_SCOPE_LOG(intent_key_prefix.ToString(),
                        std::bind(&IntentAwareIterator::DebugDump, this));
  if (resolved_intent_state_ != ResolvedIntentState::kNoIntent &&
      resolved_intent_key_prefix_.CompareTo(intent_key_prefix) >= 0) {
    return;
  }
  // Use ROCKSDB_SEEK() to force re-seek of "intent_iter_" in case the iterator was invalid by the
  // previous intent upperbound, but the upperbound has changed therefore requiring re-seek.
  ROCKSDB_SEEK(intent_iter_.get(), intent_key_prefix.AsSlice());
  SeekForwardToSuitableIntent();
}

void IntentAwareIterator::SeekForwardToSuitableIntent() {
  DOCDB_DEBUG_SCOPE_LOG("", std::bind(&IntentAwareIterator::DebugDump, this));
  resolved_intent_state_ = ResolvedIntentState::kNoIntent;
  resolved_intent_txn_dht_ = DocHybridTime::kMin;
  auto prefix = prefix_stack_.empty() ? Slice() : prefix_stack_.back();

  // Find latest suitable intent for the first SubDocKey having suitable intents.
  while (intent_iter_->Valid()) {
    auto intent_key = intent_iter_->key();
    VLOG(4) << "Intent found: " << DebugIntentKeyToString(intent_key)
            << ", resolved state: " << yb::ToString(resolved_intent_state_);
    if (resolved_intent_state_ != ResolvedIntentState::kNoIntent &&
        // Only scan intents for the first SubDocKey having suitable intents.
        !IsIntentForTheSameKey(intent_key, resolved_intent_key_prefix_)) {
      break;
    }
    status_ = intent_key.consume_byte(ValueTypeAsChar::kIntentPrefix);
    if (!status_.ok()) {
      status_ = status_.CloneAndPrepend("Bad intent key");
      return;
    }
    if (!intent_key.starts_with(prefix)) {
      break;
    }
    ProcessIntent();
    if (!status_.ok()) {
      return;
    }
    intent_iter_->Next();
  }
  if (resolved_intent_state_ != ResolvedIntentState::kNoIntent) {
    UpdateResolvedIntentSubDocKeyEncoded();
  }
}

void IntentAwareIterator::DebugDump() {
  LOG(INFO) << ">> IntentAwareIterator dump";
  LOG(INFO) << "iter_->Valid(): " << iter_->Valid();
  if (iter_->Valid()) {
    LOG(INFO) << "iter_->key(): " << DebugDumpKeyToStr(iter_->key());
  }
  if (intent_iter_) {
    LOG(INFO) << "intent_iter_->Valid(): " << intent_iter_->Valid();
    if (intent_iter_->Valid()) {
      LOG(INFO) << "intent_iter_->key(): " << intent_iter_->key().ToDebugHexString();
    }
  }
  LOG(INFO) << "resolved_intent_state_: " << yb::ToString(resolved_intent_state_);
  if (resolved_intent_state_ != ResolvedIntentState::kNoIntent) {
    LOG(INFO) << "resolved_intent_sub_doc_key_encoded_: "
              << DebugDumpKeyToStr(resolved_intent_sub_doc_key_encoded_);
  }
  LOG(INFO) << "valid(): " << valid();
  if (valid()) {
    DocHybridTime doc_ht;
    auto key = FetchKey(&doc_ht);
    if (key.ok()) {
      LOG(INFO) << "key(): " << DebugDumpKeyToStr(*key) << ", doc_ht: " << doc_ht;
    } else {
      LOG(INFO) << "key(): fetch failed: " << key.status();
    }
  }
  LOG(INFO) << "<< IntentAwareIterator dump";
}

Status IntentAwareIterator::FindLastWriteTime(
    const Slice& key_without_ht,
    DocHybridTime* max_deleted_ts,
    Value* result_value) {
  DCHECK_ONLY_NOTNULL(max_deleted_ts);
  VLOG(4) << "FindLastWriteTime(" << SubDocKey::DebugSliceToString(key_without_ht) << ", "
          << *max_deleted_ts << ")";

  DOCDB_DEBUG_SCOPE_LOG(
      SubDocKey::DebugSliceToString(key_without_ht) + ", " + yb::ToString(max_deleted_ts) + ", "
      + yb::ToString(result_value),
      std::bind(&IntentAwareIterator::DebugDump, this));
  DCHECK(!DebugHasHybridTime(key_without_ht));

  RETURN_NOT_OK(status_);

  bool found_later_intent_result = false;
  if (intent_iter_) {
    const auto intent_prefix = GetIntentPrefixForKeyWithoutHt(key_without_ht);
    SeekForwardToSuitableIntent(intent_prefix);
    RETURN_NOT_OK(status_);
    if (resolved_intent_state_ == ResolvedIntentState::kValid &&
        resolved_intent_txn_dht_ > *max_deleted_ts &&
        resolved_intent_key_prefix_.CompareTo(intent_prefix) == 0) {
      *max_deleted_ts = resolved_intent_txn_dht_;
      VLOG(4) << "Max deleted time for " << key_without_ht.ToDebugHexString() << ": "
              << *max_deleted_ts;
      max_seen_ht_.MakeAtLeast(max_deleted_ts->hybrid_time());
      found_later_intent_result = true;
    }
  }

  seek_key_buffer_.Reserve(key_without_ht.size() + encoded_read_time_global_limit_.size() + 1);
  seek_key_buffer_.Reset(key_without_ht);
  AppendEncodedDocHt(encoded_read_time_global_limit_, &seek_key_buffer_);
  SeekForwardRegular(seek_key_buffer_);
  RETURN_NOT_OK(status_);

  // After SeekForwardRegular(), we need to call valid() to skip future records and see if the
  // current key still matches the pushed prefix if any. If it does not, we are done.
  if (!valid()) {
    return Status::OK();
  }

  DocHybridTime doc_ht;
  bool found_later_regular_result = false;

  if (iter_valid_) {
    int other_encoded_ht_size = 0;
    RETURN_NOT_OK(CheckHybridTimeSizeAndValueType(iter_->key(), &other_encoded_ht_size));
    if (key_without_ht.size() + 1 + other_encoded_ht_size == iter_->key().size() &&
        iter_->key().starts_with(key_without_ht)) {
      RETURN_NOT_OK(DecodeHybridTimeFromEndOfKey(iter_->key(), &doc_ht));
      if (doc_ht > *max_deleted_ts) {
        *max_deleted_ts = doc_ht;
        VLOG(4) << "Max deleted time for " << key_without_ht.ToDebugHexString() << ": "
                << *max_deleted_ts;
        max_seen_ht_.MakeAtLeast(doc_ht.hybrid_time());
        found_later_regular_result = true;
      }
      // TODO when we support TTL on non-leaf nodes, we need to take that into account here.
    }
  }

  if (result_value) {
    if (found_later_regular_result) {
      RETURN_NOT_OK(result_value->Decode(iter_->value()));
    } else if (found_later_intent_result) {
      RETURN_NOT_OK(result_value->Decode(resolved_intent_value_));
    }
  }

  return Status::OK();
}

void IntentAwareIterator::PushPrefix(const Slice& prefix) {
  VLOG(4) << "PushPrefix: " << SubDocKey::DebugSliceToString(prefix);
  prefix_stack_.push_back(prefix);
  skip_future_records_needed_ = true;
  skip_future_intents_needed_ = true;
}

void IntentAwareIterator::PopPrefix() {
  prefix_stack_.pop_back();
  skip_future_records_needed_ = true;
  skip_future_intents_needed_ = true;
  VLOG(4) << "PopPrefix: "
          << (prefix_stack_.empty() ? std::string()
              : SubDocKey::DebugSliceToString(prefix_stack_.back()));
}

void IntentAwareIterator::SkipFutureRecords() {
  if (!status_.ok()) {
    return;
  }
  auto prefix = prefix_stack_.empty() ? Slice() : prefix_stack_.back();
  while (iter_->Valid()) {
    if (!iter_->key().starts_with(prefix)) {
      VLOG(4) << "Unmatched prefix: " << SubDocKey::DebugSliceToString(iter_->key())
              << ", prefix: " << SubDocKey::DebugSliceToString(prefix);
      iter_valid_ = false;
      return;
    }
    Slice encoded_doc_ht = iter_->key();
    int doc_ht_size = 0;
    auto decode_status = DocHybridTime::CheckAndGetEncodedSize(encoded_doc_ht, &doc_ht_size);
    if (!decode_status.ok()) {
      LOG(ERROR) << "Decode doc ht from key failed: " << decode_status
                 << ", key: " << iter_->key().ToDebugHexString();
      status_ = std::move(decode_status);
      return;
    }
    encoded_doc_ht.remove_prefix(encoded_doc_ht.size() - doc_ht_size);
    auto value = iter_->value();
    auto value_type = DecodeValueType(value);
    if (value_type == ValueType::kHybridTime) {
      // Value came from a transaction, we could try to filter it by original intent time.
      Slice encoded_intent_doc_ht = value;
      encoded_intent_doc_ht.consume_byte();
      if (encoded_intent_doc_ht.compare(Slice(encoded_read_time_local_limit_)) > 0 &&
          encoded_doc_ht.compare(Slice(encoded_read_time_global_limit_)) > 0) {
        iter_valid_ = true;
        return;
      }
    } else if (encoded_doc_ht.compare(Slice(encoded_read_time_local_limit_)) > 0) {
      iter_valid_ = true;
      return;
    }
    VLOG(4) << "Skipping because of time: " << SubDocKey::DebugSliceToString(iter_->key());
    iter_->Next(); // TODO(dtxn) use seek with the same key, but read limit as doc hybrid time.
  }
  iter_valid_ = false;
}

void IntentAwareIterator::SkipFutureIntents() {
  if (!intent_iter_ || !status_.ok()) {
    return;
  }
  auto prefix = prefix_stack_.empty() ? Slice() : prefix_stack_.back();
  if (resolved_intent_state_ != ResolvedIntentState::kNoIntent) {
    VLOG(4) << "Checking resolved intent subdockey: "
            << resolved_intent_sub_doc_key_.ToDebugHexString()
            << ", against new prefix: " << prefix.ToDebugHexString();
    auto compare_result = resolved_intent_sub_doc_key_.compare_prefix(prefix);
    if (compare_result == 0) {
      resolved_intent_state_ = ResolvedIntentState::kValid;
      return;
    } else if (compare_result > 0) {
      resolved_intent_state_ = ResolvedIntentState::kInvalidPrefix;
      return;
    }
  }
  SeekForwardToSuitableIntent();
}

Status IntentAwareIterator::SetIntentUpperbound() {
  intent_upperbound_keybytes_.Clear();
  intent_upperbound_keybytes_.AppendValueType(ValueType::kIntentPrefix);
  if (iter_->Valid()) {
    // Strip ValueType::kHybridTime + DocHybridTime at the end of SubDocKey in iter_ and append
    // to upperbound with 0xff.
    Slice subdoc_key = iter_->key();
    int doc_ht_size = 0;
    RETURN_NOT_OK(DocHybridTime::CheckAndGetEncodedSize(subdoc_key, &doc_ht_size));
    subdoc_key.remove_suffix(1 + doc_ht_size);
    intent_upperbound_keybytes_.AppendRawBytes(subdoc_key);
    intent_upperbound_keybytes_.AppendValueType(ValueType::kMaxByte);
  } else {
    // In case the current position of the regular iterator is invalid, set the exclusive
    // upperbound to the beginning of the transaction metadata region.
    intent_upperbound_keybytes_.AppendValueType(ValueType::kTransactionId);
  }
  intent_upperbound_ = intent_upperbound_keybytes_.AsSlice();
  VLOG(4) << "SetIntentUpperbound = " << intent_upperbound_.ToDebugString();
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
