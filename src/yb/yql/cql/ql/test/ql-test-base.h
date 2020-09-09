//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_CQL_QL_TEST_QL_TEST_BASE_H_
#define YB_YQL_CQL_QL_TEST_QL_TEST_BASE_H_

#include "yb/yql/cql/ql/ql_processor.h"
#include "yb/yql/cql/ql/util/ql_env.h"

#include "yb/integration-tests/mini_cluster.h"
#include "yb/master/mini_master.h"

#include "yb/util/test_util.h"

namespace yb {
namespace ql {

#define ANALYZE_VALID_STMT(stmt, parse_tree)   \
  do {                                         \
    Status s = TestAnalyzer(stmt, parse_tree); \
    EXPECT_TRUE(s.ok());                       \
  } while (false)

#define ANALYZE_INVALID_STMT(stmt, parse_tree) \
  do {                                         \
    Status s = TestAnalyzer(stmt, parse_tree); \
    EXPECT_FALSE(s.ok());                      \
  } while (false)

#define PARSE_VALID_STMT(stmt)                 \
  do {                                         \
    Status s = TestParser(stmt);               \
    EXPECT_TRUE(s.ok());                       \
  } while (false)

#define PARSE_INVALID_STMT(stmt)               \
  do {                                         \
    Status s = TestParser(stmt);               \
    EXPECT_FALSE(s.ok());                      \
  } while (false)

#define PARSE_INVALID_STMT_ERR(stmt, err_msg)  \
  do {                                         \
    Status s = TestParser(stmt);               \
    EXPECT_FALSE(s.ok());                      \
    EXPECT_FALSE(s.ToString().find(err_msg) == string::npos); \
  } while (false)

#define PROCESSOR_RUN(result, stmt)            \
    LOG(INFO) << "Run: " << stmt;              \
    Status result = processor->Run(stmt);

#define EXEC_VALID_STMT(stmt)                  \
  do {                                         \
    PROCESSOR_RUN(s, stmt);                    \
    EXPECT_TRUE(s.ok());                       \
  } while (false)

#define EXEC_INVALID_STMT_WITH_ERROR(stmt, err_msg)           \
  do {                                                        \
    PROCESSOR_RUN(s, stmt);                                   \
    EXPECT_FALSE(s.ok());                                     \
    EXPECT_FALSE(s.ToString().find(err_msg) == string::npos); \
  } while (false)

#define EXEC_INVALID_STMT(stmt) EXEC_INVALID_STMT_WITH_ERROR(stmt, "")

#define CHECK_VALID_STMT(stmt)                 \
  do {                                         \
    PROCESSOR_RUN(s, stmt);                    \
    CHECK(s.ok()) << "Failure: " << s;         \
  } while (false)

#define CHECK_INVALID_STMT(stmt)               \
  do {                                         \
    PROCESSOR_RUN(s, stmt);                    \
    CHECK(!s.ok()) << "Expect failure";        \
  } while (false)

class ClockHolder {
 protected:
  ClockHolder() : clock_(new server::HybridClock()) {
    CHECK_OK(clock_->Init());
  }

  server::ClockPtr clock_;
};

class TestQLProcessor : public ClockHolder, public QLProcessor {
 public:
  // Public types.
  typedef std::unique_ptr<TestQLProcessor> UniPtr;
  typedef std::unique_ptr<const TestQLProcessor> UniPtrConst;

  // Constructors.
  TestQLProcessor(client::YBClient* client,
                  std::shared_ptr<client::YBMetaDataCache> cache,
                  const RoleName& role_name)
      : QLProcessor(client, cache, nullptr /* ql_metrics */, nullptr /* parser_pool */, clock_,
                    TransactionPoolProvider()) {
    if (!role_name.empty()) {
      ql_env_.ql_session()->set_current_role_name(role_name);
    }
  }
  virtual ~TestQLProcessor() { }

  void RunAsyncDone(
      Callback<void(const Status&)> cb, const Status& s,
      const ExecutedResult::SharedPtr& result = nullptr) {
    result_ = result;
    cb.Run(s);
  }

  void RunAsync(
      const string& stmt, const StatementParameters& params, Callback<void(const Status&)> cb) {
    result_ = nullptr;
    parse_tree.reset(); // Delete previous parse tree.
    RunAsyncInternal(stmt, params, Bind(&TestQLProcessor::RunAsyncDone, Unretained(this), cb));
  }

  // Execute a QL statement.
  CHECKED_STATUS Run(
      const std::string& stmt, const StatementParameters& params = StatementParameters()) {
    Synchronizer s;
    RunAsync(stmt, params, Bind(&Synchronizer::StatusCB, Unretained(&s)));
    return s.Wait();
  }

