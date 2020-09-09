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

#include <vector>

#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log-test-base.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/consensus-test-util.h"
#include "yb/server/logical_clock.h"
#include "yb/server/metadata.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet-test-util.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/logging.h"
#include "yb/util/path_util.h"
#include "yb/util/random_util.h"
#include "yb/util/tostring.h"
#include "yb/tablet/tablet_options.h"
#include "yb/util/env_util.h"
#include "yb/yql/pggate/ybc_pg_typedefs.h"

DECLARE_bool(skip_flushed_entries);
DECLARE_int32(retryable_request_timeout_secs);

using std::shared_ptr;
using std::string;
using std::vector;

namespace yb {

namespace log {

extern const char* kTestTable;
extern const char* kTestTablet;
extern const char* kTestNamespace;

} // namespace log

namespace tablet {

using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::kMinimumTerm;
using consensus::MakeOpId;
using consensus::ReplicateMsg;
using consensus::ReplicateMsgPtr;
using log::Log;
using log::LogAnchorRegistry;
using log::LogTestBase;
using log::ReadableLogSegment;
using log::AppendSync;
using server::Clock;
using server::LogicalClock;
using tserver::WriteRequestPB;

struct BootstrapReport {
  // OpIds replayed using Play... functions.
  std::vector<OpId> replayed;

  // OpIds replayed only into the intents RocksDB (already flushed in the regular RocksDB).
  std::vector<OpId> replayed_to_intents_only;

  // Entries overwritten by a later entry with the same or lower index, from a leader of a later
  // term.
  std::vector<OpId> overwritten;

  // OpIds registered with RetryableRequests. This sometimes includes flushed entries.
  std::vector<OpId> retryable_requests;

  // First OpIds of segments to replay, in reverse order (we traverse them from latest to earliest
  // in TabletBootstrap).
  std::vector<OpId> first_op_ids_of_segments_reversed;
};

struct BootstrapTestHooksImpl : public TabletBootstrapTestHooksIf {
  virtual ~BootstrapTestHooksImpl() {}

  void Clear() {
    *this = BootstrapTestHooksImpl();
  }

  boost::optional<DocDbOpIds> GetFlushedOpIdsOverride() const override {
    return flushed_op_ids;
  }

  void Replayed(OpId op_id, AlreadyAppliedToRegularDB already_applied_to_regular_db) override {
    actual_report.replayed.push_back(op_id);
    if (already_applied_to_regular_db) {
      actual_report.replayed_to_intents_only.push_back(op_id);
    }
  }

  void Overwritten(OpId op_id) override {
    actual_report.overwritten.push_back(op_id);
  };

  void RetryableRequest(OpId op_id) override {
    actual_report.retryable_requests.push_back(op_id);
  }

  bool ShouldSkipTransactionUpdates() const override {
    return true;
  }

  bool ShouldSkipWritingIntents() const override {
    return true;
  }

  bool HasIntentsDB() const override {
    return transactional;
  }

  void FirstOpIdOfSegment(const std::string& path, OpId first_op_id) override {
    LOG(INFO) << "First OpId of segment " << DirName(path) << ": " << first_op_id;
    actual_report.first_op_ids_of_segments_reversed.push_back(first_op_id);
  }

  // ----------------------------------------------------------------------------------------------
  // These fields are populated based in callbacks from TabletBootstrap.
  // ----------------------------------------------------------------------------------------------

  // This is queried by TabletBootstrap during its initialization.
  boost::optional<DocDbOpIds> flushed_op_ids;

  BootstrapReport actual_report;

  // A parameter set by the test.
  bool transactional = false;
};

class BootstrapTest : public LogTestBase {
 protected:

  static constexpr TableType kTableType = TableType::YQL_TABLE_TYPE;

  void SetUp() override {
    LogTestBase::SetUp();
    test_hooks_ = std::make_shared<BootstrapTestHooksImpl>();
  }

  Status LoadTestRaftGroupMetadata(RaftGroupMetadataPtr* meta) {
    Schema schema = SchemaBuilder(schema_).Build();
    std::pair<PartitionSchema, Partition> partition = CreateDefaultPartition(schema);

    RETURN_NOT_OK(RaftGroupMetadata::LoadOrCreate(
        fs_manager_.get(),
        log::kTestTable,
        log::kTestTablet,
        log::kTestNamespace,
        log::kTestTable,
        kTableType,
        schema,
        partition.first,
        partition.second,
        boost::none /* index_info */,
        TABLET_DATA_READY,
        meta));
    return (*meta)->Flush();
  }

