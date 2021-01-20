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


#include <algorithm>
#include <map>
#include <vector>

#include "yb/client/client.h"
#include "yb/client/client-test-util.h"
#include "yb/client/error.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"

#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/test_util.h"

using namespace std::literals;

namespace yb {

using client::YBClient;
using client::YBClientBuilder;
using client::YBTableType;
using client::YBColumnSchema;
using client::YBError;
using client::YBqlWriteOp;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBSession;
using client::YBTable;
using client::YBTableAlterer;
using client::YBTableCreator;
using client::YBTableName;
using client::YBValue;
using std::shared_ptr;
using std::make_pair;
using std::map;
using std::pair;
using std::vector;
using strings::SubstituteAndAppend;

static const YBTableName kTableName(YQL_DATABASE_CQL, "my_keyspace", "test-table");
static const int kMaxColumns = 30;

class AlterTableRandomized : public YBTest {
 public:
  void SetUp() override {
    YBTest::SetUp();

    ExternalMiniClusterOptions opts;
    opts.num_tablet_servers = 3;
    // Because this test performs a lot of alter tables, we end up flushing
    // and rewriting metadata files quite a bit. Globally disabling fsync
    // speeds the test runtime up dramatically.
    opts.extra_tserver_flags.push_back("--never_fsync");
    cluster_.reset(new ExternalMiniCluster(opts));
    ASSERT_OK(cluster_->Start());

    client_ = ASSERT_RESULT(cluster_->CreateClient());
  }

  void TearDown() override {
    client_.reset();
    cluster_->Shutdown();
    YBTest::TearDown();
  }

  void RestartTabletServer(int idx) {
    LOG(INFO) << "Restarting TS " << idx;
    cluster_->tablet_server(idx)->Shutdown();
    CHECK_OK(cluster_->tablet_server(idx)->Restart());
    CHECK_OK(cluster_->WaitForTabletsRunning(cluster_->tablet_server(idx),
        MonoDelta::FromSeconds(60)));
  }

 protected:
  gscoped_ptr<ExternalMiniCluster> cluster_;
  std::unique_ptr<YBClient> client_;
};

typedef std::vector<std::pair<std::string, int32_t>> Row;

// We use this special value to denote NULL values.
// We ensure that we never insert or update to this value except in the case of NULLable columns.
const int32_t kNullValue = 0xdeadbeef;

std::string RowToString(const Row& row) {
  string ret = "{ ";
  bool first = true;
  for (const auto& e : row) {
    if (!first) {
      ret.append(", ");
    }
    first = false;
    if (e.second == kNullValue) {
      ret += "null";
    } else {
      SubstituteAndAppend(&ret, "int32:$0", e.second);
    }
  }
  ret += " }";

  return ret;
}

struct TableState {
  TableState() {
    col_names_.push_back("key");
    col_nullable_.push_back(false);
  }

  void GenRandomRow(int32_t key, int32_t seed,
                    vector<pair<string, int32_t>>* row) {
    if (seed == kNullValue) {
      seed++;
    }
    row->clear();
    row->push_back(make_pair("key", key));
    for (int i = 1; i < col_names_.size(); i++) {
      int32_t val;
      if (col_nullable_[i] && seed % 2 == 1) {
        val = kNullValue;
      } else {
        val = seed;
      }
      row->push_back(make_pair(col_names_[i], val));
    }
  }

  bool Insert(const Row& data) {
    DCHECK_EQ("key", data[0].first);
    int32_t key = data[0].second;

    return rows_.emplace(key, data).second;
  }

  bool Update(const vector<pair<string, int32_t>>& data) {
    DCHECK_EQ("key", data[0].first);
    int32_t key = data[0].second;
    auto it = rows_.find(key);
    if (it == rows_.end()) return false;

    it->second = data;
    return true;
  }

  void Delete(int32_t row_key) {
    CHECK(rows_.erase(row_key)) << "row key " << row_key << " not found";
  }

  void AddColumnWithDefault(const string& name, int32_t def, bool nullable) {
    col_names_.push_back(name);
    col_nullable_.push_back(nullable);
    for (auto& e : rows_) {
      e.second.emplace_back(name, def);
    }
  }

  void DropColumn(const string& name) {
    auto col_it = std::find(col_names_.begin(), col_names_.end(), name);
    int index = col_it - col_names_.begin();
    col_names_.erase(col_it);
    col_nullable_.erase(col_nullable_.begin() + index);
    for (auto& e : rows_) {
      e.second.erase(e.second.begin() + index);
    }
  }

