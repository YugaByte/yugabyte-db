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

#include <iostream>

#include "yb/rocksdb/util/statistics.h"

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_util.h"

#include "yb/rocksutil/write_batch_formatter.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/tablet/tablet_options.h"
#include "yb/util/env.h"
#include "yb/util/path_util.h"
#include "yb/util/string_trim.h"

using std::string;
using std::make_shared;
using std::endl;
using strings::Substitute;
using yb::FormatBytesAsStr;
using yb::util::ApplyEagerLineContinuation;
using std::vector;

namespace yb {
namespace docdb {

rocksdb::DB* DocDBRocksDBUtil::rocksdb() {
  return DCHECK_NOTNULL(regular_db_.get());
}

rocksdb::DB* DocDBRocksDBUtil::intents_db() {
  return DCHECK_NOTNULL(intents_db_.get());
}

std::string DocDBRocksDBUtil::IntentsDBDir() {
  return rocksdb_dir_ + ".intents";
}

Status DocDBRocksDBUtil::OpenRocksDB() {
  // Init the directory if needed.
  if (rocksdb_dir_.empty()) {
    RETURN_NOT_OK(InitRocksDBDir());
  }

  rocksdb::DB* rocksdb = nullptr;
  RETURN_NOT_OK(rocksdb::DB::Open(regular_db_options_, rocksdb_dir_, &rocksdb));
  LOG(INFO) << "Opened RocksDB at " << rocksdb_dir_;
  regular_db_.reset(rocksdb);

  rocksdb = nullptr;
  RETURN_NOT_OK(rocksdb::DB::Open(intents_db_options_, IntentsDBDir(), &rocksdb));
  intents_db_.reset(rocksdb);

  return Status::OK();
}

void DocDBRocksDBUtil::CloseRocksDB() {
  intents_db_.reset();
  regular_db_.reset();
}

Status DocDBRocksDBUtil::ReopenRocksDB() {
  CloseRocksDB();
  return OpenRocksDB();
}

Status DocDBRocksDBUtil::DestroyRocksDB() {
  CloseRocksDB();
  LOG(INFO) << "Destroying RocksDB database at " << rocksdb_dir_;
  RETURN_NOT_OK(rocksdb::DestroyDB(rocksdb_dir_, regular_db_options_));
  RETURN_NOT_OK(rocksdb::DestroyDB(IntentsDBDir(), intents_db_options_));
  return Status::OK();
}

void DocDBRocksDBUtil::ResetMonotonicCounter() {
  monotonic_counter_.store(0);
}

Status DocDBRocksDBUtil::PopulateRocksDBWriteBatch(
    const DocWriteBatch& dwb,
    rocksdb::WriteBatch *rocksdb_write_batch,
    HybridTime hybrid_time,
    bool decode_dockey,
    bool increment_write_id,
    PartialRangeKeyIntents partial_range_key_intents) const {
  if (decode_dockey) {
    for (const auto& entry : dwb.key_value_pairs()) {
      // Skip key validation for external intents.
      if (!entry.first.empty() && entry.first[0] == ValueTypeAsChar::kExternalTransactionId) {
        continue;
      }
      SubDocKey subdoc_key;
      // We don't expect any invalid encoded keys in the write batch. However, these encoded keys
      // don't contain the HybridTime.
      RETURN_NOT_OK_PREPEND(subdoc_key.FullyDecodeFromKeyWithOptionalHybridTime(entry.first),
          Substitute("when decoding key: $0", FormatBytesAsStr(entry.first)));
    }
  }

  if (current_txn_id_.is_initialized()) {
    if (!increment_write_id) {
      return STATUS(
          InternalError, "For transactional write only increment_write_id=true is supported");
    }
    KeyValueWriteBatchPB kv_write_batch;
    dwb.TEST_CopyToWriteBatchPB(&kv_write_batch);
    PrepareTransactionWriteBatch(
        kv_write_batch, hybrid_time, rocksdb_write_batch, *current_txn_id_, txn_isolation_level_,
        partial_range_key_intents, /* replicated_batches_state= */ Slice(), &intra_txn_write_id_);
  } else {
    // TODO: this block has common code with docdb::PrepareNonTransactionWriteBatch and probably
    // can be refactored, so common code is reused.
    IntraTxnWriteId write_id = 0;
    for (const auto& entry : dwb.key_value_pairs()) {
      string rocksdb_key;
      if (hybrid_time.is_valid()) {
        // HybridTime provided. Append a PrimitiveValue with the HybridTime to the key.
        const KeyBytes encoded_ht =
            PrimitiveValue(DocHybridTime(hybrid_time, write_id)).ToKeyBytes();
        rocksdb_key = entry.first + encoded_ht.ToStringBuffer();
      } else {
        // Useful when printing out a write batch that does not yet know the HybridTime it will be
        // committed with.
        rocksdb_key = entry.first;
      }
      rocksdb_write_batch->Put(rocksdb_key, entry.second);
      if (increment_write_id) {
        ++write_id;
      }
    }
  }
  return Status::OK();
}

Status DocDBRocksDBUtil::WriteToRocksDB(
    const DocWriteBatch& doc_write_batch,
    const HybridTime& hybrid_time,
    bool decode_dockey,
    bool increment_write_id,
    PartialRangeKeyIntents partial_range_key_intents) {
  if (doc_write_batch.IsEmpty()) {
    return Status::OK();
  }
  if (!hybrid_time.is_valid()) {
    return STATUS_SUBSTITUTE(InvalidArgument, "Hybrid time is not valid: $0",
                             hybrid_time.ToString());
  }

  ConsensusFrontiers frontiers;
  rocksdb::WriteBatch rocksdb_write_batch;
  if (!op_id_.empty()) {
    ++op_id_.index;
    set_op_id(op_id_, &frontiers);
    set_hybrid_time(hybrid_time, &frontiers);
    rocksdb_write_batch.SetFrontiers(&frontiers);
  }

  RETURN_NOT_OK(PopulateRocksDBWriteBatch(
      doc_write_batch, &rocksdb_write_batch, hybrid_time, decode_dockey, increment_write_id,
      partial_range_key_intents));

  rocksdb::DB* db = current_txn_id_ ? intents_db_.get() : regular_db_.get();
  rocksdb::Status rocksdb_write_status = db->Write(write_options(), &rocksdb_write_batch);

  if (!rocksdb_write_status.ok()) {
    LOG(ERROR) << "Failed writing to RocksDB: " << rocksdb_write_status.ToString();
    return STATUS_SUBSTITUTE(RuntimeError,
                             "Error writing to RocksDB: $0", rocksdb_write_status.ToString());
  }
  return Status::OK();
}

Status DocDBRocksDBUtil::InitCommonRocksDBOptions() {
  // TODO(bojanserafimov): create MemoryMonitor?
  const size_t cache_size = block_cache_size();
  if (cache_size > 0) {
    block_cache_ = rocksdb::NewLRUCache(cache_size);
  }

  regular_db_options_.statistics = rocksdb::CreateDBStatistics();
  intents_db_options_.statistics = rocksdb::CreateDBStatistics();
  RETURN_NOT_OK(ReinitDBOptions());
  InitRocksDBWriteOptions(&write_options_);
  return Status::OK();
}

Status DocDBRocksDBUtil::WriteToRocksDBAndClear(
    DocWriteBatch* dwb,
    const HybridTime& hybrid_time,
    bool decode_dockey, bool increment_write_id) {
  RETURN_NOT_OK(WriteToRocksDB(*dwb, hybrid_time, decode_dockey, increment_write_id));
  dwb->Clear();
  return Status::OK();
}

Status DocDBRocksDBUtil::WriteSimple(int index) {
  auto encoded_doc_key = DocKey(PrimitiveValues(Format("row$0", index), 11111 * index)).Encode();
  op_id_.term = index / 2;
  op_id_.index = index;
  auto& dwb = DefaultDocWriteBatch();
  RETURN_NOT_OK(dwb.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue(ColumnId(10))), PrimitiveValue(index)));
  return WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(1000 * index));
}