  Status PersistTestRaftGroupMetadataState(TabletDataState state) {
    RaftGroupMetadataPtr meta;
    RETURN_NOT_OK(LoadTestRaftGroupMetadata(&meta));
    meta->set_tablet_data_state(state);
    RETURN_NOT_OK(meta->Flush());
    return Status::OK();
  }

  Status RunBootstrapOnTestTablet(const RaftGroupMetadataPtr& meta,
                                  TabletPtr* tablet,
                                  ConsensusBootstrapInfo* boot_info) {
    gscoped_ptr<TabletStatusListener> listener(new TabletStatusListener(meta));
    scoped_refptr<LogAnchorRegistry> log_anchor_registry(new LogAnchorRegistry());
    // Now attempt to recover the log
    TabletOptions tablet_options;
    TabletInitData tablet_init_data = {
      .metadata = meta,
      .client_future = std::shared_future<client::YBClient*>(),
      .clock = scoped_refptr<Clock>(LogicalClock::CreateStartingAt(HybridTime::kInitial)),
      .parent_mem_tracker = shared_ptr<MemTracker>(),
      .block_based_table_mem_tracker = shared_ptr<MemTracker>(),
      .metric_registry = nullptr,
      .log_anchor_registry = log_anchor_registry,
      .tablet_options = tablet_options,
      .log_prefix_suffix = std::string(),
      .transaction_participant_context = nullptr,
      .local_tablet_filter = client::LocalTabletFilter(),
      .transaction_coordinator_context = nullptr,
      .txns_enabled = TransactionsEnabled::kTrue,
      .is_sys_catalog = IsSysCatalogTablet::kFalse,
    };
    BootstrapTabletData data = {
      .tablet_init_data = tablet_init_data,
      .listener = listener.get(),
      .append_pool = log_thread_pool_.get(),
      .allocation_pool = log_thread_pool_.get(),
      .retryable_requests = nullptr,
      .test_hooks = test_hooks_
    };
    RETURN_NOT_OK(BootstrapTablet(data, tablet, &log_, boot_info));
    return Status::OK();
  }

  Status BootstrapTestTablet(
      TabletPtr* tablet,
      ConsensusBootstrapInfo* boot_info) {
    RaftGroupMetadataPtr meta;
    RETURN_NOT_OK_PREPEND(LoadTestRaftGroupMetadata(&meta),
                          "Unable to load test tablet metadata");

    consensus::RaftConfigPB config;
    config.set_opid_index(consensus::kInvalidOpIdIndex);
    consensus::RaftPeerPB* peer = config.add_peers();
    peer->set_permanent_uuid(meta->fs_manager()->uuid());
    peer->set_member_type(consensus::RaftPeerPB::VOTER);

    std::unique_ptr<ConsensusMetadata> cmeta;
    RETURN_NOT_OK_PREPEND(ConsensusMetadata::Create(meta->fs_manager(), meta->raft_group_id(),
                                                    meta->fs_manager()->uuid(),
                                                    config, kMinimumTerm, &cmeta),
                          "Unable to create consensus metadata");

    RETURN_NOT_OK_PREPEND(RunBootstrapOnTestTablet(meta, tablet, boot_info),
                          "Unable to bootstrap test tablet");
    return Status::OK();
  }

  void IterateTabletRows(const Tablet* tablet,
                         vector<string>* results) {
    auto iter = tablet->NewRowIterator(schema_, /* transaction_id */ boost::none);
    ASSERT_OK(iter);
    ASSERT_OK(IterateToStringList(iter->get(), results));
    for (const string& result : *results) {
      VLOG(1) << result;
    }
  }

  std::shared_ptr<BootstrapTestHooksImpl> test_hooks_;
};

// ===============================================================================================
// TESTS
// ===============================================================================================

// Tests a normal bootstrap scenario.
TEST_F(BootstrapTest, TestBootstrap) {
  BuildLog();
  const auto current_op_id = MakeOpId(1, current_index_);
  AppendReplicateBatch(current_op_id, current_op_id);
  TabletPtr tablet;
  ConsensusBootstrapInfo boot_info;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));

  vector<string> results;
  IterateTabletRows(tablet.get(), &results);
}

// Tests attempting a local bootstrap of a tablet that was in the middle of a remote bootstrap
// before "crashing".
TEST_F(BootstrapTest, TestIncompleteRemoteBootstrap) {
  BuildLog();

  ASSERT_OK(PersistTestRaftGroupMetadataState(TABLET_DATA_COPYING));
  TabletPtr tablet;
  ConsensusBootstrapInfo boot_info;
  Status s = BootstrapTestTablet(&tablet, &boot_info);
  ASSERT_TRUE(s.IsCorruption()) << "Expected corruption: " << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "RaftGroupMetadata bootstrap state is TABLET_DATA_COPYING");
  LOG(INFO) << "State is still TABLET_DATA_COPYING, as expected: " << s.ToString();
}

