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
#ifndef YB_INTEGRATION_TESTS_TEST_WORKLOAD_H_
#define YB_INTEGRATION_TESTS_TEST_WORKLOAD_H_

#include "yb/client/client.h"
#include "yb/client/table.h"

namespace yb {

class MiniClusterBase;
class Thread;

struct TestWorkloadOptions {
  static const client::YBTableName kDefaultTableName;

  int payload_bytes = 11;
  int num_write_threads = 4;
  int num_read_threads = 0;
  int write_batch_size = 50;
  int write_interval_millis = 0;
  int ttl = -1;
  MonoDelta default_rpc_timeout = std::chrono::seconds(60);
  std::chrono::milliseconds write_timeout = std::chrono::seconds(20);
  bool timeout_allowed = false;
  bool not_found_allowed = false;
  bool pathological_one_row_enabled = false;
  bool sequential_write = false;
  bool insert_failures_allowed = true;
  bool retry_on_restart_required_error = false;
  bool read_only_written_keys = false;
  IsolationLevel isolation_level = IsolationLevel::NON_TRANSACTIONAL;

  int num_tablets = 1;
  client::YBTableName table_name = kDefaultTableName;

  bool is_transactional() const { return isolation_level != IsolationLevel::NON_TRANSACTIONAL; }
};

// Utility class for generating a workload against a test cluster.
//
// The actual data inserted is random, and thus can't be verified for
// integrity. However, this is still useful in conjunction with ClusterVerifier
// to verify that replicas do not diverge.
class TestWorkload {
 public:
  explicit TestWorkload(MiniClusterBase* cluster);
  ~TestWorkload();

  TestWorkload(TestWorkload&& rhs);

  void operator=(TestWorkload&& rhs);

  void set_payload_bytes(int n) {
    options_.payload_bytes = n;
  }

  void set_num_read_threads(int n) {
    options_.num_read_threads = n;
  }

  void set_num_write_threads(int n) {
    options_.num_write_threads = n;
  }

  void set_write_batch_size(int s) {
    options_.write_batch_size = s;
  }

  void set_write_interval_millis(int t) {
    options_.write_interval_millis = t;
  }

  void set_ttl(int ttl) {
    options_.ttl = ttl;
  }

  void set_client_default_rpc_timeout_millis(int t) {
    options_.default_rpc_timeout = MonoDelta::FromMilliseconds(t);
  }

  void set_write_timeout(std::chrono::milliseconds value) {
    options_.write_timeout = value;
  }

  void set_write_timeout_millis(int t) {
    options_.write_timeout = std::chrono::milliseconds(t);
  }

  // Set whether to fail if we see a TimedOut() error inserting a row.
  // By default, this triggers a CHECK failure.
  void set_timeout_allowed(bool allowed) {
    options_.timeout_allowed = allowed;
  }

  // Set whether to fail if we see a NotFound() error inserting a row.
  // This sort of error is triggered if the table is deleted while the workload
  // is running.
  // By default, this triggers a CHECK failure.
  void set_not_found_allowed(bool allowed) {
    options_.not_found_allowed = allowed;
  }

  // Set the number of tablets for the table created by this workload.
  // The split points are evenly distributed through positive int32s.
  void set_num_tablets(int tablets) {
    CHECK_GE(tablets, 1);
    options_.num_tablets = tablets;
  }

  void set_table_name(const client::YBTableName& table_name) {
    options_.table_name = table_name;
  }

  const client::YBTableName& table_name() const {
    return options_.table_name;
  }

  client::YBClient& client() const;

  void set_pathological_one_row_enabled(bool enabled) {
    options_.pathological_one_row_enabled = enabled;
  }

  void set_sequential_write(bool value) {
    options_.sequential_write = value;
  }

  void set_insert_failures_allowed(bool value) {
    options_.insert_failures_allowed = value;
  }

  void set_retry_on_restart_required_error(const bool value) {
    options_.retry_on_restart_required_error = value;
  }

  // Only read keys which have been successfully written.
  // REQUIRED: works only when sequential_write is set and we have write threads to generate keys
  // to read.
  void set_read_only_written_keys(const bool value) {
    options_.read_only_written_keys = value;
  }

  void set_transactional(IsolationLevel isolation_level, client::TransactionPool* pool);

  // Sets up the internal client and creates the table which will be used for
  // writing, if it doesn't already exist.
  void Setup(client::YBTableType table_type = client::YBTableType::YQL_TABLE_TYPE);

  // Start the write workload.
  void Start();

  // Stop the writers and wait for them to exit.
  void StopAndJoin();

  void Stop();

  void Join();

  void WaitInserted(int64_t required);

  // Return the number of rows inserted so far. This may be called either
  // during or after the write workload.
  int64_t rows_inserted() const;

  int64_t rows_insert_failed() const;

  int64_t rows_read_ok() const;

  int64_t rows_read_empty() const;

  int64_t rows_read_error() const;

  int64_t rows_read_try_again() const;

  // Return the number of batches in which we have successfully inserted at
  // least one row.
  int64_t batches_completed() const;

 private:
  class State;

  TestWorkloadOptions options_;
  std::unique_ptr<State> state_;
};

}  // namespace yb
#endif  // YB_INTEGRATION_TESTS_TEST_WORKLOAD_H_