  int32_t GetRandomRowKey(int32_t rand) {
    CHECK(!rows_.empty());
    auto it = rows_.begin();
    std::advance(it, rand % rows_.size());
    return it->first;
  }

  void ToStrings(vector<string>* strs) {
    strs->clear();
    for (const auto& e : rows_) {
      strs->push_back(RowToString(e.second));
    }
  }

  // The name of each column.
  vector<string> col_names_;

  // For each column, whether it is NULLable.
  // Has the same length as col_names_.
  vector<bool> col_nullable_;

  std::map<int32_t, Row> rows_;
};

struct MirrorTable {
  explicit MirrorTable(client::YBClient* client)
      : client_(client) {}

  Status Create() {
    RETURN_NOT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                      kTableName.namespace_type()));
    YBSchema schema;
    YBSchemaBuilder b;
    b.AddColumn("key")->Type(INT32)->HashPrimaryKey()->NotNull();
    CHECK_OK(b.Build(&schema));
    std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    return table_creator->table_name(kTableName)
        .schema(&schema)
        .Create();
  }

  bool TryInsert(int32_t row_key, int32_t rand) {
    vector<pair<string, int32_t>> row;
    ts_.GenRandomRow(row_key, rand, &row);
    return TryInsert(row);
  }

  bool TryInsert(const Row& row) {
    if (!ts_.Insert(row)) {
      return false;
    }

    Status s = DoRealOp(row, INSERT);
    CHECK_OK(s);

    return true;
  }

  void DeleteRandomRow(uint32_t rand) {
    if (ts_.rows_.empty()) return;
    DeleteRow(ts_.GetRandomRowKey(rand));
  }

  void DeleteRow(int32_t row_key) {
    Row del = {{"key", row_key}};
    ts_.Delete(row_key);
    CHECK_OK(DoRealOp(del, DELETE));
  }

  void UpdateRandomRow(uint32_t rand) {
    if (ts_.rows_.empty()) return;
    int32_t row_key = ts_.GetRandomRowKey(rand);

    vector<pair<string, int32_t>> update;
    update.push_back(make_pair("key", row_key));
    for (int i = 1; i < num_columns(); i++) {
      int32_t val = rand * i;
      if (val == kNullValue) val++;
      if (ts_.col_nullable_[i] && val % 2 == 1) {
        val = kNullValue;
      }
      update.push_back(make_pair(ts_.col_names_[i], val));
    }

    if (update.size() == 1) {
      // No columns got updated. Just ignore this update.
      return;
    }

    Status s = DoRealOp(update, UPDATE);
    if (s.IsNotFound()) {
      CHECK(!ts_.Update(update)) << "real table said not-found, fake table succeeded";
      return;
    }
    CHECK_OK(s);

    CHECK(ts_.Update(update));
  }

  void AddAColumn(const string& name) {
    bool nullable = random() % 2 == 1;
    return AddAColumn(name, nullable);
  }

  void AddAColumn(const string& name, bool nullable) {
    // Add to the real table.
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));

    table_alterer->AddColumn(name)->Type(INT32);
    ASSERT_OK(table_alterer->Alter());

    // Add to the mirror state.
    ts_.AddColumnWithDefault(name, kNullValue, nullable);
  }

  void DropAColumn(const string& name) {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    CHECK_OK(table_alterer->DropColumn(name)->Alter());
    ts_.DropColumn(name);
  }

  void DropRandomColumn(int seed) {
    if (num_columns() == 1) return;

    string name = ts_.col_names_[1 + (seed % (num_columns() - 1))];
    DropAColumn(name);
  }

  int num_columns() const {
    return ts_.col_names_.size();
  }

  void Verify() {
    // First scan the real table
    vector<string> rows;
    {
      client::TableHandle table;
      CHECK_OK(table.Open(kTableName, client_));
      client::ScanTableToStrings(table, &rows);
    }
    std::sort(rows.begin(), rows.end());

    // Then get our mock table.
    vector<string> expected;
    ts_.ToStrings(&expected);

    // They should look the same.
    LogVectorDiff(expected, rows);
    ASSERT_EQ(expected, rows);
  }

 private:
  enum OpType {
    INSERT, UPDATE, DELETE
  };

  Status DoRealOp(const vector<pair<string, int32_t>>& data, OpType op_type) {
    auto deadline = MonoTime::Now() + 15s;
    shared_ptr<YBSession> session = client_->NewSession();
    session->SetTimeout(15s);
    shared_ptr<YBTable> table;
    RETURN_NOT_OK(client_->OpenTable(kTableName, &table));
    for (;;) {
      auto op = CreateOp(table, op_type);
      auto* const req = op->mutable_request();
      bool first = true;
      auto schema = table->schema();
      for (const auto& d : data) {
        if (first) {
          req->add_hashed_column_values()->mutable_value()->set_int32_value(d.second);
          first = false;
          continue;
        }
        auto column_value = req->add_column_values();
        for (size_t i = 0; i < schema.num_columns(); ++i) {
          if (schema.Column(i).name() == d.first) {
            column_value->set_column_id(schema.ColumnId(i));
            auto value = column_value->mutable_expr()->mutable_value();
            if (d.second != kNullValue) {
              value->set_int32_value(d.second);
            }
            break;
          }
        }
      }
      auto s = session->ApplyAndFlush(op);
      if (s.ok()) {
        if (op->response().status() == QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH &&
            MonoTime::Now() < deadline) {
          continue;
        }
        CHECK(op->succeeded()) << op->response().ShortDebugString();
        return s;
      }

      client::CollectedErrors errors = session->GetAndClearPendingErrors();
      CHECK_EQ(errors.size(), 1);
      return errors[0]->status();
    }
  }

  shared_ptr<YBqlWriteOp> CreateOp(const shared_ptr<YBTable>& table, OpType op_type) {
    switch (op_type) {
      case INSERT:
        return shared_ptr<YBqlWriteOp>(table->NewQLInsert());
      case UPDATE:
        return shared_ptr<YBqlWriteOp>(table->NewQLUpdate());
      case DELETE:
        return shared_ptr<YBqlWriteOp>(table->NewQLDelete());
    }
    return shared_ptr<YBqlWriteOp>();
  }

  YBClient* client_;
  TableState ts_;
};

