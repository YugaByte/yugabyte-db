// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
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
#ifndef YB_CONSENSUS_LOG_TEST_BASE_H
#define YB_CONSENSUS_LOG_TEST_BASE_H

#include <utility>
#include <vector>
#include <string>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/consensus/log.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_reader.h"
#include "yb/consensus/opid_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"
#include "yb/server/clock.h"
#include "yb/server/hybrid_clock.h"
#include "yb/server/metadata.h"
#include "yb/tablet/tablet.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/env_util.h"
#include "yb/util/metrics.h"
#include "yb/util/path_util.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/stopwatch.h"
#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/doc_key.h"

METRIC_DECLARE_entity(tablet);

DECLARE_int32(log_min_seconds_to_retain);

namespace yb {
namespace log {

using consensus::ReplicateMsg;
using consensus::WRITE_OP;
using consensus::NO_OP;
using consensus::MakeOpId;
using consensus::MakeOpIdPB;

using server::Clock;

using tserver::WriteRequestPB;

using tablet::Tablet;

using docdb::KeyValuePairPB;
using docdb::SubDocKey;
using docdb::DocKey;
using docdb::PrimitiveValue;
using docdb::ValueType;

const char* kTestNamespace = "test-ns";
const char* kTestTable = "test-log-table";
const char* kTestTablet = "test-log-tablet";

YB_STRONGLY_TYPED_BOOL(AppendSync);

// Append a single batch of 'count' NoOps to the log.  If 'size' is not NULL, increments it by the
// expected increase in log size.  Increments 'op_id''s index once for each operation logged.
static CHECKED_STATUS AppendNoOpsToLogSync(const scoped_refptr<Clock>& clock,
                                           Log* log, OpIdPB* op_id,
                                           int count,
                                           int* size = NULL) {
  ReplicateMsgs replicates;
  for (int i = 0; i < count; i++) {
    auto replicate = std::make_shared<ReplicateMsg>();
    ReplicateMsg* repl = replicate.get();

    repl->mutable_id()->CopyFrom(*op_id);
    repl->set_op_type(NO_OP);
    repl->set_hybrid_time(clock->Now().ToUint64());

    // Increment op_id.
    op_id->set_index(op_id->index() + 1);

    if (size) {
      // If we're tracking the sizes we need to account for the fact that the Log wraps the log
      // entry in an LogEntryBatchPB, and each actual entry will have a one-byte tag.
      *size += repl->ByteSize() + 1;
    }
    replicates.push_back(replicate);
  }

  // Account for the entry batch header and wrapper PB.
  if (size) {
    *size += log::kEntryHeaderSize + 7;
  }

  Synchronizer s;
  RETURN_NOT_OK(log->AsyncAppendReplicates(
      replicates, yb::OpId() /* committed_op_id */, RestartSafeCoarseTimePoint::FromUInt64(1),
      s.AsStatusCallback()));
  RETURN_NOT_OK(s.Wait());
  return Status::OK();
}

static CHECKED_STATUS AppendNoOpToLogSync(const scoped_refptr<Clock>& clock,
                                          Log* log, OpIdPB* op_id,
                                          int* size = nullptr) {
  return AppendNoOpsToLogSync(clock, log, op_id, 1, size);
}

class LogTestBase : public YBTest {
 public:

  typedef pair<int, int> DeltaId;

  typedef std::tuple<int, int, string> TupleForAppend;

  LogTestBase()
      : schema_({ ColumnSchema("key", INT32, false, true),
                  ColumnSchema("int_val", INT32),
                  ColumnSchema("string_val", STRING, true) },
                1),
        log_anchor_registry_(new LogAnchorRegistry()) {
  }

  virtual void SetUp() override {
    YBTest::SetUp();
    current_index_ = 1;
    fs_manager_.reset(new FsManager(env_.get(), GetTestPath("fs_root"), "tserver_test"));
    metric_registry_.reset(new MetricRegistry());
    metric_entity_ = METRIC_ENTITY_tablet.Instantiate(metric_registry_.get(), "log-test-base");
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
    tablet_wal_path_ = fs_manager_->GetFirstTabletWalDirOrDie(kTestTable, kTestTablet);
    clock_.reset(new server::HybridClock());
    ASSERT_OK(clock_->Init());
    FLAGS_log_min_seconds_to_retain = 0;
    ASSERT_OK(ThreadPoolBuilder("log")
                 .unlimited_threads()
                 .Build(&log_thread_pool_));
  }

  void CleanTablet() {
    ASSERT_OK(fs_manager_->DeleteFileSystemLayout(ShouldDeleteLogs::kTrue));
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
  }