void DocDBRocksDBUtil::SetHistoryCutoffHybridTime(HybridTime history_cutoff) {
  retention_policy_->SetHistoryCutoff(history_cutoff);
}

void DocDBRocksDBUtil::SetTableTTL(uint64_t ttl_msec) {
  schema_.SetDefaultTimeToLive(ttl_msec);
  retention_policy_->SetTableTTLForTests(MonoDelta::FromMilliseconds(ttl_msec));
}

string DocDBRocksDBUtil::DocDBDebugDumpToStr() {
  return yb::docdb::DocDBDebugDumpToStr(rocksdb()) +
         yb::docdb::DocDBDebugDumpToStr(intents_db(), StorageDbType::kIntents);
}

Status DocDBRocksDBUtil::SetPrimitive(
    const DocPath& doc_path,
    const Value& value,
    const HybridTime hybrid_time,
    const ReadHybridTime& read_ht) {
  auto dwb = MakeDocWriteBatch();
  RETURN_NOT_OK(dwb.SetPrimitive(doc_path, value, read_ht));
  return WriteToRocksDB(dwb, hybrid_time);
}

Status DocDBRocksDBUtil::SetPrimitive(
    const DocPath& doc_path,
    const PrimitiveValue& primitive_value,
    const HybridTime hybrid_time,
    const ReadHybridTime& read_ht) {
  return SetPrimitive(doc_path, Value(primitive_value), hybrid_time, read_ht);
}

