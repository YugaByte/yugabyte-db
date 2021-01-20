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

#include "yb/docdb/docdb_compaction_filter_intents.h"

#include <memory>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/rocksdb/compaction_filter.h"
#include "yb/util/string_util.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/value.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/thread_pool.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/util/flag_tags.h"

DEFINE_uint64(aborted_intent_cleanup_ms, 60000, // 1 minute by default, 1 sec for testing
             "Duration in ms after which to check if a transaction is aborted.");

DEFINE_int32(aborted_intent_cleanup_max_batch_size, 256, // Cleanup 256 transactions at a time
             "Number of transactions to collect for possible cleanup.");

DEFINE_int32(external_intent_cleanup_secs, 60 * 60 * 24, // 24 hours by default
             "Duration in secs after which to cleanup external intents.");

DEFINE_uint64(intents_compaction_filter_max_errors_to_log, 100,
              "Maximum number of errors to log for life cycle of the intents compcation filter.");

using std::shared_ptr;
using std::unique_ptr;
using std::unordered_set;
using rocksdb::CompactionFilter;
using rocksdb::VectorToString;

namespace yb {
namespace docdb {

// ------------------------------------------------------------------------------------------------

namespace {
class DocDBIntentsCompactionFilter : public rocksdb::CompactionFilter {
 public:
  explicit DocDBIntentsCompactionFilter(tablet::Tablet* tablet, const KeyBounds* key_bounds)
      : tablet_(tablet), compaction_start_time_(tablet->clock()->Now().GetPhysicalValueMicros()) {}

  ~DocDBIntentsCompactionFilter() override;

  rocksdb::FilterDecision Filter(
      int level, const Slice& key, const Slice& existing_value, std::string* new_value,
      bool* value_changed) override;

  const char* Name() const override;

  void CompactionFinished() override;

  TransactionIdSet& transactions_to_cleanup() {
    return transactions_to_cleanup_;
  }

  void AddToSet(const TransactionId& transaction_id, TransactionIdSet* set);

 private:
  void CleanupTransactions();

  std::string LogPrefix() const;

  Result<boost::optional<TransactionId>> FilterTransactionMetadata(
      const Slice& key, const Slice& existing_value);

  Result<rocksdb::FilterDecision> FilterExternalIntent(const Slice& key);

  tablet::Tablet* const tablet_;
  const MicrosTime compaction_start_time_;

  TransactionIdSet transactions_to_cleanup_;
  int rejected_transactions_ = 0;
  uint64_t num_errors_ = 0;

