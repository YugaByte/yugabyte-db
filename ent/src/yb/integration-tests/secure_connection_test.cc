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
#include "yb/client/session.h"
#include "yb/client/table_handle.h"

#include "yb/common/ql_value.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/secure_stream.h"

#include "yb/server/secure.h"

#include "yb/util/size_literals.h"
#include "yb/util/env_util.h"

#include "yb/yql/cql/ql/util/errcodes.h"
#include "yb/yql/cql/ql/util/statement_result.h"

DECLARE_bool(use_client_to_server_encryption);
DECLARE_bool(use_node_to_node_encryption);
DECLARE_bool(allow_insecure_connections);
DECLARE_string(certs_dir);
DECLARE_string(TEST_public_hostname_suffix);

namespace yb {

class SecureConnectionTest : public client::KeyValueTableTest {
 public:
  SecureConnectionTest() {
  }

  void SetUp() override {
    FLAGS_use_node_to_node_encryption = true;
    FLAGS_use_client_to_server_encryption = true;
    FLAGS_allow_insecure_connections = false;
    FLAGS_TEST_public_hostname_suffix = ".ip.yugabyte";
    const auto sub_dir = JoinPathSegments("ent", "test_certs");
    auto root_dir = env_util::GetRootDir(sub_dir);
    FLAGS_certs_dir = JoinPathSegments(root_dir, sub_dir);

    client::QLDmlTestBase::SetUp();

    DontVerifyClusterBeforeNextTearDown(); // Verify requires insecure connection.
  }

  CHECKED_STATUS CreateClient() override {
    rpc::MessengerBuilder messenger_builder("test_client");
    secure_context_ = VERIFY_RESULT(server::SetupSecureContext(
        "", "", server::SecureContextType::kClientToServer, &messenger_builder));
    auto messenger = VERIFY_RESULT(messenger_builder.Build());
    messenger->TEST_SetOutboundIpBase(VERIFY_RESULT(HostToAddress("127.0.0.1")));
    client_ = VERIFY_RESULT(cluster_->CreateClient(std::move(messenger)));
    return Status::OK();
  }

  std::unique_ptr<rpc::SecureContext> secure_context_;
};

TEST_F(SecureConnectionTest, Simple) {
  CreateTable(client::Transactional::kFalse);

  const int32_t kKey = 1;
  const int32_t kValue = 2;

  {
    auto session = NewSession();
    auto op = ASSERT_RESULT(WriteRow(session, kKey, kValue));
    ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    auto value = ASSERT_RESULT(SelectRow(NewSession(), kKey));
    ASSERT_EQ(kValue, value);
  }
}

TEST_F(SecureConnectionTest, BigWrite) {
  client::YBSchemaBuilder builder;
  builder.AddColumn(kKeyColumn)->Type(INT32)->HashPrimaryKey()->NotNull();
  builder.AddColumn(kValueColumn)->Type(STRING);

  ASSERT_OK(table_.Create(client::kTableName, 1, client_.get(), &builder));

  const int32_t kKey = 1;
  const std::string kValue(64_KB, 'X');

  auto session = NewSession();
  {
    const auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, kKey);
    table_.AddStringColumnValue(req, kValueColumn, kValue);
    ASSERT_OK(session->ApplyAndFlush(op));
    ASSERT_OK(CheckOp(op.get()));
  }

  {
    const auto op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, kKey);
    table_.AddColumns({kValueColumn}, req);
    ASSERT_OK(session->ApplyAndFlush(op));
    ASSERT_OK(CheckOp(op.get()));
    auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
    ASSERT_EQ(rowblock->row_count(), 1);
    ASSERT_EQ(kValue, rowblock->row(0).column(0).string_value());
  }
}

} // namespace yb
