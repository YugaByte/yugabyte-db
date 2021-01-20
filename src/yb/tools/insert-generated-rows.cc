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
// Simple tool to insert "random junk" rows into an arbitrary table.
// First column is in ascending order, the rest are random data.
// Helps make things like availability demos a little easier.

#include <iostream>
#include <memory>
#include <vector>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/client/client.h"
#include "yb/client/error.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"

#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/tools/data_gen_util.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"

using namespace std::literals;

DEFINE_string(master_address, "localhost",
              "Comma separated list of master addresses to run against.");

namespace yb {
namespace tools {

using std::string;
using std::vector;

using client::YBClient;
using client::YBClientBuilder;
using client::YBColumnSchema;
using client::YBSchema;
using client::YBSession;
using client::YBTable;
using client::YBTableName;
using std::shared_ptr;

void PrintUsage(char** argv) {
  std::cerr << "usage: " << argv[0] << " [--master_address localhost] <table_name>"
            << std::endl;
}

static int WriteRandomDataToTable(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 2) {
    PrintUsage(argv);
    return 1;
  }
  InitGoogleLoggingSafe(argv[0]);
  FLAGS_logtostderr = true;

  YBTableName table_name(YQL_DATABASE_CQL, argv[1]); // Default namespace.

  vector<string> addrs = strings::Split(FLAGS_master_address, ",");
  CHECK(!addrs.empty()) << "At least one master address must be specified!";

  // Set up client.
  LOG(INFO) << "Connecting to YB Master...";
  auto client = CHECK_RESULT(YBClientBuilder()
      .master_server_addrs(addrs)
      .Build());

  LOG(INFO) << "Opening table...";
  client::TableHandle table;
  CHECK_OK(table.Open(table_name, client.get()));
  YBSchema schema = table->schema();

  shared_ptr<YBSession> session = client->NewSession();
  session->SetTimeout(5s); // Time out after 5 seconds.

  Random random(GetRandomSeed32());

  LOG(INFO) << "Inserting random rows...";
  for (uint64_t record_id = 0; true; ++record_id) {
    auto insert = table.NewInsertOp();
    auto req = insert->mutable_request();
    GenerateDataForRow(schema, record_id, &random, req);

    LOG(INFO) << "Inserting record: " << req->ShortDebugString();
    CHECK_OK(session->Apply(insert));
    Status s = session->Flush();
    if (PREDICT_FALSE(!s.ok())) {
      for (const auto& e : session->GetAndClearPendingErrors()) {
        if (e->status().IsAlreadyPresent()) {
          LOG(WARNING) << "Ignoring insert error: " << e->status().ToString();
        } else {
          LOG(FATAL) << "Unexpected insert error: " << e->status().ToString();
        }
      }
      continue;
    }
    LOG(INFO) << "OK";
  }

  return 0;
}

}  // namespace tools
}  // namespace yb

int main(int argc, char** argv) {
  return yb::tools::WriteRandomDataToTable(argc, argv);
}