// Test a crash before a REPLICATE message is marked as committed by a future REPLICATE message.
// Bootstrap should not replay the operation, but should return it in the ConsensusBootstrapInfo.
TEST_F(BootstrapTest, TestOrphanedReplicate) {
  BuildLog();

  // Append a REPLICATE with no commit
  int replicate_index = current_index_++;

  OpIdPB opid = MakeOpId(1, replicate_index);

  AppendReplicateBatch(opid);

  // Bootstrap the tablet. It shouldn't replay anything.
  ConsensusBootstrapInfo boot_info;
  TabletPtr tablet;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));

  // Table should be empty because we didn't replay the REPLICATE.
  vector<string> results;
  IterateTabletRows(tablet.get(), &results);
  ASSERT_EQ(0, results.size());

  // The consensus bootstrap info should include the orphaned REPLICATE.
  ASSERT_EQ(1, boot_info.orphaned_replicates.size())
      << yb::ToString(boot_info.orphaned_replicates);
  ASSERT_STR_CONTAINS(boot_info.orphaned_replicates[0]->ShortDebugString(),
                      "this is a test mutate");

  // And it should also include the latest opids.
  EXPECT_EQ("term: 1 index: 1", boot_info.last_id.ShortDebugString());
}

// Bootstrap should fail if no ConsensusMetadata file exists.
TEST_F(BootstrapTest, TestMissingConsensusMetadata) {
  BuildLog();

  RaftGroupMetadataPtr meta;
  ASSERT_OK(LoadTestRaftGroupMetadata(&meta));

  TabletPtr tablet;
  ConsensusBootstrapInfo boot_info;
  Status s = RunBootstrapOnTestTablet(meta, &tablet, &boot_info);

  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "Unable to load Consensus metadata");
}

// Tests that when we have two consecutive replicates and the commit index specified in the second
// is that of the first, only the first one is committed.
TEST_F(BootstrapTest, TestCommitFirstMessageBySpecifyingCommittedIndexInSecond) {
  BuildLog();

  // This appends a write with op 1.1
  const OpIdPB insert_opid = MakeOpId(1, 1);
  AppendReplicateBatch(insert_opid, MakeOpId(0, 0),
                       {TupleForAppend(10, 1, "this is a test insert")}, AppendSync::kTrue);

  // This appends a write with op 1.2 and commits the previous one.
  const OpIdPB mutate_opid = MakeOpId(1, 2);
  AppendReplicateBatch(mutate_opid, insert_opid,
                       {TupleForAppend(10, 2, "this is a test mutate")}, AppendSync::kTrue);
  ConsensusBootstrapInfo boot_info;
  TabletPtr tablet;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));
  ASSERT_EQ(boot_info.orphaned_replicates.size(), 1);
  ASSERT_OPID_EQ(boot_info.last_committed_id, insert_opid);

  // Confirm that one operation was applied.
  vector<string> results;
  IterateTabletRows(tablet.get(), &results);
  ASSERT_EQ(1, results.size());
}

TEST_F(BootstrapTest, TestOperationOverwriting) {
  BuildLog();

  const OpIdPB opid = MakeOpId(1, 1);

  // Append a replicate in term 1 with only one row.
  AppendReplicateBatch(opid, MakeOpId(0, 0), {TupleForAppend(1, 0, "this is a test insert")});

  // Now append replicates for 4.2 and 4.3
  AppendReplicateBatch(MakeOpId(4, 2));
  AppendReplicateBatch(MakeOpId(4, 3));

  ASSERT_OK(RollLog());
  // And overwrite with 3.2
  AppendReplicateBatch(MakeOpId(3, 2), MakeOpId(1, 1), {}, AppendSync::kTrue);

  // When bootstrapping we should apply ops 1.1 and get 3.2 as pending.
  ConsensusBootstrapInfo boot_info;
  TabletPtr tablet;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));

  ASSERT_EQ(boot_info.orphaned_replicates.size(), 1);
  ASSERT_OPID_EQ(boot_info.orphaned_replicates[0]->id(), MakeOpId(3, 2));

  // Confirm that the legitimate data is there.
  vector<string> results;
  IterateTabletRows(tablet.get(), &results);
  ASSERT_EQ(1, results.size());

  ASSERT_EQ("{ int32_value: 1 int32_value: 0 string_value: \"this is a test insert\" }",
            results[0]);
}