  // We use this to only log a message that the filter is being used once on the first call to
  // the Filter function.
  bool filter_usage_logged_ = false;
};

#define MAYBE_LOG_ERROR_AND_RETURN_KEEP(result) { \
  if (!result.ok()) { \
    if (num_errors_ < GetAtomicFlag(&FLAGS_intents_compaction_filter_max_errors_to_log)) { \
      LOG_WITH_PREFIX(ERROR) << StatusToString(result.status()); \
    } \
    num_errors_++; \
    return rocksdb::FilterDecision::kKeep; \
  } \
}

void DocDBIntentsCompactionFilter::CleanupTransactions() {
  VLOG_WITH_PREFIX(3) << "DocDB intents compaction filter is being deleted";
  if (transactions_to_cleanup_.empty()) {
    return;
  }
  TransactionStatusManager* manager = tablet_->transaction_participant();
  if (rejected_transactions_ > 0) {
    LOG_WITH_PREFIX(WARNING) << "Number of aborted transactions not cleaned up " <<
                                "on account of reaching size limits:" << rejected_transactions_;
  }
  manager->Cleanup(std::move(transactions_to_cleanup_));
}

DocDBIntentsCompactionFilter::~DocDBIntentsCompactionFilter() {
}

rocksdb::FilterDecision DocDBIntentsCompactionFilter::Filter(
    int level, const Slice& key, const Slice& existing_value, std::string* new_value,
    bool* value_changed) {
  if (!filter_usage_logged_) {
    VLOG_WITH_PREFIX(3) << "DocDB intents compaction filter is being used for a compaction";
    filter_usage_logged_ = true;
  }

  if (GetKeyType(key, StorageDbType::kIntents) == KeyType::kExternalIntents) {
    auto filter_decision_result = FilterExternalIntent(key);
    MAYBE_LOG_ERROR_AND_RETURN_KEEP(filter_decision_result);
    return *filter_decision_result;
  }

  // Find transaction metadata row.
  if (GetKeyType(key, StorageDbType::kIntents) == KeyType::kTransactionMetadata) {
    auto transaction_id_result = FilterTransactionMetadata(key, existing_value);
    MAYBE_LOG_ERROR_AND_RETURN_KEEP(transaction_id_result);
    auto transaction_id_optional = *transaction_id_result;
    if (transaction_id_optional.has_value()) {
      AddToSet(transaction_id_optional.value(), &transactions_to_cleanup_);
    }
  }

  // TODO(dtxn): If/when we add processing of reverse index or intents here - we will need to
  // respect key_bounds passed to constructor in order to ignore/delete non-relevant keys. As of
  // 06/19/2019, intents and reverse indexes are being deleted by docdb::PrepareApplyIntentsBatch.
  return rocksdb::FilterDecision::kKeep;
}

Result<boost::optional<TransactionId>> DocDBIntentsCompactionFilter::FilterTransactionMetadata(
    const Slice& key, const Slice& existing_value) {
  TransactionMetadataPB metadata_pb;
  if (!metadata_pb.ParseFromArray(existing_value.cdata(), existing_value.size())) {
    return STATUS(IllegalState, "Failed to parse transaction metadata");
  }
  uint64_t write_time = metadata_pb.metadata_write_time();
  if (!write_time) {
    write_time = HybridTime(metadata_pb.start_hybrid_time()).GetPhysicalValueMicros();
  }
  if (compaction_start_time_ < write_time + FLAGS_aborted_intent_cleanup_ms * 1000) {
    return boost::none;
  }
  Slice key_slice = key;
  return VERIFY_RESULT_PREPEND(
      DecodeTransactionIdFromIntentValue(&key_slice), "Could not decode Transaction metadata");
}

Result<rocksdb::FilterDecision>
DocDBIntentsCompactionFilter::FilterExternalIntent(const Slice& key) {
  Slice key_slice = key;
  RETURN_NOT_OK_PREPEND(key_slice.consume_byte(ValueTypeAsChar::kExternalTransactionId),
                        "Could not decode external transaction byte");
  // Ignoring transaction id result since function just returns kKeep or kDiscard.
  ignore_result(VERIFY_RESULT_PREPEND(
      DecodeTransactionId(&key_slice), "Could not decode external transaction id"));
  auto doc_hybrid_time = VERIFY_RESULT_PREPEND(
      DecodeInvertedDocHt(key_slice), "Could not decode hybrid time");
  auto write_time_micros = doc_hybrid_time.hybrid_time().GetPhysicalValueMicros();
  auto delta_micros = compaction_start_time_ - write_time_micros;
  if (delta_micros >
      GetAtomicFlag(&FLAGS_external_intent_cleanup_secs) * MonoTime::kMicrosecondsPerSecond) {
    return rocksdb::FilterDecision::kDiscard;
  }
  return rocksdb::FilterDecision::kKeep;
}

void DocDBIntentsCompactionFilter::CompactionFinished() {
  if (num_errors_ > 0) {
    LOG_WITH_PREFIX(WARNING) << Format(
        "Found $0 total errors during intents compaction filter.", num_errors_);
  }
  CleanupTransactions();
}

void DocDBIntentsCompactionFilter::AddToSet(const TransactionId& transaction_id,
                                            TransactionIdSet* set) {
  if (set->size() <= FLAGS_aborted_intent_cleanup_max_batch_size) {
    set->insert(transaction_id);
  } else {
    rejected_transactions_++;
  }
}

const char* DocDBIntentsCompactionFilter::Name() const {
  return "DocDBIntentsCompactionFilter";
}

std::string DocDBIntentsCompactionFilter::LogPrefix() const {
  return Format("T $0: ", tablet_->tablet_id());
}

} // namespace

// ------------------------------------------------------------------------------------------------

DocDBIntentsCompactionFilterFactory::DocDBIntentsCompactionFilterFactory(
    tablet::Tablet* tablet, const KeyBounds* key_bounds)
    : tablet_(tablet), key_bounds_(key_bounds) {}

DocDBIntentsCompactionFilterFactory::~DocDBIntentsCompactionFilterFactory() {}

std::unique_ptr<CompactionFilter> DocDBIntentsCompactionFilterFactory::CreateCompactionFilter(
    const CompactionFilter::Context& context) {
  return std::make_unique<DocDBIntentsCompactionFilter>(tablet_, key_bounds_);
}

const char* DocDBIntentsCompactionFilterFactory::Name() const {
  return "DocDBIntentsCompactionFilterFactory";
}

}  // namespace docdb
}  // namespace yb