  void BuildLog() {
    Schema schema_with_ids = SchemaBuilder(schema_).Build();
    ASSERT_OK(Log::Open(options_,
                       kTestTablet,
                       tablet_wal_path_,
                       fs_manager_->uuid(),
                       schema_with_ids,
                       0, // schema_version
                       metric_entity_.get(),
                       log_thread_pool_.get(),
                       log_thread_pool_.get(),
                       std::numeric_limits<int64_t>::max(), // cdc_min_replicated_index
                       &log_));
    LOG(INFO) << "Sucessfully opened the log at " << tablet_wal_path_;
  }

  void CheckRightNumberOfSegmentFiles(int expected) {
    // Test that we actually have the expected number of files in the fs. We should have n segments.
    const vector<string> files =
        ASSERT_RESULT(env_->GetChildren(tablet_wal_path_, ExcludeDots::kTrue));
    int count = 0;
    for (const string& s : files) {
      if (HasPrefixString(s, FsManager::kWalFileNamePrefix)) {
        count++;
      }
    }
    ASSERT_EQ(expected, count);
  }

  static void CheckReplicateResult(const consensus::ReplicateMsgPtr& msg, const Status& s) {
    ASSERT_OK(s);
  }

  struct AppendReplicateBatchData {
    yb::OpId op_id;
    yb::OpId committed_op_id;
    std::vector<TupleForAppend> writes;
    AppendSync sync = AppendSync::kTrue;
    consensus::OperationType op_type = consensus::OperationType::WRITE_OP;
    TransactionId txn_id = TransactionId::Nil();
    TransactionStatus txn_status = TransactionStatus::CLEANUP;
  };

  void AppendReplicateBatch(AppendReplicateBatchData data) {
    AppendReplicateBatch(
        MakeOpIdPB(data.op_id),
        MakeOpIdPB(data.committed_op_id),
        std::move(data.writes),
        data.sync,
        data.op_type,
        data.txn_id,
        data.txn_status);
  }

  // Appends a batch with size 2, or the given set of writes.
  void AppendReplicateBatch(
      const OpIdPB& opid,
      const OpIdPB& committed_opid = MakeOpId(0, 0),
      std::vector<TupleForAppend> writes = {},
      AppendSync sync = AppendSync::kTrue,
      consensus::OperationType op_type = consensus::OperationType::WRITE_OP,
      TransactionId txn_id = TransactionId::Nil(),
      TransactionStatus txn_status = TransactionStatus::APPLYING) {
    auto replicate = std::make_shared<ReplicateMsg>();
    replicate->set_op_type(op_type);
    replicate->mutable_id()->CopyFrom(opid);
    replicate->mutable_committed_op_id()->CopyFrom(committed_opid);
    replicate->set_hybrid_time(clock_->Now().ToUint64());
    WriteRequestPB *batch_request = replicate->mutable_write_request();

    if (op_type == consensus::OperationType::UPDATE_TRANSACTION_OP) {
      ASSERT_TRUE(!txn_id.IsNil());
      replicate->mutable_transaction_state()->set_status(txn_status);
    } else if (op_type == consensus::OperationType::WRITE_OP) {
      if (writes.empty()) {
        const int opid_index_as_int = static_cast<int>(opid.index());
        // Since OpIds deal with int64 index and term, we are downcasting here. In order to be able
        // to test with values > INT_MAX, we need to make sure we do not overflow, while still
        // wanting to add 2 different values here.
        //
        // Picking x and x / 2 + 1 as the 2 values.
        // For small numbers, special casing x <= 2.
        const int other_int = opid_index_as_int <= 2 ? 3 : opid_index_as_int / 2 + 1;
        writes.emplace_back(
            /* key */ opid_index_as_int, /* int_val */ 0, /* string_val */ "this is a test insert");
        writes.emplace_back(
            /* key */ other_int, /* int_val */ 0, /* string_val */ "this is a test mutate");
      }

      auto write_batch = batch_request->mutable_write_batch();
      if (!txn_id.IsNil()) {
        write_batch->mutable_transaction()->set_transaction_id(txn_id.data(), txn_id.size());
      }
      for (const auto &w : writes) {
        AddKVToPB(std::get<0>(w), std::get<1>(w), std::get<2>(w), write_batch);
      }
    } else {
      FAIL() << "Unexpected operation type: " << consensus::OperationType_Name(op_type);
    }

    batch_request->set_tablet_id(kTestTablet);
    AppendReplicateBatch(replicate, sync);
  }