TEST_F(BootstrapTest, OverwriteTailWithFlushedIndex) {
  BuildLog();

  test_hooks_->flushed_op_ids = DocDbOpIds{{3, 2}, {3, 2}};

  const std::string kTestStr("this is a test insert");
  const auto get_test_tuple = [kTestStr](int i) {
    return TupleForAppend(i, 0, kTestStr);
  };

  // Append a replicate in term 1 with only one row (not committed yet).
  const auto nothing_committed = MakeOpId(0, 0);

  AppendReplicateBatch(MakeOpId(1, 1), nothing_committed, {get_test_tuple(10)});

  // Now append replicates for 2.2 and 2.3 (not committed yet).
  AppendReplicateBatch(MakeOpId(2, 2), nothing_committed, {get_test_tuple(1020)});
  AppendReplicateBatch(MakeOpId(2, 3), nothing_committed, {get_test_tuple(1030)});

  // And overwrite with 3.2, committing 1.1 and 3.2. This should abort 2.2 and a 2.3.
  AppendReplicateBatch(MakeOpId(3, 2), MakeOpId(3, 2), {get_test_tuple(20)});

  AppendReplicateBatch(MakeOpId(3, 3), MakeOpId(3, 2), {get_test_tuple(30)});
  AppendReplicateBatch(MakeOpId(3, 4), MakeOpId(3, 3), {get_test_tuple(40)});

  ConsensusBootstrapInfo boot_info;
  TabletPtr tablet;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));

  LOG(INFO) << "Replayed OpIds: " << ToString(test_hooks_->actual_report.replayed);

  ASSERT_EQ(boot_info.orphaned_replicates.size(), 1);
  ASSERT_OPID_EQ(boot_info.orphaned_replicates[0]->id(), MakeOpId(3, 4));

  const std::vector<OpId> expected_replayed_op_ids{{3, 3}};
  ASSERT_EQ(expected_replayed_op_ids, test_hooks_->actual_report.replayed);

  // Confirm that the legitimate data is there. Note that none of the data for previously flushed
  // OpIds (anything at index 2 or before) has been replayed.
  vector<string> results;
  IterateTabletRows(tablet.get(), &results);
  ASSERT_EQ(1, results.size());

  ASSERT_EQ(
      Format("{ int32_value: 30 int32_value: 0 string_value: \"$0\" }", kTestStr),
      results[0]);
}

// Test that we do not crash when a consensus-only operation has a hybrid_time that is higher than a
// hybrid_time assigned to a write operation that follows it in the log.
// TODO: this must not happen in YB. Ensure this is not happening and update the test.
TEST_F(BootstrapTest, TestConsensusOnlyOperationOutOfOrderHybridTime) {
  BuildLog();

  // Append NO_OP.
  auto noop_replicate = std::make_shared<ReplicateMsg>();
  noop_replicate->set_op_type(consensus::NO_OP);
  *noop_replicate->mutable_id() = MakeOpId(1, 1);
  noop_replicate->set_hybrid_time(2);

  // All YB REPLICATEs require this:
  *noop_replicate->mutable_committed_op_id() = MakeOpId(0, 0);

  AppendReplicateBatch(noop_replicate, AppendSync::kTrue);

  // Append WRITE_OP with higher OpId and lower hybrid_time, and commit both messages.
  const auto second_opid = MakeOpId(1, 2);
  AppendReplicateBatch(second_opid, second_opid, {TupleForAppend(1, 1, "foo")});

  ConsensusBootstrapInfo boot_info;
  TabletPtr tablet;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));
  ASSERT_EQ(boot_info.orphaned_replicates.size(), 0);
  ASSERT_OPID_EQ(boot_info.last_committed_id, second_opid);

  // Confirm that the insert op was applied.
  vector<string> results;
  IterateTabletRows(tablet.get(), &results);
  ASSERT_EQ(1, results.size());
}

// Test that we don't overflow opids. Regression test for KUDU-1933.
TEST_F(BootstrapTest, TestBootstrapHighOpIdIndex) {
  // Start appending with a log index 3 under the int32 max value.
  // Append 6 log entries, which will roll us right through the int32 max.
  const int64_t first_log_index = std::numeric_limits<int32_t>::max() - 3;
  const int kNumEntries = 6;
  BuildLog();
  current_index_ = first_log_index;
  for (int i = 0; i < kNumEntries; i++) {
    AppendReplicateBatchToLog(1);
  }

  // Kick off tablet bootstrap and ensure everything worked.
  TabletPtr tablet;
  ConsensusBootstrapInfo boot_info;
  ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));
  OpIdPB last_opid;
  last_opid.set_term(1);
  last_opid.set_index(current_index_ - 1);
  ASSERT_OPID_EQ(last_opid, boot_info.last_id);
  ASSERT_OPID_EQ(last_opid, boot_info.last_committed_id);
}

