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

#include "yb/tablet/transaction_loader.h"

#include "yb/docdb/bounded_rocksdb_iterator.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/docdb_rocksdb_util.h"

#include "yb/tablet/transaction_status_resolver.h"

#include "yb/util/bitmap.h"
#include "yb/util/flag_tags.h"
#include "yb/util/operation_counter.h"
#include "yb/util/pb_util.h"
#include "yb/util/scope_exit.h"

using namespace std::literals;

DEFINE_test_flag(int32, inject_load_transaction_delay_ms, 0,
                 "Inject delay before loading each transaction at startup.");

DECLARE_bool(TEST_fail_on_replicated_batch_idx_set_in_txn_record);

METRIC_DEFINE_simple_counter(
    tablet, transaction_load_attempts,
    "Total number of attempts to load transaction metadata from the intents RocksDB",
    yb::MetricUnit::kTransactions);

namespace yb {
namespace tablet {

namespace {

docdb::BoundedRocksDbIterator CreateFullScanIterator(rocksdb::DB* db) {
  return docdb::BoundedRocksDbIterator(docdb::CreateRocksDBIterator(
      db, &docdb::KeyBounds::kNoBounds,
      docdb::BloomFilterMode::DONT_USE_BLOOM_FILTER,
      /* user_key_for_filter= */ boost::none, rocksdb::kDefaultQueryId));
}

} // namespace

class TransactionLoader::Executor {
 public:
  explicit Executor(
      TransactionLoader* loader,
      RWOperationCounter* pending_op_counter)
      : loader_(*loader), scoped_pending_operation_(pending_op_counter) {
    metric_transaction_load_attempts_ = METRIC_transaction_load_attempts.Instantiate(
        loader_.entity_);
  }

  bool Start(const docdb::DocDB& db) {
    if (!scoped_pending_operation_.ok()) {
      return false;
    }
    regular_iterator_ = CreateFullScanIterator(db.regular);
    intents_iterator_ = CreateFullScanIterator(db.intents);
    auto& load_thread = loader_.load_thread_;
    load_thread = std::thread(&Executor::Execute, this);
    return true;
  }

 private:
  void Execute() {
    CDSAttacher attacher;

    SetThreadName("TransactionLoader");

    auto se = ScopeExit([this] {
      auto pending_applies = std::move(pending_applies_);
      TransactionLoaderContext& context = loader_.context_;
      loader_.executor_.reset();

      context.LoadFinished(pending_applies);
    });

    LOG_WITH_PREFIX(INFO) << "Load transactions start";

    LoadPendingApplies();
    LoadTransactions();
  }

  void LoadTransactions() {
    size_t loaded_transactions = 0;
    TransactionId id = TransactionId::Nil();
    AppendTransactionKeyPrefix(id, &current_key_);
    intents_iterator_.Seek(current_key_.AsSlice());
    while (intents_iterator_.Valid()) {
      auto key = intents_iterator_.key();
      if (!key.TryConsumeByte(docdb::ValueTypeAsChar::kTransactionId)) {
        break;
      }
      auto decode_id_result = DecodeTransactionId(&key);
      if (!decode_id_result.ok()) {
        LOG_WITH_PREFIX(DFATAL)
            << "Failed to decode transaction id from: " << key.ToDebugHexString();
        intents_iterator_.Next();
        continue;
      }
      id = *decode_id_result;
      current_key_.Clear();
      AppendTransactionKeyPrefix(id, &current_key_);
      if (key.empty()) { // The key only contains a transaction id - it is metadata record.
        if (FLAGS_TEST_inject_load_transaction_delay_ms > 0) {
          std::this_thread::sleep_for(FLAGS_TEST_inject_load_transaction_delay_ms * 1ms);
        }
        LoadTransaction(id);
        ++loaded_transactions;
      }
      current_key_.AppendValueType(docdb::ValueType::kMaxByte);
      intents_iterator_.Seek(current_key_.AsSlice());
    }

    intents_iterator_.Reset();

    context().CompleteLoad([this] {
      loader_.all_loaded_.store(true, std::memory_order_release);
    });
    loader_.load_cond_.notify_all();
    LOG_WITH_PREFIX(INFO) << __func__ << " done: loaded " << loaded_transactions << " transactions";
  }