Status DocDBRocksDBUtil::AddExternalIntents(
    const TransactionId& txn_id,
    const std::vector<ExternalIntent>& intents,
    const Uuid& involved_tablet,
    HybridTime hybrid_time) {
  class Provider : public ExternalIntentsProvider {
   public:
    Provider(
        const std::vector<ExternalIntent>* intents, const Uuid& involved_tablet,
        HybridTime hybrid_time)
        : intents_(*intents), involved_tablet_(involved_tablet), hybrid_time_(hybrid_time) {}

    void SetKey(const Slice& slice) override {
      key_.AppendRawBytes(slice);
    }

    void SetValue(const Slice& slice) override {
      value_ = slice;
    }

    void Apply(rocksdb::WriteBatch* batch) {
      KeyValuePairPB kv_pair;
      kv_pair.set_key(key_.ToStringBuffer());
      kv_pair.set_value(value_.ToString());
      ExternalTxnApplyState external_txn_apply_state;
      AddPairToWriteBatch(kv_pair, hybrid_time_, 0, &external_txn_apply_state, nullptr, batch);
    }

    boost::optional<std::pair<Slice, Slice>> Next() override {
      if (next_idx_ >= intents_.size()) {
        return boost::none;
      }

      // It is ok to have inefficient code in tests.
      const auto& intent = intents_[next_idx_];
      ++next_idx_;

      intent_key_ = intent.doc_path.encoded_doc_key();
      for (const auto& subkey : intent.doc_path.subkeys()) {
        subkey.AppendToKey(&intent_key_);
      }
      intent_value_ = intent.value.Encode();

      return std::pair<Slice, Slice>(intent_key_.AsSlice(), intent_value_);
    }

    const Uuid& InvolvedTablet() override {
      return involved_tablet_;
    }

   private:
    const std::vector<ExternalIntent>& intents_;
    const Uuid involved_tablet_;
    const HybridTime hybrid_time_;
    size_t next_idx_ = 0;
    KeyBytes key_;
    KeyBuffer value_;

    KeyBytes intent_key_;
    std::string intent_value_;
  };

  Provider provider(&intents, involved_tablet, hybrid_time);
  CombineExternalIntents(txn_id, &provider);

  rocksdb::WriteBatch rocksdb_write_batch;
  provider.Apply(&rocksdb_write_batch);

  return intents_db_->Write(write_options(), &rocksdb_write_batch);
}

Status DocDBRocksDBUtil::InsertSubDocument(
    const DocPath& doc_path,
    const SubDocument& value,
    const HybridTime hybrid_time,
    MonoDelta ttl,
    const ReadHybridTime& read_ht) {
  auto dwb = MakeDocWriteBatch();
  RETURN_NOT_OK(dwb.InsertSubDocument(doc_path, value, read_ht,
                                      CoarseTimePoint::max(), rocksdb::kDefaultQueryId, ttl));
  return WriteToRocksDB(dwb, hybrid_time);
}