struct BootstrapInputEntry {
  const OpId& op_id() const { return batch_data.op_id; }

  bool IsTransactional() const { return !batch_data.txn_id.IsNil(); }

  const std::string ToString() const {
    std::ostringstream ss;
    ss << "{ ";
    ss << "op_id: " << op_id() << " ";
    ss << "committed_op_id: " << batch_data.committed_op_id << " ";
    ss << "op_type: " << consensus::OperationType_Name(batch_data.op_type) << " ";
    if (IsTransactional()) {
      ss << "txn: " << batch_data.txn_id << " ";
      ss << "txn_status: " << TransactionStatus_Name(batch_data.txn_status) << " ";
    }
    if (start_new_segment_with_this_entry) {
      ss << "start_new_segment_with_this_entry: true ";
    }
    ss << "}";
    return ss.str();
  }

  LogTestBase::AppendReplicateBatchData batch_data;

  bool start_new_segment_with_this_entry = false;
};

struct BootstrapInput {
  // All entries that are written to the log and then bootstrapped.
  std::vector<BootstrapInputEntry> entries;

  BootstrapReport expected_report;

  // This should match the OpIds of entries we call "orphaned replicates" at the end of bootstrap.
  std::vector<OpId> uncommitted_tail;

  // Entries that can be overwritten. None of these are comitted.
  std::set<OpId> overwritable;

  // Committed entries. None of these can be overwritten.
  std::set<OpId> committed;

  DocDbOpIds flushed_op_ids;
  OpId final_committed_op_id;
  bool transactional = false;
};

// ------------------------------------------------------------------------------------------------
// Randomized bootstrap test
// ------------------------------------------------------------------------------------------------

// An internal function for generating a randomized tablet bootstrap input. Populates the entries
// vector in the res_input struct. Also returns the final map of index to OpId, with all overwrites
// of entries by a new leader having already taken place.
std::map<int64_t, OpId> GenerateRawEntriesAndFinalOpByIndex(
    const size_t num_entries,
    std::mt19937_64* const rng,
    BootstrapInput* const res_input) {
  const bool transactional = res_input->transactional;
  auto& entries = res_input->entries;

  // This map holds the final OpId at any given index, provided that it has not been overwritten
  // by an entry at a later term and the same or earlier index.
  std::map<int64_t, OpId> final_op_id_by_index;

  int64_t index = 1;
  int64_t term = 1;
  int64_t max_index = 1;
  entries.resize(num_entries);
  for (size_t i = 0; i < num_entries; ++i) {
    auto& entry = entries[i];
    auto& batch_data = entry.batch_data;
    batch_data.op_id = {term, index};
    if (transactional && RandomUniformInt(1, 4, rng) == 1) {
      batch_data.op_type = consensus::OperationType::UPDATE_TRANSACTION_OP;
      batch_data.txn_status = TransactionStatus::APPLYING;
      batch_data.txn_id = TransactionId::GenerateRandom(rng);
    } else {
      batch_data.op_type = consensus::OperationType::WRITE_OP;
      if (transactional && RandomUniformInt(1, 2, rng) == 1) {
        batch_data.txn_id = TransactionId::GenerateRandom(rng);
      }
    }

    final_op_id_by_index[index] = batch_data.op_id;
    max_index = std::max(max_index, index);

    // The first entry always start a new segment. Otherwise, we start a new segment randomly.
    entry.start_new_segment_with_this_entry = i == 0 || RandomUniformInt(1, 100, rng) == 1;

    // Advance to the next OpId.
    if (i < num_entries - 1) {
      if (RandomUniformInt(1, 30) == 1) {
        // We advance to the next term and the new leader overwrites a tail of the log.
        term++;

        // Jump back in most cases, but index_delta of 0 (keeping the same index) or even
        // index_delta of 1 (increasing the index by 1) are also possible.
        const auto index_delta = RandomUniformInt(
            // Jump back by some amount. Very rarely, we might jump back pretty far.
            RandomUniformInt(1, 500) == 0 ? -200 : -10,
            // Upper bound is 1, meaning we increment both term and index.
            1,
            rng);
        index = std::max<int64_t>(1, index + index_delta);
      } else {
        // In the majority of cases we just advance to the next index.
        index++;
      }

      const auto lower_bound_it = final_op_id_by_index.lower_bound(index);
      final_op_id_by_index.erase(lower_bound_it, final_op_id_by_index.end());
    }
  }

  return final_op_id_by_index;
}