// Stress test for various alter table scenarios. This performs a random sequence of:
//   - insert a row (using the latest schema)
//   - delete a random row
//   - update a row (all columns with the latest schema)
//   - add a new column
//   - drop a column
//   - restart the tablet server
//
// During the sequence of operations, a "mirror" of the table in memory is kept up to
// date. We periodically scan the actual table, and ensure that the data in YB
// matches our in-memory "mirror".
TEST_F(AlterTableRandomized, TestRandomSequence) {
  MirrorTable t(client_.get());
  ASSERT_OK(t.Create());

  Random rng(SeedRandom());

  const int n_iters = AllowSlowTests() ? 2000 : 1000;
  for (int i = 0; i < n_iters; i++) {
    // Perform different operations with varying probability.
    // We mostly insert and update, with occasional deletes,
    // and more occasional table alterations or restarts.
    int r = rng.Uniform(1000);
    if (r < 400) {
      bool inserted = t.TryInsert(1000000 + rng.Uniform(1000000), rng.Next());
      if (!inserted) {
        continue;
      }
    } else if (r < 600) {
      t.UpdateRandomRow(rng.Next());
    } else if (r < 920) {
      t.DeleteRandomRow(rng.Next());
    } else if (r < 970) {
      if (t.num_columns() < kMaxColumns) {
        t.AddAColumn(strings::Substitute("c$0", i));
      }
    } else if (r < 995) {
      t.DropRandomColumn(rng.Next());
    } else {
      RestartTabletServer(rng.Uniform(cluster_->num_tablet_servers()));
    }

    if (i % 1000 == 0) {
      LOG(INFO) << "Verifying iteration " << i;
      ASSERT_NO_FATALS(t.Verify());
      LOG(INFO) << "Verification of iteration " << i << " successful";
    }
  }

  LOG(INFO) << "About to do the last verification";
  ASSERT_NO_FATALS(t.Verify());
  LOG(INFO) << "Last verification succeeded";

  // Not only should the data returned by a scanner match what we expect,
  // we also expect all of the replicas to agree with each other.
  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
}

TEST_F(AlterTableRandomized, AddDropRestart) {
  MirrorTable t(client_.get());
  ASSERT_OK(t.Create());

  t.AddAColumn("value", true);
  for (int key = 1; key != 20; ++key) {
    LOG(INFO) << "================================================================================";
    auto column = Format("c_$0", key);
    t.AddAColumn(column, true);
    t.TryInsert({{ "key", key }, { "value", 666 }, { column, 1111 }});
    t.DropAColumn(column);
    RestartTabletServer(0);
    t.DeleteRow(key);
    ASSERT_NO_FATALS(t.Verify());
  }
}

}  // namespace yb