Status DocDBRocksDBUtil::ExtendSubDocument(
    const DocPath& doc_path,
    const SubDocument& value,
    const HybridTime hybrid_time,
    MonoDelta ttl,
    const ReadHybridTime& read_ht) {
  auto dwb = MakeDocWriteBatch();
  RETURN_NOT_OK(dwb.ExtendSubDocument(doc_path, value, read_ht,
                                      CoarseTimePoint::max(), rocksdb::kDefaultQueryId, ttl));
  return WriteToRocksDB(dwb, hybrid_time);
}

Status DocDBRocksDBUtil::ExtendList(
    const DocPath& doc_path,
    const SubDocument& value,
    HybridTime hybrid_time,
    const ReadHybridTime& read_ht) {
  auto dwb = MakeDocWriteBatch();
  RETURN_NOT_OK(dwb.ExtendList(doc_path, value, read_ht, CoarseTimePoint::max()));
  return WriteToRocksDB(dwb, hybrid_time);
}

Status DocDBRocksDBUtil::ReplaceInList(
    const DocPath &doc_path,
    const std::vector<int>& indexes,
    const std::vector<SubDocument>& values,
    const ReadHybridTime& read_ht,
    const HybridTime& hybrid_time,
    const rocksdb::QueryId query_id,
    MonoDelta default_ttl,
    MonoDelta ttl,
    UserTimeMicros user_timestamp) {
  auto dwb = MakeDocWriteBatch();
  RETURN_NOT_OK(dwb.ReplaceCqlInList(
      doc_path, indexes, values, read_ht, CoarseTimePoint::max(), query_id, default_ttl, ttl));
  return WriteToRocksDB(dwb, hybrid_time);
}

Status DocDBRocksDBUtil::DeleteSubDoc(
    const DocPath& doc_path,
    HybridTime hybrid_time,
    const ReadHybridTime& read_ht) {
  auto dwb = MakeDocWriteBatch();
  RETURN_NOT_OK(dwb.DeleteSubDoc(doc_path, read_ht));
  return WriteToRocksDB(dwb, hybrid_time);
}

void DocDBRocksDBUtil::DocDBDebugDumpToConsole() {
  DocDBDebugDump(regular_db_.get(), std::cerr, StorageDbType::kRegular);
}

Status DocDBRocksDBUtil::FlushRocksDbAndWait() {
  rocksdb::FlushOptions flush_options;
  flush_options.wait = true;
  return rocksdb()->Flush(flush_options);
}

Status DocDBRocksDBUtil::ReinitDBOptions() {
  tablet::TabletOptions tablet_options;
  tablet_options.block_cache = block_cache_;
  docdb::InitRocksDBOptions(
      &regular_db_options_, "[R] " /* log_prefix */, regular_db_options_.statistics,
      tablet_options);
  docdb::InitRocksDBOptions(
      &intents_db_options_, "[I] " /* log_prefix */, intents_db_options_.statistics,
      tablet_options);
  regular_db_options_.compaction_filter_factory =
      std::make_shared<docdb::DocDBCompactionFilterFactory>(
          retention_policy_, &KeyBounds::kNoBounds);
  if (!regular_db_) {
    return Status::OK();
  }
  return ReopenRocksDB();
}

DocWriteBatch DocDBRocksDBUtil::MakeDocWriteBatch() {
  return DocWriteBatch(
      DocDB::FromRegularUnbounded(regular_db_.get()), init_marker_behavior_, &monotonic_counter_);
}

DocWriteBatch DocDBRocksDBUtil::MakeDocWriteBatch(InitMarkerBehavior init_marker_behavior) {
  return DocWriteBatch(
      DocDB::FromRegularUnbounded(regular_db_.get()), init_marker_behavior, &monotonic_counter_);
}

DocWriteBatch& DocDBRocksDBUtil::DefaultDocWriteBatch() {
  if (!doc_write_batch_) {
    doc_write_batch_ = MakeDocWriteBatch();
  }

  return *doc_write_batch_;
}

void DocDBRocksDBUtil::SetInitMarkerBehavior(InitMarkerBehavior init_marker_behavior) {
  if (init_marker_behavior_ != init_marker_behavior) {
    LOG(INFO) << "Setting init marker behavior to " << init_marker_behavior;
    init_marker_behavior_ = init_marker_behavior;
  }
}

}  // namespace docdb
}  // namespace yb