  // Appends the provided batch to the log.
  void AppendReplicateBatch(const consensus::ReplicateMsgPtr& replicate,
                            AppendSync sync = AppendSync::kTrue) {
    const auto committed_op_id = yb::OpId::FromPB(replicate->committed_op_id());
    const auto batch_mono_time = restart_safe_coarse_mono_clock_.Now();
    if (sync) {
      Synchronizer s;
      ASSERT_OK(log_->AsyncAppendReplicates(
          { replicate }, committed_op_id, batch_mono_time, s.AsStatusCallback()));
      ASSERT_OK(s.Wait());
    } else {
      // AsyncAppendReplicates does not free the ReplicateMsg on completion, so we
      // need to pass it through to our callback.
      ASSERT_OK(log_->AsyncAppendReplicates(
          { replicate }, committed_op_id, batch_mono_time,
          Bind(&LogTestBase::CheckReplicateResult, replicate)));
    }
  }

  // Appends 'count' ReplicateMsgs to the log as committed entries.
  void AppendReplicateBatchToLog(int count, AppendSync sync = AppendSync::kTrue) {
    for (int i = 0; i < count; i++) {
      OpIdPB opid = consensus::MakeOpId(1, current_index_);
      AppendReplicateBatch(opid, opid, /* writes */ {}, sync);
      current_index_ += 1;
    }
  }

  // Append a single NO_OP entry. Increments op_id by one.  If non-NULL, and if the write is
  // successful, 'size' is incremented by the size of the written operation.
  CHECKED_STATUS AppendNoOp(OpIdPB* op_id, int* size = nullptr) {
    return AppendNoOpToLogSync(clock_, log_.get(), op_id, size);
  }

  // Append a number of no-op entries to the log.  Increments op_id's index by the number of records
  // written.  If non-NULL, 'size' keeps track of the size of the operations successfully written.
  CHECKED_STATUS AppendNoOps(OpIdPB* op_id, int num, int* size = nullptr) {
    for (int i = 0; i < num; i++) {
      RETURN_NOT_OK(AppendNoOp(op_id, size));
    }
    return Status::OK();
  }

  CHECKED_STATUS RollLog() {
    return log_->AllocateSegmentAndRollOver();
  }

  string DumpSegmentsToString(const SegmentSequence& segments) {
    string dump;
    for (const scoped_refptr<ReadableLogSegment>& segment : segments) {
      dump.append("------------\n");
      strings::SubstituteAndAppend(&dump, "Segment: $0, Path: $1\n",
                                   segment->header().sequence_number(), segment->path());
      strings::SubstituteAndAppend(&dump, "Header: $0\n",
                                   segment->header().ShortDebugString());
      if (segment->HasFooter()) {
        strings::SubstituteAndAppend(&dump, "Footer: $0\n", segment->footer().ShortDebugString());
      } else {
        dump.append("Footer: None or corrupt.");
      }
    }
    return dump;
  }

 protected:
  const Schema schema_;
  gscoped_ptr<FsManager> fs_manager_;
  gscoped_ptr<MetricRegistry> metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  std::unique_ptr<ThreadPool> log_thread_pool_;
  scoped_refptr<Log> log_;
  int64_t current_index_;
  LogOptions options_;
  // Reusable entries vector that deletes the entries on destruction.
  scoped_refptr<LogAnchorRegistry> log_anchor_registry_;
  scoped_refptr<Clock> clock_;
  string tablet_wal_path_;
  RestartSafeCoarseMonoClock restart_safe_coarse_mono_clock_;
};

// Corrupts the last segment of the provided log by either truncating it
// or modifying a byte at the given offset.
enum CorruptionType {
  TRUNCATE_FILE,
  FLIP_BYTE
};

Status CorruptLogFile(Env* env, const string& log_path,
                      CorruptionType type, int corruption_offset) {
  faststring buf;
  RETURN_NOT_OK_PREPEND(ReadFileToString(env, log_path, &buf),
                        "Couldn't read log");

  switch (type) {
    case TRUNCATE_FILE:
      buf.resize(corruption_offset);
      break;
    case FLIP_BYTE:
      CHECK_LT(corruption_offset, buf.size());
      buf[corruption_offset] ^= 0xff;
      break;
  }

  // Rewrite the file with the corrupt log.
  RETURN_NOT_OK_PREPEND(WriteStringToFile(env, Slice(buf), log_path),
                        "Couldn't rewrite corrupt log file");

  return Status::OK();
}

} // namespace log
} // namespace yb

#endif // YB_CONSENSUS_LOG_TEST_BASE_H
