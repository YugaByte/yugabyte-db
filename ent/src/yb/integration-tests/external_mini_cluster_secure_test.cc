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

#include "yb/client/ql-dml-test-base.h"
#include "yb/client/table_handle.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/secure_stream.h"

#include "yb/server/secure.h"

#include "yb/util/size_literals.h"
#include "yb/util/env_util.h"

#include "yb/yql/cql/ql/util/errcodes.h"
#include "yb/yql/cql/ql/util/statement_result.h"

#include "yb/integration-tests/external_mini_cluster_ent.h"

DECLARE_bool(use_client_to_server_encryption);
DECLARE_bool(use_node_to_node_encryption);
DECLARE_bool(allow_insecure_connections);
DECLARE_string(certs_dir);

namespace yb {

class ExternalMiniClusterSecureTest :
    public MiniClusterTestWithClient<ExternalMiniCluster> {
 public:
  void SetUp() override {
    FLAGS_use_node_to_node_encryption = true;
    FLAGS_use_client_to_server_encryption = true;
    FLAGS_allow_insecure_connections = false;
    const auto sub_dir = JoinPathSegments("ent", "test_certs");
    FLAGS_certs_dir = JoinPathSegments(env_util::GetRootDir(sub_dir), sub_dir);

    MiniClusterTestWithClient::SetUp();

    ASSERT_NO_FATALS(StartSecure(&cluster_, &secure_context_, &messenger_));

    ASSERT_OK(CreateClient());

    DontVerifyClusterBeforeNextTearDown(); // Verify requires insecure connection.
  }

  void DoTearDown() override {
    messenger_->Shutdown();
    MiniClusterTestWithClient::DoTearDown();
  }

  CHECKED_STATUS CreateClient() override {
    return cluster_->CreateClient(messenger_.get()).MoveTo(&client_);
  }

  std::unique_ptr<rpc::SecureContext> secure_context_;
  std::unique_ptr<rpc::Messenger> messenger_;
  client::TableHandle table_;
};

TEST_F(ExternalMiniClusterSecureTest, Simple) {
  client::kv_table_test::CreateTable(
      client::Transactional::kFalse, CalcNumTablets(3), client_.get(), &table_);

  const int32_t kKey = 1;
  const int32_t kValue = 2;

  {
    auto session = NewSession();
    auto op = ASSERT_RESULT(client::kv_table_test::WriteRow(
        &table_, session, kKey, kValue));
    ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    auto value = ASSERT_RESULT(client::kv_table_test::SelectRow(
        &table_, NewSession(), kKey));
    ASSERT_EQ(kValue, value);
  }
}

} // namespace yb