void GenerateRandomInput(size_t num_entries, std::mt19937_64* rng, BootstrapInput* res_input) {
  auto& entries = res_input->entries;
  const bool transactional = RandomUniformBool(rng);
  res_input->transactional = transactional;

  const auto final_op_id_by_index = GenerateRawEntriesAndFinalOpByIndex(
      num_entries, rng, res_input);

  const auto committed_op_id_for_index = [&final_op_id_by_index](int index) -> OpId {
    auto it = final_op_id_by_index.find(index);
    if (it == final_op_id_by_index.end()) {
      return OpId();
    }
    return it->second;
  };

  const auto is_op_id_committable = [&committed_op_id_for_index](const OpId& op_id) {
    return committed_op_id_for_index(op_id.index) == op_id;
  };

  // ----------------------------------------------------------------------------------------------
  // Compute committed OpId for every entry, as well as the final committed OpId.
  {
    OpId committed_op_id;

    // Entries that have not been overwritten by future entries with the same index and a later
    // term.
    std::set<int64_t> finalized_indexes;
    int64_t committable_up_to_index = 0;

    for (auto& entry : entries) {
      const auto& op_id = entry.op_id();

      if (is_op_id_committable(op_id)) {
        finalized_indexes.insert(op_id.index);
      }

      while (finalized_indexes.count(committable_up_to_index + 1)) {
        committable_up_to_index++;
      }

      if (committable_up_to_index >= 1) {
        const int64_t new_committed_index =
            RandomUniformBool(rng) ? committed_op_id.index
                                   : RandomUniformInt(committed_op_id.index,
                                                      committable_up_to_index,
                                                      rng);
        const auto new_committed_op_id = committed_op_id_for_index(new_committed_index);
        ASSERT_GE(new_committed_op_id, committed_op_id);
        committed_op_id = new_committed_op_id;
      }

      entry.batch_data.committed_op_id = committed_op_id;
    }

    res_input->final_committed_op_id = committed_op_id;
  }

  // ----------------------------------------------------------------------------------------------
  // Choose flushed OpIds for regular and intents RocksDBs.
  // ----------------------------------------------------------------------------------------------

  // Test the important case of the flushed OpIds being exactly equal to the last entry in the
  // log. In this case we would previously fail to correctly overwrite the tail of the log
  // because we would not even look at these entries.
  //
  // More details https://github.com/yugabyte/yugabyte-db/issues/5003
  const bool all_entries_committed_and_flushed = RandomUniformInt(1, 20, rng) == 1;
  if (all_entries_committed_and_flushed) {
    res_input->final_committed_op_id = entries.back().op_id();
  }
  const auto final_committed_op_id = res_input->final_committed_op_id;

  const auto regular_flushed_op_id = all_entries_committed_and_flushed
      ? final_committed_op_id
      : committed_op_id_for_index(RandomUniformInt<int64_t>(0, final_committed_op_id.index, rng));

  // Intents RocksDB cannot be ahead of regular RocksDB in its flushed OpId.
  const auto intents_flushed_op_id = transactional ? (all_entries_committed_and_flushed
      ? final_committed_op_id
      : (RandomUniformBool(rng)
          // Make intents and regular DB's flushed OpId the same with a 50% probability.
          ? regular_flushed_op_id
          // Otherwise, the flushed index in the intents DB will be lagging that of the regular DB.
          : committed_op_id_for_index(RandomUniformInt<int64>(0, regular_flushed_op_id.index, rng))
      )
  ) : /* or, in the non-transactional case: */ OpId();

  res_input->flushed_op_ids = {
    .regular = regular_flushed_op_id,
    .intents = intents_flushed_op_id
  };
  const int64_t intents_flushed_index = intents_flushed_op_id.index;
  const int64_t regular_flushed_index = regular_flushed_op_id.index;

  const std::vector<OpId> first_opids_in_segments = [&entries]() {
    std::vector<OpId> first_op_ids;
    for (const auto& entry : entries) {
      if (entry.start_new_segment_with_this_entry) {
        first_op_ids.push_back(entry.op_id());
      }
    }
    return first_op_ids;
  }();

  // ----------------------------------------------------------------------------------------------
  // Determine segments that the bootstrap procedure will replay.
  // ----------------------------------------------------------------------------------------------

  // Find the first OpId of the segment that we'll look at in the --skip_wal_rewrite mode.
  const OpId first_op_id_of_segment_to_replay = [&]() {
    // This is the cut-off OpId that we use in the "bootstrap optimizer" (--skip_wal_rewrite) logic
    // to find the first log segment to replay. The production code uses min of intents and regular
    // flushed OpId, but we know that intents_flushed_op_id <= regular_flushed_op_id.
    const auto flushed_op_id_for_first_segment_search = transactional ?
        intents_flushed_op_id : regular_flushed_op_id;

    // Find the first OpId in the array of first OpIds of segments such that it is greater than the
    // cut-off. Then, the segment before that will be the last segment with OpId <= cutoff, which is
    // what we need.
    const auto first_segment_to_replay_op_id_it = std::upper_bound(
        first_opids_in_segments.begin(), first_opids_in_segments.end(),
        flushed_op_id_for_first_segment_search);
    return first_segment_to_replay_op_id_it == first_opids_in_segments.begin()
        ? entries.front().op_id()
        : *(first_segment_to_replay_op_id_it - 1);
  }();

  for (auto it = first_opids_in_segments.rbegin(); it != first_opids_in_segments.rend(); it++) {
    if (*it < first_op_id_of_segment_to_replay)
      break;
    res_input->expected_report.first_op_ids_of_segments_reversed.push_back(*it);
  }

  // ----------------------------------------------------------------------------------------------
  // Compute expected overwritten OpIds.
  // ----------------------------------------------------------------------------------------------

  // Compute the set of OpIds to be overwritten by iterating starting with the first segment
  // that will be replayed.
  {
    auto& exact_overwrites = res_input->expected_report.overwritten;
    std::map<int64_t, OpId> pending_replicates;
    for (const auto& entry : entries) {
      const auto& op_id = entry.op_id();
      if (op_id >= first_op_id_of_segment_to_replay) {
        auto remove_from_it = pending_replicates.lower_bound(entry.op_id().index);
        for (auto it = remove_from_it; it != pending_replicates.end(); ++it) {
          exact_overwrites.push_back(it->second);
        }
        pending_replicates.erase(remove_from_it, pending_replicates.end());
        ASSERT_TRUE(pending_replicates.emplace(op_id.index, op_id).second);
      }
    }
  }

  // ----------------------------------------------------------------------------------------------
  // Compute expected replayed OpIds, OpIds to be added to RetryableRequests, "overwritable" OpIds.
  // ----------------------------------------------------------------------------------------------

  {
    auto& replayed = res_input->expected_report.replayed;
    auto& replayed_to_intents_only = res_input->expected_report.replayed_to_intents_only;
    for (const auto& entry : entries) {
      const auto op_id = entry.op_id();
      const auto& batch_data = entry.batch_data;
      const auto op_type = batch_data.op_type;
      const int64_t index = op_id.index;
      const bool is_transactional = entry.IsTransactional();
      if (is_op_id_committable(op_id) && op_id <= final_committed_op_id) {
        // This operation has been committed in Raft.
        res_input->committed.insert(op_id);
        if (op_id >= first_op_id_of_segment_to_replay) {
          res_input->expected_report.retryable_requests.push_back(op_id);
        }
        if (index > intents_flushed_index) {
          if (op_id >= first_op_id_of_segment_to_replay) {
            bool replay = true;
            if (index <= regular_flushed_index) {
              // We are in the (intents_flushed_index, regular_flushed_index] range. Special rules
              // are used to decide whether to replay these operations.
              if (op_type == consensus::OperationType::WRITE_OP) {
                // Only need to replay intent writes in this index range.
                replay = is_transactional;
              } else if (op_type == consensus::OperationType::UPDATE_TRANSACTION_OP) {
                replay = batch_data.txn_status == TransactionStatus::APPLYING;
                if (replay) {
                  replayed_to_intents_only.push_back(op_id);
                }
              } else {
                FAIL() << "Unknown operation type: " << consensus::OperationType_Name(op_type);
              }
            }
            if (replay) {
              replayed.push_back(op_id);
            }
          }
        }
      } else {
        // This operation was never committed. Mark it as "overwritable", meaning it _could_ be
        // overwritten as part of tablet bootstrap, but is not guaranteed to be.
        res_input->overwritable.insert(op_id);
      }
    }
  }

  // ----------------------------------------------------------------------------------------------
  // Uncommitted tail / orphaned replicates
  // ----------------------------------------------------------------------------------------------

  // Compute the expected "uncommitted tail" of operations, i.e. those operations that will be left
  // in "orphaned replicates" at the end of tablet bootstrap because we don't know if they are
  // Raft-committed yet.
  res_input->uncommitted_tail.clear();
  for (const auto& index_and_final_op_id : final_op_id_by_index) {
    const auto& op_id = index_and_final_op_id.second;
    if (op_id > final_committed_op_id) {
      res_input->uncommitted_tail.push_back(index_and_final_op_id.second);
    }
  }
}