  // Construct a row_block and send it back.
  std::shared_ptr<QLRowBlock> row_block() const {
    LOG(INFO) << (result_ == NULL ? "Result is NULL." : "Got result.")
              << " Return type = " << static_cast<int>(ExecutedResult::Type::ROWS);
    if (result_ != nullptr && result_->type() == ExecutedResult::Type::ROWS) {
      return std::shared_ptr<QLRowBlock>(static_cast<RowsResult*>(result_.get())->GetRowBlock());
    }
    return nullptr;
  }

  const ExecutedResult::SharedPtr& result() const { return result_; }

  const RowsResult* rows_result() const {
    if (result_ != nullptr && result_->type() == ExecutedResult::Type::ROWS) {
      return static_cast<const RowsResult*>(result_.get());
    }
    return nullptr;
  }

  std::string CurrentKeyspace() const { return ql_env_.CurrentKeyspace(); }

  CHECKED_STATUS UseKeyspace(const std::string& keyspace_name) {
    return ql_env_.UseKeyspace(keyspace_name);
  }

  void RemoveCachedTableDesc(const client::YBTableName& table_name) {
    ql_env_.RemoveCachedTableDesc(table_name);
  }

  const ParseTree::UniPtr& GetLastParseTree() const {
    return parse_tree;
  }

 private:
  void RunAsyncInternal(const std::string& stmt, const StatementParameters& params,
                        StatementExecutedCallback cb, bool reparsed = false) {
    const Status s = Prepare(stmt, &parse_tree, reparsed);
    if (PREDICT_FALSE(!s.ok())) {
      return cb.Run(s, nullptr /* result */);
    }
    // Do not make a copy of stmt and params when binding to the RunAsyncDone callback because when
    // error occurs due to stale matadata, the statement needs to be reexecuted. We should pass the
    // original references which are guaranteed to still be alive when the statement is reexecuted.
    ExecuteAsync(*parse_tree, params, Bind(&QLProcessor::RunAsyncDone, Unretained(this),
        ConstRef(stmt), ConstRef(params), Unretained(parse_tree.get()), cb));
  }

  // Execute result.
  ExecutedResult::SharedPtr result_;

  ParseTree::UniPtr parse_tree;
};

// Base class for all QL test cases.
class QLTestBase : public YBTest {
 public:
  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  QLTestBase();
  ~QLTestBase();

  //------------------------------------------------------------------------------------------------
  // Test start and cleanup functions.
  virtual void SetUp() override {
    YBTest::SetUp();
  }

  virtual void TearDown() override {
    client_.reset();
    if (cluster_ != nullptr) {
      cluster_->Shutdown();
    }
    YBTest::TearDown();
  }

  //------------------------------------------------------------------------------------------------
  // Test only the parser.
  CHECKED_STATUS TestParser(const std::string& stmt) {
    QLProcessor* processor = GetQLProcessor();
    ParseTree::UniPtr parse_tree;
    return processor->Parse(stmt, &parse_tree);
  }

  // Tests parser and analyzer
  CHECKED_STATUS TestAnalyzer(const string& stmt, ParseTree::UniPtr* parse_tree) {
    QLProcessor* processor = GetQLProcessor();
    RETURN_NOT_OK(processor->Parse(stmt, parse_tree));
    RETURN_NOT_OK(processor->Analyze(parse_tree));
    return Status::OK();
  }

  //------------------------------------------------------------------------------------------------
  // Create simulated cluster.
  void CreateSimulatedCluster(int num_tablet_servers = 1);

  // Create ql processor.
  TestQLProcessor* GetQLProcessor(const RoleName& role_name = "");


  //------------------------------------------------------------------------------------------------
  // Utility functions for QL tests.

  void VerifyPaginationSelect(TestQLProcessor* processor,
                              const string &select_query,
                              int page_size,
                              const string expected_rows) {
    StatementParameters params;
    params.set_page_size(page_size);
    string rows;
    do {
      CHECK_OK(processor->Run(select_query, params));
      std::shared_ptr<QLRowBlock> row_block = processor->row_block();
      if (row_block->row_count() > 0) {
        rows.append(row_block->ToString());
      } else {
        // Skip appending empty rowblock but verify it happens only at the last fetch.
        EXPECT_TRUE(processor->rows_result()->paging_state().empty());
      }
      if (processor->rows_result()->paging_state().empty()) {
        break;
      }
      CHECK_OK(params.SetPagingState(processor->rows_result()->paging_state()));
    } while (true);
    EXPECT_EQ(expected_rows, rows);
  }

 protected:
  //------------------------------------------------------------------------------------------------

  // Simulated cluster.
  std::shared_ptr<MiniCluster> cluster_;

  // Simulated YB client.
  std::unique_ptr<client::YBClient> client_;
  std::shared_ptr<client::YBMetaDataCache> metadata_cache_;

  // QL Processor.
  std::vector<TestQLProcessor::UniPtr> ql_processors_;

  static const std::string kDefaultKeyspaceName;
};

}  // namespace ql
}  // namespace yb

#endif  // YB_YQL_CQL_QL_TEST_QL_TEST_BASE_H_
