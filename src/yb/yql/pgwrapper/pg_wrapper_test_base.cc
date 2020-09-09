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

#include "yb/yql/pgwrapper/pg_wrapper_test_base.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

#include "yb/util/env_util.h"
#include "yb/util/path_util.h"
#include "yb/util/tostring.h"

using std::unique_ptr;

using yb::util::TrimStr;
using yb::util::TrimTrailingWhitespaceFromEveryLine;
using yb::util::LeftShiftTextBlock;

namespace yb {
namespace pgwrapper {

void PgWrapperTestBase::SetUp() {
  YBMiniClusterTestBase::SetUp();

  ExternalMiniClusterOptions opts;
  opts.enable_ysql = true;

  // With ysql_num_shards_per_tserver=1 and 3 tservers we'll be creating 3 tablets per table, which
  // is enough for most tests.
  opts.extra_tserver_flags.emplace_back("--ysql_num_shards_per_tserver=1");

  // Collect old records very aggressively to catch bugs with old readpoints.
  opts.extra_tserver_flags.emplace_back("--timestamp_history_retention_interval_sec=0");

  opts.extra_master_flags.emplace_back("--hide_pg_catalog_table_creation_logs");

  opts.num_masters = GetNumMasters();

  opts.num_tablet_servers = GetNumTabletServers();

  opts.extra_master_flags.emplace_back("--client_read_write_timeout_ms=120000");
  opts.extra_master_flags.emplace_back(Format("--memory_limit_hard_bytes=$0", 2_GB));

  UpdateMiniClusterOptions(&opts);

  cluster_.reset(new ExternalMiniCluster(opts));
  ASSERT_OK(cluster_->Start());

  if (cluster_->num_tablet_servers() > 0) {
    pg_ts = cluster_->tablet_server(0);
  }

  // TODO: fix cluster verification for PostgreSQL tables.
  DontVerifyClusterBeforeNextTearDown();
}

namespace {

string TrimSqlOutput(string output) {
  return TrimStr(TrimTrailingWhitespaceFromEveryLine(LeftShiftTextBlock(output)));
}

string CertsDir() {
  const auto sub_dir = JoinPathSegments("ent", "test_certs");
  return JoinPathSegments(env_util::GetRootDir(sub_dir), sub_dir);
}

} // namespace

void PgCommandTestBase::RunPsqlCommand(const string &statement, const string &expected_output) {
  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));

  unique_ptr<WritableFile> tmp_file;
  string tmp_file_name;
  ASSERT_OK(
      Env::Default()->NewTempWritableFile(
          WritableFileOptions(),
          tmp_dir + "/psql_statementXXXXXX",
          &tmp_file_name,
          &tmp_file));
  ASSERT_OK(tmp_file->Append(statement));
  ASSERT_OK(tmp_file->Close());

  vector<string> argv{
      GetPostgresInstallRoot() + "/bin/ysqlsh",
      "-h", pg_ts->bind_host(),
      "-p", std::to_string(pg_ts->pgsql_rpc_port()),
      "-U", "yugabyte",
      "-f", tmp_file_name
  };

  if (!db_name_.empty()) {
    argv.push_back("-d");
    argv.push_back(db_name_);
  }

  if (encrypt_connection_) {
    argv.push_back(Format(
        "sslmode=require sslcert=$0/ysql.crt sslrootcert=$0/ca.crt sslkey=$0/ysql.key",
        CertsDir()));
  }

  LOG(INFO) << "Run tool: " << yb::ToString(argv);
  Subprocess proc(argv.front(), argv);
  if (use_auth_) {
    proc.SetEnv("PGPASSWORD", "yugabyte");
  }

  string psql_stdout;
  LOG(INFO) << "Executing statement: " << statement;
  ASSERT_OK(proc.Call(&psql_stdout));
  LOG(INFO) << "Output from statement {{ " << statement << " }}:\n"
            << psql_stdout;
  ASSERT_EQ(TrimSqlOutput(expected_output), TrimSqlOutput(psql_stdout));
}

void PgCommandTestBase::UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) {
  PgWrapperTestBase::UpdateMiniClusterOptions(options);
  if (encrypt_connection_) {
    const vector<string> common_flags{"--use_node_to_node_encryption=true",
                                      "--certs_dir=" + CertsDir()};
    for (auto flags : {&options->extra_master_flags, &options->extra_tserver_flags}) {
      flags->insert(flags->begin(), common_flags.begin(), common_flags.end());
    }
    options->extra_tserver_flags.push_back("--use_client_to_server_encryption=true");
    options->extra_tserver_flags.push_back("--allow_insecure_connections=false");
    options->use_even_ips = true;
  }

  if (use_auth_) {
    options->extra_tserver_flags.push_back("--ysql_enable_auth");
  }
}

} // namespace pgwrapper
} // namespace yb