  void LoadPendingApplies() {
    std::array<char, 1 + sizeof(TransactionId) + 1> seek_buffer;
    seek_buffer[0] = docdb::ValueTypeAsChar::kTransactionApplyState;
    seek_buffer[seek_buffer.size() - 1] = docdb::ValueTypeAsChar::kMaxByte;
    regular_iterator_.Seek(Slice(seek_buffer.data(), 1));

    while (regular_iterator_.Valid()) {
      auto key = regular_iterator_.key();
      if (!key.TryConsumeByte(docdb::ValueTypeAsChar::kTransactionApplyState)) {
        break;
      }
      auto txn_id = DecodeTransactionId(&key);
      if (!txn_id.ok() || !key.TryConsumeByte(docdb::ValueTypeAsChar::kGroupEnd)) {
        LOG_WITH_PREFIX(DFATAL) << "Wrong txn id: " << regular_iterator_.key().ToDebugString();
        regular_iterator_.Next();
        continue;
      }
      Slice value = regular_iterator_.value();
      if (value.TryConsumeByte(docdb::ValueTypeAsChar::kString)) {
        auto pb = pb_util::ParseFromSlice<docdb::ApplyTransactionStatePB>(value);
        if (!pb.ok()) {
          LOG_WITH_PREFIX(DFATAL) << "Failed to decode apply state " << key.ToDebugString() << ": "
                                  << pb.status();
          regular_iterator_.Next();
          continue;
        }

        auto it = pending_applies_.emplace(*txn_id, ApplyStateWithCommitHt {
          .state = docdb::ApplyTransactionState::FromPB(*pb),
          .commit_ht = HybridTime(pb->commit_ht())
        }).first;

        VLOG_WITH_PREFIX(4) << "Loaded pending apply for " << *txn_id << ": "
                            << it->second.ToString();
      } else if (value.TryConsumeByte(docdb::ValueTypeAsChar::kTombstone)) {
        VLOG_WITH_PREFIX(4) << "Found deleted large apply for " << *txn_id;
      } else {
        LOG_WITH_PREFIX(DFATAL)
            << "Unexpected value type in apply state: " << value.ToDebugString();
      }

      memcpy(seek_buffer.data() + 1, txn_id->data(), txn_id->size());
      ROCKSDB_SEEK(&regular_iterator_, Slice(seek_buffer));
    }
  }

  // id - transaction id to load.
  void LoadTransaction(const TransactionId& id) {
    metric_transaction_load_attempts_->Increment();
    VLOG_WITH_PREFIX(1) << "Loading transaction: " << id;

    TransactionMetadataPB metadata_pb;

    const Slice& value = intents_iterator_.value();
    if (!metadata_pb.ParseFromArray(value.cdata(), value.size())) {
      LOG_WITH_PREFIX(DFATAL) << "Unable to parse stored metadata: "
                              << value.ToDebugHexString();
      return;
    }

    auto metadata = TransactionMetadata::FromPB(metadata_pb);
    if (!metadata.ok()) {
      LOG_WITH_PREFIX(DFATAL) << "Loaded bad metadata: " << metadata.status();
      return;
    }

    if (!metadata->start_time.is_valid()) {
      metadata->start_time = HybridTime::kMin;
      LOG_WITH_PREFIX(INFO) << "Patched start time " << metadata->transaction_id << ": "
                            << metadata->start_time;
    }

    TransactionalBatchData last_batch_data;
    OneWayBitmap replicated_batches;
    FetchLastBatchData(id, &last_batch_data, &replicated_batches);

    if (!status_resolver_) {
      status_resolver_ = &context().AddStatusResolver();
    }
    status_resolver_->Add(metadata->status_tablet, id);

    auto pending_apply_it = pending_applies_.find(id);
    context().LoadTransaction(
        std::move(*metadata), std::move(last_batch_data), std::move(replicated_batches),
        pending_apply_it != pending_applies_.end() ? &pending_apply_it->second : nullptr);
    {
      std::lock_guard<std::mutex> lock(loader_.mutex_);
      loader_.last_loaded_ = id;
    }
    loader_.load_cond_.notify_all();
  }