TEST_F(BootstrapTest, RandomizedInput) {
  std::mt19937_64 rng;

  // Do not change this random seed so we can keep the tests repeatable.
  rng.seed(3141592653);

  const bool kVerboseOutput = false;  // Turn this on when debugging the test.

  // This is to avoid non-deterministic time-based behavior in "bootstrap optimizer"
  // (skip_wal_rewrite mode).
  FLAGS_retryable_request_timeout_secs = 0;

  const auto kNumIter = NonTsanVsTsan(400, 150);
  const auto kNumEntries = NonTsanVsTsan(1500, 500);
  for (int iteration = 1; iteration <= kNumIter; ++iteration) {
    LOG(INFO) << "Starting test iteration " << iteration;
    SCOPED_TRACE(Format("Test iteration $0", iteration));
    BootstrapInput input;
    ASSERT_NO_FATALS(GenerateRandomInput(kNumEntries, &rng, &input));
    if (kVerboseOutput) {
      for (const auto& entry : input.entries) {
        LOG(INFO) << "Entry: " << entry.ToString();
      }
    }
    LOG(INFO) << "Flushed OpIds in the test case: " << input.flushed_op_ids.ToString();

    CleanTablet();
    test_hooks_->Clear();
    test_hooks_->flushed_op_ids = input.flushed_op_ids;
    test_hooks_->transactional = input.transactional;
    LOG(INFO) << "Test iteration " << iteration << " is "
              << (input.transactional ? "TRANSACTIONAL" : "NON-TRANSACTIONAL");
    SCOPED_TRACE(Format("Test iteration $0 is transactional: $1", iteration, input.transactional));

    BuildLog();

    for (auto i = 0; i < input.entries.size(); ++i) {
      const auto& entry = input.entries[i];
      if (entry.start_new_segment_with_this_entry && i != 0) {
        ASSERT_OK(RollLog());
      }
      AppendReplicateBatch(entry.batch_data);
    }

    TabletPtr tablet;
    ConsensusBootstrapInfo boot_info;
    ASSERT_OK(BootstrapTestTablet(&tablet, &boot_info));

    std::ostringstream error_details;
    const auto& expected_report = input.expected_report;
    const auto& actual_report = test_hooks_->actual_report;
    const auto& actual_replayed_op_ids = actual_report.replayed;

    bool test_failed = false;
    for (const auto& op_id : actual_replayed_op_ids) {
      if (input.committed.count(op_id) == 0) {
        const auto msg = Format("An uncommitted entry was replayed: $0", op_id);
        LOG(ERROR) << "Failure: " << msg;
        error_details << msg << std::endl;
        test_failed = true;
      }
    }
    ASSERT_VECTORS_EQ(expected_report.replayed, actual_replayed_op_ids);
    ASSERT_VECTORS_EQ(
        expected_report.replayed_to_intents_only,
        actual_report.replayed_to_intents_only);

    for (const auto& op_id : actual_report.overwritten) {
      auto it = input.overwritable.find(op_id);
      if (it == input.overwritable.end()) {
        FAIL() << "Entry " << op_id << " was overwritten but was not suppossed to be.";
      }
    }

    std::vector<OpId> actual_uncommitted_tail;
    actual_uncommitted_tail.reserve(boot_info.orphaned_replicates.size());
    for (const auto& orphaned_replicate : boot_info.orphaned_replicates) {
      actual_uncommitted_tail.push_back(OpId::FromPB(orphaned_replicate->id()));
    }

    ASSERT_VECTORS_EQ(input.uncommitted_tail, actual_uncommitted_tail);
    ASSERT_VECTORS_EQ(
        expected_report.overwritten,
        actual_report.overwritten);
    ASSERT_VECTORS_EQ(
        expected_report.retryable_requests,
        actual_report.retryable_requests);
    ASSERT_VECTORS_EQ(
        expected_report.first_op_ids_of_segments_reversed,
        actual_report.first_op_ids_of_segments_reversed);

    if (test_failed) {
      FAIL() << error_details.str();
    }
    LOG(INFO) << "Test iteration " << iteration << " has succeeded";
  }
}

} // namespace tablet
} // namespace yb
