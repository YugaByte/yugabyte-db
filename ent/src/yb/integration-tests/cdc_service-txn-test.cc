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

#include "yb/common/wire_protocol.h"
#include "yb/common/wire_protocol-test-util.h"

#include "yb/cdc/cdc_service.h"
#include "yb/cdc/cdc_service.proxy.h"

#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/transaction.h"
#include "yb/client/txn-test-base.h"

#include "yb/docdb/primitive_value.h"
#include "yb/docdb/value_type.h"

#include "yb/integration-tests/cdc_test_util.h"

#include "yb/rpc/messenger.h"

#include "yb/tablet/tablet.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/slice.h"

DECLARE_bool(cdc_enable_replicate_intents);

namespace yb {
namespace cdc {

using client::Flush;
using client::TransactionTestBase;
using client::WriteOpType;
using rpc::RpcController;

class CDCServiceTxnTest : public TransactionTestBase<MiniCluster>,
                          public testing::WithParamInterface<bool /* enable_intents */> {
 protected:
  void SetUp() override {
    mini_cluster_opt_.num_masters = 1;
    mini_cluster_opt_.num_tablet_servers = 1;
    SetAtomicFlag(GetParam(), &FLAGS_cdc_enable_replicate_intents);
    create_table_ = false;
    SetIsolationLevel(IsolationLevel::SERIALIZABLE_ISOLATION);
    SetNumTablets(1);
    TransactionTestBase::SetUp();
    CreateTable();

    const auto mini_server = cluster_->mini_tablet_servers().front();
    cdc_proxy_ = std::make_unique<CDCServiceProxy>(
        &client_->proxy_cache(), HostPort::FromBoundEndpoint(mini_server->bound_rpc_addr()));
  }

  std::unique_ptr<CDCServiceProxy> cdc_proxy_;
};

INSTANTIATE_TEST_CASE_P(EnableIntentReplication, CDCServiceTxnTest, ::testing::Bool());

void AssertValue(const google::protobuf::Map<string, QLValuePB>& changes, int32_t expected_value) {
  ASSERT_EQ(changes.size(), 1);
  const auto& value = changes.find("value");
  ASSERT_NE(value, changes.end());
  ASSERT_EQ(value->second.int32_value(), expected_value);
}

void CheckIntentRecord(const CDCRecordPB& record, int expected_value, bool replicate_intents) {
  ASSERT_EQ(record.changes_size(), 1);
  // Check the key.
  ASSERT_NO_FATALS(AssertIntKey(record.key(), expected_value));
  // Make sure transaction metadata is set.
  if (replicate_intents) {
    ASSERT_TRUE(record.has_transaction_state());
    ASSERT_TRUE(record.has_time());
    const auto& transaction_state = record.transaction_state();
    ASSERT_TRUE(transaction_state.has_transaction_id());
  }
}

void CheckApplyRecord(const CDCRecordPB& apply_record, bool replicate_intents) {
  ASSERT_EQ(apply_record.changes_size(), 0);
  if (replicate_intents) {
    ASSERT_TRUE(apply_record.has_transaction_state());
    ASSERT_TRUE(apply_record.has_partition());
    const auto& txn_state = apply_record.transaction_state();
    ASSERT_TRUE(txn_state.has_transaction_id());
    ASSERT_EQ(apply_record.operation(), cdc::CDCRecordPB::APPLY);
    ASSERT_TRUE(apply_record.has_time());
  }
}

void CheckRegularRecord(const CDCRecordPB& record, int expected_value) {
  ASSERT_EQ(record.changes_size(), 1);
  ASSERT_NO_FATALS(AssertIntKey(record.key(), expected_value));
}

TEST_P(CDCServiceTxnTest, TestGetChanges) {
  // Consider the following writes:
  // TO: WRITE K0
  // T1: WRITE K1 (TXN1)
  // T2: WRITE K2 (TXN2)
  // T3: WRITE K3
  // T4: APPLYING TXN2
  // T5: APPLYING TXN1
  // T6: WRITE K4
  bool replicate_intents = GetParam();
  auto session = CreateSession();
  ASSERT_RESULT(WriteRow(session, 10000 /* key */, 10000 /* value */, WriteOpType::INSERT,
                         Flush::kTrue));

  auto txn1 = CreateTransaction();
  auto session1 = CreateSession(txn1);
  ASSERT_RESULT(WriteRow(session1, 10001 /* key */, 10001 /* value */, WriteOpType::INSERT,
                         Flush::kTrue));

  auto txn2 = CreateTransaction();
  auto session2 = CreateSession(txn2);
  ASSERT_RESULT(WriteRow(session2, 10002 /* key */, 10002 /* value */, WriteOpType::INSERT,
                         Flush::kTrue));

  ASSERT_RESULT(WriteRow(session, 10003 /* key */, 10003 /* value */, WriteOpType::INSERT,
                         Flush::kTrue));

  ASSERT_OK(txn2->CommitFuture().get());
  ASSERT_OK(txn1->CommitFuture().get());

  ASSERT_RESULT(WriteRow(session, 10004 /* key */, 10004 /* value */, WriteOpType::INSERT,
                         Flush::kTrue));

  // Get tablet ID.
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(client_->GetTablets(table_->name(), 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);

  // Create CDC stream on table.
  CDCStreamId stream_id;
  CreateCDCStream(cdc_proxy_, table_.table()->id(), &stream_id);

  GetChangesRequestPB change_req;
  GetChangesResponsePB change_resp;

  change_req.set_stream_id(stream_id);
  change_req.set_tablet_id(tablets.Get(0).tablet_id());
  change_req.mutable_from_checkpoint()->mutable_op_id()->set_index(0);
  change_req.mutable_from_checkpoint()->mutable_op_id()->set_term(0);

  // Get CDC changes.
  {
    RpcController rpc;
    SCOPED_TRACE(change_req.DebugString());
    ASSERT_OK(cdc_proxy_->GetChanges(change_req, &change_resp, &rpc));
    SCOPED_TRACE(change_resp.DebugString());
    ASSERT_FALSE(change_resp.has_error());

    // Expect total 7 records: 5 WRITE_OP records and 2 UPDATE_TRANSACTION_OP records.
    ASSERT_EQ(change_resp.records_size(), 7);

    struct Record {
      int32_t value; // 0 if apply record.
      bool is_intent; // Differentiate between intent and regular record.
    };

    Record expected_records_in_order[7] =
        {{10000, false}, {10001, true}, {10002, true}, {10003, false}, {0, false}, {0, false},
        {10004, false}};
    Record expected_records_out_of_order[7] =
        {{10000, false}, {10003, false}, {10002, true}, {0, false}, {10001, true}, {0, false},
        {10004, false}};

    // We expect a different order of records based on whether we replicate intents, since with the
    // feature enabled, we will receive intents at write time and not commit time.
    const Record* expected_order = replicate_intents ?
        expected_records_in_order : expected_records_out_of_order;

    for (int i = 0; i < 7; i++) {
      const auto& record = expected_order[i];
      if (record.value == 0) {
        // This contains the record for APPLYING transaction.
        ASSERT_NO_FATALS(CheckApplyRecord(change_resp.records(i), replicate_intents));
      } else if (record.is_intent) {
        ASSERT_NO_FATALS(CheckIntentRecord(change_resp.records(i), record.value,
                                           replicate_intents));
      } else {
        ASSERT_NO_FATALS(CheckRegularRecord(change_resp.records(i), record.value));
      }
    }
  }
}

TEST_P(CDCServiceTxnTest, TestGetChangesForPendingTransaction) {
  // If GetChanges is called in the middle of a transaction, ensure that transaction is not
  // incorrectly considered as aborted if we can't find the transaction commit record.
  // A subsequent call to GetChanges after the transaction is committed should get the
  // rows committed by transaction.

  static const int32_t kNumIntentsToWrite = 3;
  static const int32_t kStartKey = 10000;
  // Get tablet ID.
  bool replicate_intents = GetParam();
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(client_->GetTablets(table_->name(), 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);

  // Create CDC stream on table.
  CDCStreamId stream_id;
  CreateCDCStream(cdc_proxy_, table_.table()->id(), &stream_id);

  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  ASSERT_RESULT(WriteRow(session, kStartKey /* key */, kStartKey /* value */, WriteOpType::INSERT,
                         Flush::kTrue));
  ASSERT_RESULT(WriteRow(session, kStartKey + 1 /* key */, kStartKey + 1 /* value */,
                         WriteOpType::INSERT, Flush::kTrue));
  ASSERT_RESULT(WriteRow(session, kStartKey + 2 /* key */, kStartKey + 2 /* value */,
                         WriteOpType::INSERT, Flush::kTrue));

  GetChangesRequestPB change_req;
  GetChangesResponsePB change_resp;

  change_req.set_stream_id(stream_id);
  change_req.set_tablet_id(tablets.Get(0).tablet_id());
  change_req.mutable_from_checkpoint()->mutable_op_id()->set_index(0);
  change_req.mutable_from_checkpoint()->mutable_op_id()->set_term(0);

  // Get CDC changes.
  {
    RpcController rpc;
    SCOPED_TRACE(change_req.DebugString());
    ASSERT_OK(cdc_proxy_->GetChanges(change_req, &change_resp, &rpc));
    SCOPED_TRACE(change_resp.DebugString());
    ASSERT_FALSE(change_resp.has_error());

    // Expect 3 records for the 3 intents if we replicate intents but 0 if we don't.
    ASSERT_EQ(change_resp.records_size(), replicate_intents ? kNumIntentsToWrite : 0);
  }

  int32_t expected_order[kNumIntentsToWrite] = {kStartKey, kStartKey + 1, kStartKey + 2};

  for (int i = 0; i < change_resp.records_size(); i++) {
    ASSERT_NO_FATALS(CheckIntentRecord(change_resp.records(i), expected_order[i],
                                       replicate_intents));
  }

  // Commit transaction.
  ASSERT_OK(txn->CommitFuture().get());
  ASSERT_OK(session->Flush());

  auto checkpoint = change_resp.checkpoint();

  // Get CDC changes.
  {
    // Need to poll because Flush returns on majority_replicated and CDC waits for fully committed.
    ASSERT_OK(WaitFor([&]() -> Result<bool> {
      RpcController rpc;
      *change_req.mutable_from_checkpoint() = checkpoint;
      change_resp.Clear();
      RETURN_NOT_OK(cdc_proxy_->GetChanges(change_req, &change_resp, &rpc));
      if (change_resp.has_error()) return Result<bool>(StatusFromPB(change_resp.error().status()));
      // Expect 1 new record if we replicate intents and 4 if we don't.
      return change_resp.records_size() == (replicate_intents ? 1 : kNumIntentsToWrite + 1);
    }, MonoDelta::FromSeconds(30), "Wait for Transaction to be committed."));
    for (int i = 0; i < change_resp.records_size() - 1; i++) {
      ASSERT_NO_FATALS(CheckIntentRecord(change_resp.records(i), expected_order[i],
                                         replicate_intents));
    }
    ASSERT_NO_FATALS(CheckApplyRecord(change_resp.records(change_resp.records_size() - 1),
                                      replicate_intents));
  }
}

// Only test 'enable_replicate_intents = true'.
class CDCServiceTxnTestEnableReplicateIntents : public CDCServiceTxnTest {
};

INSTANTIATE_TEST_CASE_P(EnableIntentReplication, CDCServiceTxnTestEnableReplicateIntents,
                        ::testing::Values(true /* enable_replicate_intents */));

TEST_P(CDCServiceTxnTestEnableReplicateIntents, MetricsTest) {
  static const int32_t entry_to_add = 100;
  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  ASSERT_RESULT(WriteRow(session, entry_to_add /* key */, entry_to_add /* value */,
                         WriteOpType::INSERT, Flush::kTrue));
  ASSERT_OK(txn->CommitFuture().get());

  // Get tablet ID.
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(client_->GetTablets(table_->name(), 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);

  // Create CDC stream on table.
  CDCStreamId stream_id;
  CreateCDCStream(cdc_proxy_, table_.table()->id(), &stream_id);

  auto tablet_id = tablets.Get(0).tablet_id();

  GetChangesRequestPB change_req;
  GetChangesResponsePB change_resp;

  change_req.set_stream_id(stream_id);
  change_req.set_tablet_id(tablet_id);
  change_req.mutable_from_checkpoint()->mutable_op_id()->set_index(0);
  change_req.mutable_from_checkpoint()->mutable_op_id()->set_term(0);

  // Get CDC changes.
  {
    RpcController rpc;
    SCOPED_TRACE(change_req.DebugString());
    ASSERT_OK(cdc_proxy_->GetChanges(change_req, &change_resp, &rpc));
    SCOPED_TRACE(change_resp.DebugString());
    ASSERT_FALSE(change_resp.has_error());

    ASSERT_EQ(change_resp.records_size(), 2);
  }

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    const auto& tserver = cluster_->mini_tablet_server(0)->server();
    auto cdc_service = dynamic_cast<CDCServiceImpl*>(
        tserver->rpc_server()->service_pool("yb.cdc.CDCService")->TEST_get_service().get());
    auto metrics = cdc_service->GetCDCTabletMetrics({"" /* UUID */, stream_id, tablet_id});
    auto lag = metrics->async_replication_sent_lag_micros->value();
    YB_LOG_EVERY_N_SECS(INFO, 1) << "Sent lag: " << lag << "us";
    // Only check sent lag, since we're just calling GetChanges once and expect committed lag to be
    // greater than 0.
    return lag == 0;
  }, MonoDelta::FromSeconds(10), "Wait for Sent Lag == 0"));


}

} // namespace cdc
} // namespace yb