  void FetchLastBatchData(
      const TransactionId& id,
      TransactionalBatchData* last_batch_data,
      OneWayBitmap* replicated_batches) {
    current_key_.AppendValueType(docdb::ValueType::kMaxByte);
    intents_iterator_.Seek(current_key_.AsSlice());
    if (intents_iterator_.Valid()) {
      intents_iterator_.Prev();
    } else {
      intents_iterator_.SeekToLast();
    }
    current_key_.RemoveLastByte();
    while (intents_iterator_.Valid() && intents_iterator_.key().starts_with(current_key_)) {
      auto decoded_key = docdb::DecodeIntentKey(intents_iterator_.value());
      LOG_IF_WITH_PREFIX(DFATAL, !decoded_key.ok())
          << "Failed to decode intent while loading transaction " << id << ", "
          << intents_iterator_.key().ToDebugHexString() << " => "
          << intents_iterator_.value().ToDebugHexString() << ": " << decoded_key.status();
      if (decoded_key.ok() && docdb::HasStrong(decoded_key->intent_types)) {
        last_batch_data->hybrid_time = decoded_key->doc_ht.hybrid_time();
        Slice rev_key_slice(intents_iterator_.value());
        // Required by the transaction sealing protocol.
        if (!rev_key_slice.empty() && rev_key_slice[0] == docdb::ValueTypeAsChar::kBitSet) {
          CHECK(!FLAGS_TEST_fail_on_replicated_batch_idx_set_in_txn_record);
          rev_key_slice.remove_prefix(1);
          auto result = OneWayBitmap::Decode(&rev_key_slice);
          if (result.ok()) {
            *replicated_batches = std::move(*result);
            VLOG_WITH_PREFIX(1) << "Decoded replicated batches for " << id << ": "
                                << replicated_batches->ToString();
          } else {
            LOG_WITH_PREFIX(DFATAL)
                << "Failed to decode replicated batches from "
                << intents_iterator_.value().ToDebugHexString() << ": " << result.status();
          }
        }
        std::string rev_key = rev_key_slice.ToBuffer();
        intents_iterator_.Seek(rev_key);
        // Delete could run in parallel to this load, and since our deletes break snapshot read
        // we could get into a situation when metadata and reverse record were successfully read,
        // but intent record could not be found.
        if (intents_iterator_.Valid() && intents_iterator_.key().starts_with(rev_key)) {
          VLOG_WITH_PREFIX(1)
              << "Found latest record for " << id
              << ": " << docdb::SubDocKey::DebugSliceToString(intents_iterator_.key())
              << " => " << intents_iterator_.value().ToDebugHexString();
          auto status = docdb::DecodeIntentValue(
              intents_iterator_.value(), id.AsSlice(), &last_batch_data->next_write_id,
              nullptr /* body */);
          LOG_IF_WITH_PREFIX(DFATAL, !status.ok())
              << "Failed to decode intent value: " << status << ", "
              << docdb::SubDocKey::DebugSliceToString(intents_iterator_.key()) << " => "
              << intents_iterator_.value().ToDebugHexString();
          ++last_batch_data->next_write_id;
        }
        break;
      }
      intents_iterator_.Prev();
    }
  }

  TransactionLoaderContext& context() const {
    return loader_.context_;
  }

  const std::string& LogPrefix() const {
    return context().LogPrefix();
  }

  TransactionLoader& loader_;
  ScopedRWOperation scoped_pending_operation_;

  docdb::BoundedRocksDbIterator regular_iterator_;
  docdb::BoundedRocksDbIterator intents_iterator_;

  // Buffer that contains key of current record, i.e. value type + transaction id.
  docdb::KeyBytes current_key_;

  TransactionStatusResolver* status_resolver_ = nullptr;

  ApplyStatesMap pending_applies_;

  scoped_refptr<Counter> metric_transaction_load_attempts_;
};

TransactionLoader::TransactionLoader(
    TransactionLoaderContext* context, const scoped_refptr<MetricEntity>& entity)
    : context_(*context), entity_(entity) {}

TransactionLoader::~TransactionLoader() {
}

void TransactionLoader::Start(RWOperationCounter* pending_op_counter, const docdb::DocDB& db) {
  executor_ = std::make_unique<Executor>(this, pending_op_counter);
  if (!executor_->Start(db)) {
    executor_ = nullptr;
  }
}

void TransactionLoader::WaitLoaded(const TransactionId& id) NO_THREAD_SAFETY_ANALYSIS {
  if (all_loaded_.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  while (!all_loaded_.load(std::memory_order_acquire)) {
    if (last_loaded_ >= id) {
      break;
    }
    load_cond_.wait(lock);
  }
}

// Disable thread safety analysis because std::unique_lock is used.
void TransactionLoader::WaitAllLoaded() NO_THREAD_SAFETY_ANALYSIS {
  if (all_loaded_.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  load_cond_.wait(lock, [this] {
    return all_loaded_.load(std::memory_order_acquire);
  });
}

void TransactionLoader::Shutdown() {
  if (load_thread_.joinable()) {
    load_thread_.join();
  }
}

} // namespace tablet
} // namespace yb
