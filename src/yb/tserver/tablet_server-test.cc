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
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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

#include <limits>

#include "yb/cdc/xcluster_types.h"

#include "yb/common/entity_ids.h"
#include "yb/common/ql_value.h"
#include "yb/common/schema_pbutil.h"
#include "yb/consensus/log-test-base.h"

#include "yb/client/yb_table_name.h"

#include "yb/dockv/partition.h"

#include "yb/gutil/strings/escaping.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/rpc_test_util.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/master/master_defaults.h"

#include "yb/server/call_home-test-util.h"
#include "yb/server/hybrid_clock.h"
#include "yb/server/server_base.pb.h"
#include "yb/server/server_base.proxy.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet_types.pb.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server-test-base.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tablet_server_test_util.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/tserver/tserver_call_home.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/tserver/tserver_shared_mem.h"
#include "yb/tserver/tserver_xcluster_context_if.h"

#include "yb/util/crc.h"
#include "yb/util/curl_util.h"
#include "yb/util/format.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/prometheus_metric_filter.h"
#include "yb/util/size_literals.h"
#include "yb/util/status_log.h"
#include "yb/util/stol_utils.h"

using yb::rpc::MessengerBuilder;
using yb::rpc::RpcController;
using yb::server::HybridClock;
using std::string;

DEFINE_NON_RUNTIME_int32(single_threaded_insert_latency_bench_warmup_rows, 100,
             "Number of rows to insert in the warmup phase of the single threaded"
             " tablet server insert latency micro-benchmark");

DEFINE_NON_RUNTIME_int32(single_threaded_insert_latency_bench_insert_rows, 1000,
             "Number of rows to insert in the testing phase of the single threaded"
             " tablet server insert latency micro-benchmark");

DECLARE_bool(enable_tiered_storage);
DECLARE_bool(enforce_xcluster_guarded_lease);
DECLARE_int32(metrics_retirement_age_ms);
DECLARE_string(block_manager);
DECLARE_string(rpc_bind_addresses);
DECLARE_bool(disable_clock_sync_error);
DECLARE_bool(enable_active_table_metrics_filtering);
DECLARE_uint32(max_active_table_metrics_table_count);
DECLARE_string(metric_node_name);
DECLARE_bool(TEST_tablet_peer_force_get_raft_consensus_failure);

// Declare these metrics prototypes for simpler unit testing of their behavior.
METRIC_DECLARE_counter(rows_inserted);
METRIC_DECLARE_counter(rows_updated);
METRIC_DECLARE_counter(rows_deleted);
METRIC_DECLARE_gauge_uint64(untracked_memory);

namespace yb::tserver {

class TabletServerTest : public TabletServerTestBase {
 public:
  explicit TabletServerTest(TableType table_type = YQL_TABLE_TYPE)
      : TabletServerTestBase(table_type) {}

  // Starts the tablet server, override to start it later.
  void SetUp() override {
    TabletServerTestBase::SetUp();
    StartTabletServer();
  }

  Status CallDeleteTablet(
      const string& uuid, const char* tablet_id, tablet::TabletDataState state) {
    DeleteTabletRequestPB req;
    DeleteTabletResponsePB resp;
    RpcController rpc;

    req.set_dest_uuid(uuid);
    req.set_tablet_id(tablet_id);
    req.set_delete_type(state);

    // Send the call
    {
      SCOPED_TRACE(req.DebugString());
      RETURN_NOT_OK(admin_proxy_->DeleteTablet(req, &resp, &rpc));
      SCOPED_TRACE(resp.DebugString());
      if (resp.has_error()) {
        auto status = StatusFromPB(resp.error().status());
        RETURN_NOT_OK(status);
      }
    }
    return Status::OK();
  }

  string GetWebserverDir() { return GetTestPath("webserver-docroot"); }
};

// Tiered storage is a preview feature: both of its admin RPCs are refused while the flag is off.
TEST_F(TabletServerTest, TieredStorageRpcsRequirePreviewFlag) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_tiered_storage) = false;
  const auto uuid = mini_server_->server()->fs_manager()->uuid();
  {
    AlterTabletTierRequestPB req;
    AlterTabletTierResponsePB resp;
    RpcController rpc;
    req.set_dest_uuid(uuid);
    req.set_tablet_id(kTabletId);
    req.set_target_tier("hdd");
    ASSERT_OK(admin_proxy_->AlterTabletTier(req, &resp, &rpc));
    ASSERT_TRUE(resp.has_error()) << resp.DebugString();
    auto status = StatusFromPB(resp.error().status());
    ASSERT_TRUE(status.IsNotSupported()) << status;
    ASSERT_STR_CONTAINS(status.ToString(), "enable_tiered_storage");
  }
  {
    GetTabletTierInfoRequestPB req;
    GetTabletTierInfoResponsePB resp;
    RpcController rpc;
    req.set_dest_uuid(uuid);
    req.set_tablet_id(kTabletId);
    ASSERT_OK(admin_proxy_->GetTabletTierInfo(req, &resp, &rpc));
    ASSERT_TRUE(resp.has_error()) << resp.DebugString();
    auto status = StatusFromPB(resp.error().status());
    ASSERT_TRUE(status.IsNotSupported()) << status;
  }
}

TEST_F(TabletServerTest, TestPingServer) {
  // Ping the server.
  server::PingRequestPB req;
  server::PingResponsePB resp;
  RpcController controller;
  ASSERT_OK(generic_proxy_->Ping(req, &resp, &controller));
}

// The active-table list applies only to YSQL tables, so this test uses one.
class TabletServerYsqlTableTest : public TabletServerTest {
 public:
  TabletServerYsqlTableTest() : TabletServerTest(PGSQL_TABLE_TYPE) {}
};

TEST_F(TabletServerYsqlTableTest, ActiveTableMetricsFiltering) {
  EasyCurl curl;
  const auto url = Format(
      "http://$0/prometheus-metrics?show_help=false",
      yb::ToString(mini_server_->bound_http_addr()));
  auto Scrape = [&](const std::string& extra_args = "") {
    faststring buffer;
    CHECK_OK(curl.FetchURL(url + extra_args, &buffer));
    return buffer.ToString();
  };
  constexpr std::string_view kFilterArgs = "&version=v2&apply_table_IDs_filter=true";
  auto FilteredScrape = [&](const std::string& extra_args = "") {
    return Scrape(Format("$0$1", kFilterArgs, extra_args));
  };
  const auto table_label = Format(R"#(table_id="$0")#", kTableName.table_name());
  auto GaugeValue = [](
      const std::string& scrape, std::string_view metric_name) -> Result<uint64_t> {
    const auto metric_pos = scrape.find(Format("$0{", metric_name));
    if (metric_pos == std::string::npos) {
      return STATUS_FORMAT(NotFound, "$0 not found", metric_name);
    }
    const auto value_pos = scrape.find("} ", metric_pos);
    if (value_pos == std::string::npos) {
      return STATUS_FORMAT(Corruption, "Malformed $0", metric_name);
    }
    const auto value_end = scrape.find_first_of(" \n", value_pos + 2);
    if (value_end == std::string::npos) {
      return STATUS_FORMAT(Corruption, "Malformed $0", metric_name);
    }
    return CheckedStoull(scrape.substr(value_pos + 2, value_end - value_pos - 2));
  };
  auto LastUpdateTime = [&](const std::string& scrape) {
    return GaugeValue(scrape, "active_table_metrics_last_update_time");
  };
  auto TableCount = [&](const std::string& scrape) {
    return GaugeValue(scrape, "active_table_metrics_table_count");
  };
  auto ListVersion = [&](const std::string& scrape) {
    return GaugeValue(scrape, "active_table_metrics_list_version");
  };

  // The gflag is only a kill switch and defaults to on, so the URL parameters are the opt-in.
  ASSERT_TRUE(FLAGS_enable_active_table_metrics_filtering);
  ASSERT_EQ(FLAGS_max_active_table_metrics_table_count, 1000);
  auto scrape = Scrape();
  ASSERT_EQ(0, ASSERT_RESULT(LastUpdateTime(scrape)));
  ASSERT_EQ(0, ASSERT_RESULT(TableCount(scrape)));
  ASSERT_EQ(0, ASSERT_RESULT(ListVersion(scrape)));
  // Existing scrape URLs retain their behavior.
  ASSERT_STR_CONTAINS(scrape, table_label);
  ASSERT_STR_CONTAINS(Scrape("&version=v2&apply_table_IDs_filter=false"), table_label);
  // v1 scrapes are never filtered, because their table metrics have no server-level copy.
  ASSERT_STR_CONTAINS(Scrape("&apply_table_IDs_filter=true"), table_label);
  ASSERT_STR_CONTAINS(Scrape("&version=v1&apply_table_IDs_filter=true"), table_label);
  // Opted-in scrapes emit no table metrics until the process receives its first list.
  ASSERT_STR_NOT_CONTAINS(FilteredScrape(), table_label);
  // Turning the kill switch off exports every table regardless of the URL parameters.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = false;
  ASSERT_STR_CONTAINS(FilteredScrape(), table_label);
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = true;
  ASSERT_STR_NOT_CONTAINS(FilteredScrape(), table_label);
  // Metrics matching table_always_allowlist are exported for every table.
  ASSERT_STR_CONTAINS(FilteredScrape("&table_always_allowlist=log_wal_size"), table_label);
  ASSERT_STR_NOT_CONTAINS(FilteredScrape("&table_always_allowlist=NONE"), table_label);

  SetActiveTableIdsForMetricsRequestPB req;
  SetActiveTableIdsForMetricsResponsePB resp;
  req.add_table_ids("another-table");
  RpcController controller;
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  ASSERT_TRUE(resp.filtering_enabled());
  scrape = FilteredScrape();
  const auto first_update_time = ASSERT_RESULT(LastUpdateTime(scrape));
  ASSERT_GT(first_update_time, 0);
  ASSERT_EQ(1, ASSERT_RESULT(TableCount(scrape)));
  // The caller did not supply a version.
  ASSERT_EQ(0, ASSERT_RESULT(ListVersion(scrape)));
  ASSERT_STR_CONTAINS(Scrape(), table_label);
  ASSERT_STR_NOT_CONTAINS(FilteredScrape(), table_label);

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = false;
  ASSERT_STR_CONTAINS(FilteredScrape(), table_label);
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = true;
  ASSERT_STR_NOT_CONTAINS(FilteredScrape(), table_label);

  SleepFor(MonoDelta::FromMilliseconds(1));
  req.clear_table_ids();
  req.add_table_ids(kTableName.table_name());
  req.set_list_version(7);
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  scrape = FilteredScrape();
  // The version supplied by the caller is reported as is.
  ASSERT_EQ(7, ASSERT_RESULT(ListVersion(scrape)));
  const auto second_update_time = ASSERT_RESULT(LastUpdateTime(scrape));
  ASSERT_GT(second_update_time, first_update_time);
  ASSERT_STR_CONTAINS(scrape, table_label);

  // The most recently received list remains active indefinitely.
  SleepFor(MonoDelta::FromMilliseconds(100 * kTimeMultiplier));
  ASSERT_STR_CONTAINS(FilteredScrape(), table_label);

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_max_active_table_metrics_table_count) = 1;
  req.add_table_ids("another-table");
  req.set_list_version(8);
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_TRUE(resp.has_error());
  ASSERT_TRUE(StatusFromPB(resp.error().status()).IsInvalidArgument());
  scrape = FilteredScrape();
  ASSERT_EQ(ASSERT_RESULT(LastUpdateTime(scrape)), second_update_time);
  ASSERT_EQ(7, ASSERT_RESULT(ListVersion(scrape)));
  // Rejected updates leave the previously accepted list active.
  ASSERT_STR_CONTAINS(scrape, table_label);

  // The limit is enforced while the request is read: the request is rejected for the limit as soon
  // as it passes it, before reaching the invalid empty ID that follows.
  req.clear_table_ids();
  for (int i = 0; i < 1000; ++i) {
    req.add_table_ids(Format("spurious-table-$0", i));
  }
  req.add_table_ids("");
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_TRUE(resp.has_error());
  const auto over_limit_status = StatusFromPB(resp.error().status());
  ASSERT_TRUE(over_limit_status.IsInvalidArgument()) << over_limit_status;
  ASSERT_STR_CONTAINS(over_limit_status.ToString(), "more than 1 tables");
  scrape = FilteredScrape();
  ASSERT_EQ(ASSERT_RESULT(LastUpdateTime(scrape)), second_update_time);
  ASSERT_STR_CONTAINS(scrape, table_label);

  // The limit counts distinct table IDs, so repeating one does not consume the budget twice.
  req.clear_table_ids();
  req.add_table_ids(kTableName.table_name());
  req.add_table_ids(kTableName.table_name());
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  scrape = FilteredScrape();
  ASSERT_STR_CONTAINS(scrape, table_label);
  ASSERT_EQ(1, ASSERT_RESULT(TableCount(scrape)));

  SleepFor(MonoDelta::FromMilliseconds(1));
  req.clear_table_ids();
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  scrape = FilteredScrape();
  ASSERT_GT(ASSERT_RESULT(LastUpdateTime(scrape)), second_update_time);
  ASSERT_EQ(0, ASSERT_RESULT(TableCount(scrape)));
  ASSERT_STR_CONTAINS(Scrape(), table_label);
  ASSERT_STR_NOT_CONTAINS(scrape, table_label);

  // An empty table ID rejects the whole request, leaving the previously accepted list active.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_max_active_table_metrics_table_count) = 1000;
  req.add_table_ids(kTableName.table_name());
  req.add_table_ids("");
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_TRUE(resp.has_error());
  ASSERT_TRUE(StatusFromPB(resp.error().status()).IsInvalidArgument());
  ASSERT_STR_NOT_CONTAINS(FilteredScrape(), table_label);
  ASSERT_TRUE(resp.filtering_enabled());

  // A list sent while filtering is disabled is still accepted, and the response tells the caller
  // that no scrape is using it.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = false;
  req.clear_table_ids();
  req.add_table_ids(kTableName.table_name());
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  ASSERT_FALSE(resp.filtering_enabled());
  ASSERT_STR_CONTAINS(FilteredScrape(), table_label);

  // Once filtering is enabled again, the list stored while it was off takes effect immediately.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = true;
  ASSERT_STR_CONTAINS(FilteredScrape(), table_label);
  req.clear_table_ids();
  req.add_table_ids("another-table");
  // The largest version a caller can send, reported exactly.
  constexpr uint32_t kLastListVersion = std::numeric_limits<uint32_t>::max();
  req.set_list_version(kLastListVersion);
  resp.Clear();
  controller.Reset();
  ASSERT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  scrape = FilteredScrape();
  ASSERT_STR_NOT_CONTAINS(scrape, table_label);
  ASSERT_EQ(kLastListVersion, ASSERT_RESULT(ListVersion(scrape)));

  // The /tables page shows the current list: its size and version, whether each local table is in
  // it, and the listed IDs that match no local table.
  faststring tables_page;
  ASSERT_OK(curl.FetchURL(
      Format("http://$0/tables", yb::ToString(mini_server_->bound_http_addr())), &tables_page));
  const auto tables_page_str = tables_page.ToString();
  ASSERT_STR_CONTAINS(tables_page_str, "Active table metrics list");
  ASSERT_STR_CONTAINS(tables_page_str, std::to_string(kLastListVersion));
  ASSERT_STR_CONTAINS(tables_page_str, "<li>another-table</li>");
  ASSERT_STR_CONTAINS(
      tables_page_str, "1 table IDs in the list are not hosted on this tablet server");
}

// Tables created by YugabyteDB itself always keep their table-level metrics, colocated tables are
// selected through their parent table ID, and table IDs that match no table are harmless.
TEST_F(TabletServerYsqlTableTest, ActiveTableMetricsSystemAndColocatedTables) {
  struct TestTable {
    std::string namespace_name;
    std::string table_id;
    TableType table_type;
  };
  // AddTestTablet uses the table ID as the table name too.
  const std::vector<TestTable> kSystemTables = {
      // YCQL system tables. Only YSQL tables are filtered, so these pass on table type alone.
      {"system", "cdc_state", YQL_TABLE_TYPE},
      {"system", "pg_advisory_locks", YQL_TABLE_TYPE},
      // YSQL tables in the namespaces YugabyteDB creates.
      {"system_postgres", "sequences_data", PGSQL_TABLE_TYPE},
      {"yb_system", "pg_yb_notifications", PGSQL_TABLE_TYPE},
      {"yb_system", "another_yb_system_table", PGSQL_TABLE_TYPE},
      // xCluster automatic mode DDL replication tables, which live in user databases.
      {"user_db", "ddl_queue", PGSQL_TABLE_TYPE},
      {"user_db", "replicated_ddls", PGSQL_TABLE_TYPE},
      // sequences_data as created before v2.3.0.0: its tablets never got a namespace_name, so only
      // its fixed table ID identifies it.
      {"", kPgSequencesDataTableId, PGSQL_TABLE_TYPE},
  };
  // All tables of a colocated database share the tablets of a parent table, and report their
  // metrics under its ID.
  const std::string kColocatedParentId = "00004000000030008000000000004001.colocation.parent.uuid";
  const std::string kColocatedChildId = "000040000000300080000000000040aa";

  std::vector<TestTable> tables = kSystemTables;
  tables.push_back({"user_db", kColocatedParentId, PGSQL_TABLE_TYPE});
  for (const auto& table : tables) {
    const auto tablet_id = Format("tablet-$0", table.table_id);
    ASSERT_OK(mini_server_->AddTestTablet(
        table.namespace_name, table.table_id, tablet_id, schema_, table.table_type));
    ASSERT_OK(WaitForTabletRunning(tablet_id.c_str()));
  }

  EasyCurl curl;
  const auto base_url = Format("http://$0", yb::ToString(mini_server_->bound_http_addr()));
  auto FilteredScrape = [&]() -> Result<std::string> {
    faststring buffer;
    RETURN_NOT_OK(curl.FetchURL(
        base_url + "/prometheus-metrics?show_help=false&version=v2&apply_table_IDs_filter=true",
        &buffer));
    return buffer.ToString();
  };
  auto Label = [](const std::string& table_id) {
    return Format(R"#(table_id="$0")#", table_id);
  };
  auto SetActiveTables = [&](const std::vector<std::string>& table_ids) -> Status {
    SetActiveTableIdsForMetricsRequestPB req;
    SetActiveTableIdsForMetricsResponsePB resp;
    for (const auto& table_id : table_ids) {
      req.add_table_ids(table_id);
    }
    RpcController controller;
    RETURN_NOT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }
    return Status::OK();
  };
  // Returns the "Table metrics exported" cell of a table's row on the /tables page.
  auto ExportedOnTablesPage = [&](const std::string& table_id) -> Result<std::string> {
    faststring page;
    RETURN_NOT_OK(curl.FetchURL(base_url + "/tables", &page));
    const auto page_str = page.ToString();
    const auto id_pos = page_str.find(Format(">$0<", table_id));
    SCHECK_NE(id_pos, std::string::npos, NotFound, Format("No /tables row for $0", table_id));
    const auto row_end = page_str.find("</tr>", id_pos);
    SCHECK_NE(row_end, std::string::npos, Corruption, "Unterminated /tables row");
    const auto row = page_str.substr(id_pos, row_end - id_pos);
    for (const auto* status : {"Yes (filtering disabled)", "Yes (not a YSQL table)",
                               "Yes (created by YugabyteDB)", "Yes (in list)",
                               "No (no list received)", "No (not in list)"}) {
      if (row.find(status) != std::string::npos) {
        return std::string(status);
      }
    }
    return STATUS_FORMAT(NotFound, "No export status in the /tables row for $0", table_id);
  };
  auto CheckSystemTablesOnTablesPage = [&]() {
    for (const auto& table : kSystemTables) {
      SCOPED_TRACE(table.table_id);
      ASSERT_EQ(
          table.table_type == PGSQL_TABLE_TYPE ? "Yes (created by YugabyteDB)"
                                               : "Yes (not a YSQL table)",
          ASSERT_RESULT(ExportedOnTablesPage(table.table_id)));
    }
  };
  auto CheckSystemTablesExported = [&](const std::string& scrape) {
    for (const auto& table : kSystemTables) {
      SCOPED_TRACE(table.table_id);
      ASSERT_STR_CONTAINS(scrape, Label(table.table_id));
    }
  };

  // Before any list arrives, an opted-in scrape still exports the system tables.
  auto scrape = ASSERT_RESULT(FilteredScrape());
  ASSERT_NO_FATALS(CheckSystemTablesExported(scrape));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kTableName.table_name()));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kColocatedParentId));
  // The /tables page agrees, and says why.
  ASSERT_NO_FATALS(CheckSystemTablesOnTablesPage());
  ASSERT_EQ("No (no list received)", ASSERT_RESULT(ExportedOnTablesPage(kTableName.table_name())));
  ASSERT_EQ("No (no list received)", ASSERT_RESULT(ExportedOnTablesPage(kColocatedParentId)));

  // Table IDs that match no table, including a colocated child ID, select nothing and leave the
  // system tables alone. They are never written to the scrape output.
  const std::vector<std::string> kSpuriousIds = {
      "does-not-exist", kColocatedChildId, "<script>alert(1)</script>", "table\"} 1\nfake_metric 2",
      "\xc3\xa9t\xc3\xa9", std::string(256, 'x')};
  ASSERT_OK(SetActiveTables(kSpuriousIds));
  scrape = ASSERT_RESULT(FilteredScrape());
  ASSERT_NO_FATALS(CheckSystemTablesExported(scrape));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kTableName.table_name()));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kColocatedParentId));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kColocatedChildId));
  ASSERT_STR_NOT_CONTAINS(scrape, "fake_metric");
  ASSERT_STR_NOT_CONTAINS(scrape, "<script>");

  // The /tables page lists them escaped.
  faststring tables_page;
  ASSERT_OK(curl.FetchURL(base_url + "/tables", &tables_page));
  ASSERT_STR_NOT_CONTAINS(tables_page.ToString(), "<script>alert(1)</script>");
  ASSERT_STR_CONTAINS(tables_page.ToString(), "&lt;script&gt;alert(1)&lt;/script&gt;");

  // A table ID above the length bound rejects the request, leaving the previous list active.
  ASSERT_TRUE(SetActiveTables({std::string(257, 'x')}).IsInvalidArgument());
  ASSERT_TRUE(SetActiveTables({kColocatedParentId, std::string(257, 'x')}).IsInvalidArgument());
  ASSERT_STR_NOT_CONTAINS(ASSERT_RESULT(FilteredScrape()), Label(kColocatedParentId));

  // Listing the colocated parent exports the metrics of its colocated tables, which are reported
  // under the parent ID.
  ASSERT_OK(SetActiveTables({kColocatedParentId}));
  scrape = ASSERT_RESULT(FilteredScrape());
  ASSERT_STR_CONTAINS(scrape, Label(kColocatedParentId));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kTableName.table_name()));
  ASSERT_NO_FATALS(CheckSystemTablesExported(scrape));
  ASSERT_EQ("Yes (in list)", ASSERT_RESULT(ExportedOnTablesPage(kColocatedParentId)));
  ASSERT_EQ("No (not in list)", ASSERT_RESULT(ExportedOnTablesPage(kTableName.table_name())));
  ASSERT_NO_FATALS(CheckSystemTablesOnTablesPage());

  // With the kill switch off, every table is exported.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = false;
  ASSERT_EQ(
      "Yes (filtering disabled)", ASSERT_RESULT(ExportedOnTablesPage(kTableName.table_name())));
  ASSERT_STR_CONTAINS(ASSERT_RESULT(FilteredScrape()), Label(kTableName.table_name()));
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_active_table_metrics_filtering) = true;

  // Listing system tables changes nothing for them.
  std::vector<std::string> system_table_ids;
  for (const auto& table : kSystemTables) {
    system_table_ids.push_back(table.table_id);
  }
  ASSERT_OK(SetActiveTables(system_table_ids));
  scrape = ASSERT_RESULT(FilteredScrape());
  ASSERT_NO_FATALS(CheckSystemTablesExported(scrape));
  ASSERT_STR_NOT_CONTAINS(scrape, Label(kColocatedParentId));
}

// The metrics filter cannot depend on yb/master, yb/cdc or yb/common, so it repeats the names of
// the internal tables it exempts. This keeps them in sync with the real constants.
TEST_F(TabletServerTest, ActiveTableMetricsInternalTableConstants) {
  auto Exemption = [](
      const std::string& table_id, const std::string& namespace_name,
      const std::string& table_name) {
    return GetActiveTableListExemption({
        {"table_id", table_id}, {"table_type", TableType_Name(PGSQL_TABLE_TYPE)},
        {"namespace_name", namespace_name}, {"table_name", table_name}});
  };
  ASSERT_EQ(ActiveTableListExemption::kInternalTable,
            Exemption("t1", master::kSystemNamespaceName, "t"));
  ASSERT_EQ(ActiveTableListExemption::kInternalTable,
            Exemption("t2", master::kYbSystemDbName, master::kPgYbNotificationsTableName));
  ASSERT_EQ(ActiveTableListExemption::kInternalTable,
            Exemption("t3", "user_db", xcluster::kDDLQueueTableName));
  ASSERT_EQ(ActiveTableListExemption::kInternalTable,
            Exemption("t4", "user_db", xcluster::kDDLReplicatedTableName));
  // sequences_data is recognised by its table ID even without a namespace name.
  ASSERT_EQ(ActiveTableListExemption::kInternalTable,
            Exemption(kPgSequencesDataTableId, "", "sequences_data"));
  // The filter's check matches the table type label the metrics carry.
  ASSERT_EQ(ActiveTableListExemption::kNone, Exemption("t5", "user_db", "user_table"));
}

// Tables of APIs other than YSQL keep their table-level metrics, since the collector cannot rank
// them.
TEST_F(TabletServerTest, ActiveTableMetricsIgnoreYcqlTables) {
  EasyCurl curl;
  const auto table_label = Format(R"#(table_id="$0")#", kTableName.table_name());
  auto FilteredScrape = [&]() -> Result<std::string> {
    faststring buffer;
    RETURN_NOT_OK(curl.FetchURL(
        Format(
            "http://$0/prometheus-metrics?show_help=false&version=v2&apply_table_IDs_filter=true",
            yb::ToString(mini_server_->bound_http_addr())),
        &buffer));
    return buffer.ToString();
  };
  auto SetActiveTables = [&](std::initializer_list<std::string> table_ids) -> Status {
    SetActiveTableIdsForMetricsRequestPB req;
    SetActiveTableIdsForMetricsResponsePB resp;
    for (const auto& table_id : table_ids) {
      req.add_table_ids(table_id);
    }
    RpcController controller;
    RETURN_NOT_OK(proxy_->SetActiveTableIdsForMetrics(req, &resp, &controller));
    return resp.has_error() ? StatusFromPB(resp.error().status()) : Status::OK();
  };

  // Before any list arrives.
  ASSERT_STR_CONTAINS(ASSERT_RESULT(FilteredScrape()), table_label);
  // With a list that leaves the YCQL table out.
  ASSERT_OK(SetActiveTables({"another-table"}));
  ASSERT_STR_CONTAINS(ASSERT_RESULT(FilteredScrape()), table_label);
  // With an empty list.
  ASSERT_OK(SetActiveTables({}));
  ASSERT_STR_CONTAINS(ASSERT_RESULT(FilteredScrape()), table_label);
}

TEST_F(TabletServerTest, TestServerClock) {
  server::ServerClockRequestPB req;
  server::ServerClockResponsePB resp;
  RpcController controller;

  ASSERT_OK(generic_proxy_->ServerClock(req, &resp, &controller));
  ASSERT_GT(mini_server_->Now().ToUint64(), resp.hybrid_time());
}

TEST_F(TabletServerTest, TestSetFlagsAndCheckWebPages) {
  server::GenericServiceProxy proxy(
      proxy_cache_.get(), HostPort::FromBoundEndpoint(mini_server_->bound_rpc_addr()));

  server::SetFlagRequestPB req;
  server::SetFlagResponsePB resp;

  // Set an invalid flag.
  {
    RpcController controller;
    req.set_flag("foo");
    req.set_value("bar");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(server::SetFlagResponsePB::NO_SUCH_FLAG, resp.result());
    ASSERT_EQ(resp.msg(), "Flag does not exist");
  }

  // Set a valid flag to a valid value.
  {
    int32_t old_val = FLAGS_metrics_retirement_age_ms;
    RpcController controller;
    req.set_flag("metrics_retirement_age_ms");
    req.set_value("12345");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(server::SetFlagResponsePB::SUCCESS, resp.result());
    ASSERT_EQ(resp.msg(), "metrics_retirement_age_ms set to 12345\n");
    ASSERT_EQ(Format("$0", old_val), resp.old_value());
    ASSERT_EQ(12345, FLAGS_metrics_retirement_age_ms);
  }

  // Set a valid flag to an invalid value.
  {
    RpcController controller;
    req.set_flag("metrics_retirement_age_ms");
    req.set_value("foo");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(server::SetFlagResponsePB::BAD_VALUE, resp.result());
    ASSERT_STR_CONTAINS(resp.msg(), "Unable to set flag: Bad value");
    ASSERT_EQ(12345, FLAGS_metrics_retirement_age_ms);
  }
  // Set a valid flag to an value that is rejected by its validator.
  {
    RpcController controller;
    req.set_flag("log_min_segments_to_retain");
    req.set_value("0");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(server::SetFlagResponsePB::BAD_VALUE, resp.result());
    ASSERT_STR_CONTAINS(resp.msg(), "Must be greater than 0");
    ASSERT_EQ(12345, FLAGS_metrics_retirement_age_ms);
  }

  // Try setting a flag which isn't runtime-modifiable
  {
    RpcController controller;
    req.set_flag("tablet_do_compaction_cleanup_for_intents");
    req.set_value("true");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(server::SetFlagResponsePB::NOT_SAFE, resp.result());
  }

  // Try again, but with the force flag.
  {
    RpcController controller;
    req.set_flag("tablet_do_compaction_cleanup_for_intents");
    req.set_value("true");
    req.set_force(true);
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(server::SetFlagResponsePB::SUCCESS, resp.result());
  }

  EasyCurl c;
  faststring buf;
  string addr = yb::ToString(mini_server_->bound_http_addr());

  // Tablets page should list tablet.
  ASSERT_OK(c.FetchURL(Format("http://$0/tablets", addr),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), kTabletId);

  // Tablet page should include the schema.
  ASSERT_OK(c.FetchURL(Format("http://$0/tablet?id=$1", addr, kTabletId),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "<th>key</th>");
  ASSERT_STR_CONTAINS(buf.ToString(), "<td>string NULLABLE VALUE</td>");

  ASSERT_OK(c.FetchURL(Format("http://$0/tablet-consensus-status?id=$1",
                       addr, kTabletId), &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), kTabletId);

  // Test fetching metrics.
  // Fetching metrics has the side effect of retiring metrics, but not in a single pass.
  // So, we check a couple of times in a loop -- thus, if we had a bug where one of these
  // metrics was accidentally un-referenced too early, we'd cause it to get retired.
  // If the metrics survive several passes of fetching, then we are pretty sure they will
  // stick around properly for the whole lifetime of the server.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_metrics_retirement_age_ms) = 0;
  for (int i = 0; i < 3; i++) {
    SCOPED_TRACE(i);
    ASSERT_OK(c.FetchURL(Format("http://$0/jsonmetricz", addr, kTabletId),
                                &buf));

    // Check that the tablet entry shows up.
    ASSERT_STR_CONTAINS(buf.ToString(), "\"type\": \"tablet\"");
    ASSERT_STR_CONTAINS(buf.ToString(), "\"id\": \"test-tablet\"");

    // Check entity attributes.
    ASSERT_STR_CONTAINS(buf.ToString(), "\"table_name\": \"test-table\"");

    // Check for the existence of some particular metrics for which we've had early-retirement
    // bugs in the past.
    ASSERT_STR_CONTAINS(buf.ToString(), "hybrid_clock_hybrid_time");
    ASSERT_STR_CONTAINS(buf.ToString(), "threads_started");
#if YB_TCMALLOC_ENABLED
    ASSERT_STR_CONTAINS(buf.ToString(), "tcmalloc_max_total_thread_cache_bytes");
#endif
    ASSERT_STR_CONTAINS(buf.ToString(), "glog_info_messages");
  }

  // Smoke-test the tracing infrastructure.
  ASSERT_OK(c.FetchURL(
                Format("http://$0/tracing/json/get_buffer_percent_full", addr, kTabletId),
                &buf));
  ASSERT_EQ(buf.ToString(), "0");

  string enable_req_json = "{\"categoryFilter\":\"*\", \"useContinuousTracing\": \"true\","
    " \"useSampling\": \"false\"}";
  string req_b64;
  Base64Escape(enable_req_json, &req_b64);

  ASSERT_OK(c.FetchURL(Format("http://$0/tracing/json/begin_recording?$1",
                                     addr,
                                     req_b64), &buf));
  ASSERT_EQ(buf.ToString(), "");
  ASSERT_OK(c.FetchURL(Format("http://$0/tracing/json/end_recording", addr),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "__metadata");
  ASSERT_OK(c.FetchURL(Format("http://$0/tracing/json/categories", addr),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "\"rpc\"");

  // Smoke test the pprof contention profiler handler.
  ASSERT_OK(c.FetchURL(Format("http://$0/pprof/contention?seconds=1", addr),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "Discarded samples = 0");
#if defined(__linux__)
  // The executable name appears as part of the dump of /proc/self/maps, which
  // only exists on Linux.
  ASSERT_STR_CONTAINS(buf.ToString(), "tablet_server-test");
#endif

  // Test URL parameter: reset_histograms.
  // This parameter allow user to have percentile not to be reseted for every web page fetch.
  // Here, handler_latency_yb_tserver_TabletServerService_Write's percentile is used for testing.
  // In the begining, we expect it's value is zero.
  ASSERT_OK(c.FetchURL(Format("http://$0/prometheus-metrics?reset_histograms=false", addr),
                &buf));
  // Find our target metric and concatenate value zero to it. metric_instance_with_zero_value is a
  // string looks like: handler_latency_yb_tserver_TabletServerService_Write{...quantile=p50...} 0
  string page_content = buf.ToString();
  const auto kMetricLinePrefix = "handler_latency_yb_tserver_TabletServerService_Write{";
  std::size_t begin = 0;
  std::size_t end = 0;
  while ((begin = page_content.find(kMetricLinePrefix, begin)) != std::string::npos) {
    end = page_content.find("}", begin);
    ASSERT_NE(end, std::string::npos);
    if (page_content.find("quantile=\"p50\"", begin) < end) {
      break;
    }
    begin = end + 1;
  }
  ASSERT_NE(begin, std::string::npos);
  string metric_instance_with_zero_value = page_content.substr(begin, end - begin + 1) + " 0";

  ASSERT_STR_CONTAINS(buf.ToString(), metric_instance_with_zero_value);

  // Insert some data
  auto tablet = ASSERT_RESULT(mini_server_->server()->tablet_manager()->GetTablet(kTabletId));
  tablet.reset();
  WriteRequestPB w_req;
  w_req.set_tablet_id(kTabletId);
  WriteResponsePB w_resp;
  {
    RpcController controller;
    AddTestRowInsert(1234, 5678, "testing reset histograms via RPC", &w_req);
    SCOPED_TRACE(w_req.DebugString());
    ASSERT_OK(proxy_->Write(w_req, &w_resp, &controller));
    SCOPED_TRACE(w_resp.DebugString());
    ASSERT_FALSE(w_resp.has_error());
    w_req.clear_ql_write_batch();
  }

  // Check that its percentile become none zero after inserting data
  ASSERT_OK(c.FetchURL(Format("http://$0/prometheus-metrics?reset_histograms=false", addr),
                &buf));
  ASSERT_STR_NOT_CONTAINS(buf.ToString(), metric_instance_with_zero_value);

  // Check that percentile should not to be reseted to zero after refreshing the page
  ASSERT_OK(c.FetchURL(Format("http://$0/prometheus-metrics?reset_histograms=false", addr),
                &buf));
  ASSERT_STR_NOT_CONTAINS(buf.ToString(), metric_instance_with_zero_value);

  // Fetch the page again with reset_histograms=true to reset the percentile
  ASSERT_OK(c.FetchURL(Format("http://$0/prometheus-metrics?reset_histograms=true", addr),
                &buf));

  // Verify that the percentile has been reseted back to zero
  ASSERT_OK(c.FetchURL(Format("http://$0/prometheus-metrics?reset_histograms=true", addr),
                &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), metric_instance_with_zero_value);
  tablet.reset();
}

TEST_F(TabletServerTest, TestInsert) {
  WriteRequestPB req;

  req.set_tablet_id(kTabletId);

  WriteResponsePB resp;
  RpcController controller;

  auto peer = ASSERT_RESULT(mini_server_->server()->tablet_manager()->GetTablet(kTabletId));
  auto tablet = ASSERT_RESULT(peer->shared_tablet());
  scoped_refptr<Counter> rows_inserted =
      METRIC_rows_inserted.Instantiate(tablet->GetTabletMetricsEntity());
  ASSERT_EQ(0, rows_inserted->value());
  tablet.reset();

  // Send an empty request.
  // This should succeed and do nothing.
  {
    controller.Reset();
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
  }

  // Send an actual row insert.
  {
    controller.Reset();
    AddTestRowInsert(1234, 5678, "hello world via RPC", &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    req.clear_ql_write_batch();
  }

  // Send a batch with multiple rows, one of which is a duplicate of
  // the above insert. This should generate one error into per_row_errors.
  {
    controller.Reset();
    AddTestRowInsert(1, 1, "ceci n'est pas une dupe", &req);
    AddTestRowInsert(2, 1, "also not a dupe key", &req);
    AddTestRowInsert(1234, 1, "I am a duplicate key", &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
  }

  // get the clock's current hybrid_time
  HybridTime now_before = mini_server_->Now();

  rows_inserted = nullptr;
  ASSERT_OK(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(1, 1), KeyValue(2, 1), KeyValue(1234, 5678) });

  // get the clock's hybrid_time after replay
  HybridTime now_after = mini_server_->Now();

  // make sure 'now_after' is greater than or equal to 'now_before'
  ASSERT_GE(now_after.value(), now_before.value());
}

TEST_F(TabletServerTest, TestExternalConsistencyModes_ClientPropagated) {
  WriteRequestPB req;
  req.set_tablet_id(kTabletId);
  WriteResponsePB resp;
  RpcController controller;

  auto tablet = ASSERT_RESULT(mini_server_->server()->tablet_manager()->GetTablet(kTabletId));
  // Advance current to some time in the future. we do 5 secs to make
  // sure this hybrid_time will still be in the future when it reaches the
  // server.
  HybridTime current = mini_server_->Now().AddMicroseconds(5000000);

  AddTestRowInsert(1234, 5678, "hello world via RPC", &req);

  req.set_propagated_hybrid_time(current.ToUint64());
  SCOPED_TRACE(req.DebugString());
  ASSERT_OK(proxy_->Write(req, &resp, &controller));
  SCOPED_TRACE(resp.DebugString());
  ASSERT_FALSE(resp.has_error());

  // make sure the server returned a write hybrid_time where only
  // the logical value was increased since he should have updated
  // its clock with the client's value.
  HybridTime write_hybrid_time(resp.propagated_hybrid_time());

  ASSERT_EQ(current.GetPhysicalValueMicros(),
            write_hybrid_time.GetPhysicalValueMicros());

  ASSERT_LE(current.GetLogicalValue() + 1,
            write_hybrid_time.GetLogicalValue());
}

TEST_F(TabletServerTest, TestInsertAndMutate) {
  ASSERT_OK(mini_server_->server()->tablet_manager()->GetTablet(kTabletId));

  RpcController controller;

  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);

    AddTestRowInsert(1, 1, "original1", &req);
    AddTestRowInsert(2, 2, "original2", &req);
    AddTestRowInsert(3, 3, "original3", &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
    controller.Reset();
  }

  // Try and mutate the rows inserted above
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);

    AddTestRowUpdate(1, 2, "mutation1", &req);
    AddTestRowUpdate(2, 3, "mutation2", &req);
    AddTestRowUpdate(3, 4, "mutation3", &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
    controller.Reset();
  }

  // Try and mutate a non existent row key (should not get an error)
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);

    AddTestRowUpdate(1234, 2, "mutated", &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
    controller.Reset();
  }

  // Try and delete 1 row
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);

    AddTestRowDelete(1, &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
    controller.Reset();
  }

  // Now try and mutate a row we just deleted, we should not get an error
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);

    AddTestRowUpdate(1, 2, "mutated1", &req);
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
    controller.Reset();
  }

  // At this point, we have two left.
  VerifyRows(schema_, {KeyValue(1, 2), KeyValue(2, 3), KeyValue(3, 4), KeyValue(1234, 2)});

  // Do a mixed operation (some insert, update, and delete, some of which fail)
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);

    // op 0: Mutate row 1, which doesn't exist. This should not fail.
    AddTestRowUpdate(1, 3, "mutate_should_not_fail", &req);
    // op 1: Insert a new row 4 (succeeds)
    AddTestRowInsert(4, 4, "new row 4", &req);
    // op 2: Delete a non-existent row 5 (should fail)
    AddTestRowDelete(5, &req);
    // op 3: Insert a new row 6 (succeeds)
    AddTestRowInsert(6, 6, "new row 6", &req);

    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
    controller.Reset();
  }

  // get the clock's current hybrid_time
  HybridTime now_before = mini_server_->Now();

  ASSERT_NO_FATALS(WARN_NOT_OK(ShutdownAndRebuildTablet(), "Shutdown failed: "));
  VerifyRows(schema_,
      { KeyValue(1, 3), KeyValue(2, 3), KeyValue(3, 4), KeyValue(4, 4), KeyValue(6, 6),
        KeyValue(1234, 2) });

  // get the clock's hybrid_time after replay
  HybridTime now_after = mini_server_->Now();

  // make sure 'now_after' is greater that or equal to 'now_before'
  ASSERT_GE(now_after.value(), now_before.value());
}

// Test that passing a schema with fields not present in the tablet schema
// throws an exception.
TEST_F(TabletServerTest, TestInvalidWriteRequest_BadSchema) {
  SchemaBuilder schema_builder(schema_);
  ASSERT_OK(schema_builder.AddColumn("col_doesnt_exist", DataType::INT32));
  Schema bad_schema_with_ids = schema_builder.Build();
  Schema bad_schema = schema_builder.BuildWithoutIds();

  // Send a row insert with an extra column
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    RpcController controller;

    req.set_tablet_id(kTabletId);

    AddTestRowInsert(1234, 5678, "hello world via RPC", &req);
    req.mutable_ql_write_batch(0)->set_schema_version(1);

    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    ASSERT_EQ(QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH, resp.ql_response_batch(0).status());
  }
}

TEST_F(TabletServerTest, TestClientGetsErrorBackWhenRecoveryFailed) {
  ASSERT_NO_FATALS(InsertTestRowsRemote(0, 1, 7));

  auto tablet = ASSERT_RESULT(tablet_peer_->shared_tablet());
  ASSERT_OK(tablet->Flush(tablet::FlushMode::kSync, rocksdb::FlushReason::kTestOnly));

  // Save the log path before shutting down the tablet (and destroying
  // the tablet peer).
  string log_path = tablet_peer_->log()->TEST_ActiveSegment()->path();
  auto idx = tablet_peer_->log()->TEST_ActiveSegment()->first_entry_offset() + 300;

  ShutdownTablet();
  ASSERT_OK(log::CorruptLogFile(env_.get(), log_path, log::FLIP_BYTE, idx));

  ASSERT_FALSE(ShutdownAndRebuildTablet().ok());

  // Connect to it.
  CreateTsClientProxies(HostPort::FromBoundEndpoint(mini_server_->bound_rpc_addr()),
                        proxy_cache_.get(),
                        &proxy_, &admin_proxy_, &consensus_proxy_, &generic_proxy_, &backup_proxy_);

  WriteRequestPB req;
  req.set_tablet_id(kTabletId);

  WriteResponsePB resp;
  rpc::RpcController controller;

  // We're expecting the write to fail.
  ASSERT_OK(DCHECK_NOTNULL(proxy_.get())->Write(req, &resp, &controller));
  ASSERT_EQ(TabletServerErrorPB::TABLET_NOT_RUNNING, resp.error().code());
  ASSERT_STR_CONTAINS(
      resp.error().status().message(), Format("Tablet $0 not RUNNING: FAILED", kTabletId));
}

TEST_F(TabletServerTest, TestCreateTablet_TabletExists) {
  CreateTabletRequestPB req;
  CreateTabletResponsePB resp;
  RpcController rpc;

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_table_id("testtb");
  req.set_tablet_id(kTabletId);
  req.set_table_name("testtb");
  req.mutable_config()->CopyFrom(mini_server_->CreateLocalConfig());

  Schema schema = SchemaBuilder(schema_).Build();
  SchemaToPB(schema, req.mutable_schema());

  // Send the call
  {
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(admin_proxy_->CreateTablet(req, &resp, &rpc));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::TABLET_ALREADY_EXISTS, resp.error().code());
  }
}

TEST_F(TabletServerTest, TestDeleteTablet) {
  // Verify that the tablet exists
  ASSERT_OK(mini_server_->server()->tablet_manager()->GetTablet(kTabletId));

  // Put some data in the tablet. We flush and insert more rows to ensure that
  // there is data both in the MRS and on disk.
  ASSERT_NO_FATALS(InsertTestRowsRemote(0, 1, 1));
  auto tablet = ASSERT_RESULT(tablet_peer_->shared_tablet());
  ASSERT_OK(tablet->Flush(tablet::FlushMode::kSync, rocksdb::FlushReason::kTestOnly));
  ASSERT_NO_FATALS(InsertTestRowsRemote(0, 2, 1));

  // Drop any local references to the tablet from within this test,
  // so that when we delete it on the server, it's not held alive
  // by the test code.
  tablet_peer_.reset();

  ASSERT_OK(CallDeleteTablet(mini_server_->server()->fs_manager()->uuid(),
                             kTabletId,
                             tablet::TABLET_DATA_DELETED));

  // Verify that the tablet is removed from the tablet map
  ASSERT_TRUE(ResultToStatus(
      mini_server_->server()->tablet_manager()->GetTablet(kTabletId)).IsNotFound());

  // Verify that fetching metrics doesn't crash. Regression test for KUDU-638.
  EasyCurl c;
  faststring buf;
  ASSERT_OK(c.FetchURL(Format("http://$0/jsonmetricz",
                              AsString(mini_server_->bound_http_addr())),
                                           &buf));

  // Verify that after restarting the TS, the tablet is still not in the tablet manager.
  // This ensures that the on-disk metadata got removed.
  Status s = ShutdownAndRebuildTablet();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_TRUE(ResultToStatus(
      mini_server_->server()->tablet_manager()->GetTablet(kTabletId)).IsNotFound());

  // Verify that the BlockBasedTable mem tracker is still attached to the server mem tracker.
  ASSERT_TRUE(mini_server_->server()->mem_tracker()->FindChild("BlockBasedTable") != nullptr);
}

TEST_F(TabletServerTest, TestDeleteTablet_TabletNotCreated) {
  Status s = CallDeleteTablet(mini_server_->server()->fs_manager()->uuid(),
                              "NotPresentTabletId",
                              tablet::TABLET_DATA_DELETED);
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
}

// Regression test for #31046. CheckTserverTabletHealth used to call
// GetConsensusOrRespond inside its per-tablet loop, which (a) FATALed via
// Result::get() on a non-OK Result before the 3-arg overload was fixed,
// and (b) consumed the RpcContext on the first failing tablet, leading
// the trailing context.RespondSuccess() to double-respond.
// Production hits this when the RUNNING-state check in LookupTabletPeer
// races with consensus transitioning to non-OK. We use a TEST flag to
// force that condition deterministically: LookupTabletPeer still sees
// the peer as RUNNING, but GetRaftConsensus returns IllegalState.
// The RPC must complete without crashing the process.
TEST_F(TabletServerTest, CheckTserverTabletHealthDoesNotCrashWhenConsensusUnavailable) {
  // YBTest::flag_saver_ restores all gflags on fixture teardown, so no manual reset needed.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_tablet_peer_force_get_raft_consensus_failure) = true;

  CheckTserverTabletHealthRequestPB req;
  req.add_tablet_ids(kTabletId);
  CheckTserverTabletHealthResponsePB resp;
  RpcController controller;
  ASSERT_OK(proxy_->CheckTserverTabletHealth(req, &resp, &controller));
  EXPECT_FALSE(resp.has_error());
  EXPECT_EQ(0, resp.tablet_healths_size());
}

// Test that with concurrent requests to delete the same tablet, one wins and
// the other fails, with no assertion failures. Regression test for KUDU-345.
TEST_F(TabletServerTest, TestConcurrentDeleteTablet) {
  // Verify that the tablet exists
  ASSERT_OK(mini_server_->server()->tablet_manager()->GetTablet(kTabletId));

  static const int kNumDeletes = 2;
  RpcController rpcs[kNumDeletes];
  DeleteTabletResponsePB responses[kNumDeletes];
  CountDownLatch latch(kNumDeletes);

  DeleteTabletRequestPB req;
  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_delete_type(tablet::TABLET_DATA_DELETED);

  for (int i = 0; i < kNumDeletes; i++) {
    SCOPED_TRACE(req.DebugString());
    admin_proxy_->DeleteTabletAsync(
        req, &responses[i], &rpcs[i], [&latch]() { latch.CountDown(); });
  }
  latch.Wait();

  int num_success = 0;
  for (int i = 0; i < kNumDeletes; i++) {
    ASSERT_TRUE(rpcs[i].finished());
    LOG(INFO) << "STATUS " << i << ": " << rpcs[i].status().ToString();
    LOG(INFO) << "RESPONSE " << i << ": " << responses[i].DebugString();
    if (!responses[i].has_error()) {
      num_success++;
    }
  }

  // Verify that the tablet is removed from the tablet map
  ASSERT_TRUE(ResultToStatus(
      mini_server_->server()->tablet_manager()->GetTablet(kTabletId)).IsNotFound());
  ASSERT_EQ(1, num_success);
}

TEST_F(TabletServerTest, TestInsertLatencyMicroBenchmark) {
  METRIC_DEFINE_entity(test);
  METRIC_DEFINE_event_stats(test, insert_latency,
                          "Insert Latency",
                          MetricUnit::kMicroseconds,
                          "TabletServer single threaded insert latency (microseconds).");

  scoped_refptr<EventStats> stats =
      METRIC_insert_latency.Instantiate(ts_test_metric_entity_);

  auto warmup = AllowSlowTests() ?
      FLAGS_single_threaded_insert_latency_bench_warmup_rows : 10;

  for (int i = 0; i < warmup; i++) {
    InsertTestRowsRemote(0, i, 1);
  }

  auto max_rows = AllowSlowTests() ?
      FLAGS_single_threaded_insert_latency_bench_insert_rows : 100;

  MonoTime start = MonoTime::Now();

  for (int i = warmup; i < warmup + max_rows; i++) {
    MonoTime before = MonoTime::Now();
    InsertTestRowsRemote(0, i, 1);
    MonoTime after = MonoTime::Now();
    MonoDelta delta = after.GetDeltaSince(before);
    stats->Increment(delta.ToMicroseconds());
  }

  MonoTime end = MonoTime::Now();
  double throughput = ((max_rows - warmup) * 1.0) / end.GetDeltaSince(start).ToSeconds();

  // Generate the JSON.
  std::stringstream out;
  JsonWriter writer(&out, JsonWriter::PRETTY);
  ASSERT_OK(stats->WriteAsJson(&writer, MetricJsonOptions()));

  LOG(INFO) << "Throughput: " << throughput << " rows/sec.";
  LOG(INFO) << out.str();
}

// Simple test to ensure we can destroy an RpcServer in different states of
// initialization before Start()ing it.
TEST_F(TabletServerTest, TestRpcServerCreateDestroy) {
  server::RpcServerOptions opts;
  {
    server::RpcServer server1(
        "server1", opts, rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>());
  }
  {
    MessengerBuilder mb("foo");
    auto messenger = rpc::CreateAutoShutdownMessengerHolder(ASSERT_RESULT(mb.Build()));
    {
      server::RpcServer server2(
          "server2", opts, rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>());
      ASSERT_OK(server2.Init(messenger.get()));
    }
  }
}

// Simple test to ensure we can create RpcServer with different bind address options.
TEST_F(TabletServerTest, TestRpcServerRPCFlag) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_rpc_bind_addresses) = "0.0.0.0:2000";
  server::RpcServerOptions opts;
  ServerRegistrationPB reg;
  auto tbo = ASSERT_RESULT(TabletServerOptions::CreateTabletServerOptions());
  MessengerBuilder mb("foo");
  auto messenger = CreateAutoShutdownMessengerHolder(ASSERT_RESULT(mb.Build()));

  server::RpcServer server1(
      "server1", opts, rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>());
  ASSERT_OK(server1.Init(messenger.get()));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_rpc_bind_addresses) = "0.0.0.0:2000,0.0.0.1:2001";
  server::RpcServerOptions opts2;
  server::RpcServer server2(
      "server2", opts2, rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>());
  ASSERT_OK(server2.Init(messenger.get()));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_rpc_bind_addresses) = "10.20.30.40:2017";
  server::RpcServerOptions opts3;
  server::RpcServer server3(
      "server3", opts3, rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>());
  ASSERT_OK(server3.Init(messenger.get()));

  reg.Clear();
  tbo.fs_opts.data_paths = { GetTestPath("fake-ts") };
  tbo.rpc_opts = opts3;
  TabletServer server(tbo);

  ASSERT_NO_FATALS(WARN_NOT_OK(server.Init(), "Ignore"));
  // This call will fail for http binding, but this test is for rpc.
  ASSERT_NO_FATALS(WARN_NOT_OK(server.GetRegistration(&reg), "Ignore"));
  ASSERT_EQ(1, reg.private_rpc_addresses().size());
  ASSERT_EQ("10.20.30.40", reg.private_rpc_addresses(0).host());
  ASSERT_EQ(2017, reg.private_rpc_addresses(0).port());

  reg.Clear();
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_rpc_bind_addresses) = "10.20.30.40:2017,20.30.40.50:2018";
  server::RpcServerOptions opts4;
  tbo.rpc_opts = opts4;
  TabletServer tserver2(tbo);
  ASSERT_NO_FATALS(WARN_NOT_OK(tserver2.Init(), "Ignore"));
  // This call will fail for http binding, but this test is for rpc.
  ASSERT_NO_FATALS(WARN_NOT_OK(tserver2.GetRegistration(&reg), "Ignore"));
  ASSERT_EQ(2, reg.private_rpc_addresses().size());
}

// We are not checking if a row is out of bounds in YB because we are using a completely different
// hash-based partitioning scheme, so this test is now actually testing that there is no error
// returned from the server. If we introduce such range checking, this test could be enhanced to
// test for it.
TEST_F(TabletServerTest, TestWriteOutOfBounds) {
  const char *tabletId = "TestWriteOutOfBoundsTablet";
  Schema schema = SchemaBuilder(schema_).Build();

  dockv::PartitionSchema partition_schema;
  CHECK_OK(dockv::PartitionSchema::FromPB(PartitionSchemaPB(), schema, &partition_schema));

  dockv::Partition partition;
  auto table_info = tablet::TableInfo::TEST_Create(
      "TestWriteOutOfBoundsTable", "test_ns", tabletId, YQL_TABLE_TYPE, schema, partition_schema);
  ASSERT_OK(mini_server_->server()->tablet_manager()->CreateNewTablet(
      table_info, tabletId, partition, mini_server_->CreateLocalConfig()));

  ASSERT_OK(WaitForTabletRunning(tabletId));

  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController controller;
  req.set_tablet_id(tabletId);

  for (auto op : { QLWriteRequestPB::QL_STMT_INSERT, QLWriteRequestPB::QL_STMT_UPDATE }) {
    AddTestRow(20, 1, "1", op, &req);
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    req.clear_ql_write_batch();
    controller.Reset();
  }
}

namespace {

void CalcTestRowChecksum(uint64_t *out, int32_t key, uint8_t string_field_defined = true) {
  QLValue value;

  string strval = Format("original$0", key);
  string buffer;
  uint32_t index = 0;
  buffer.append(pointer_cast<const char*>(&index), sizeof(index));
  value.set_int32_value(key);
  value.value().AppendToString(&buffer);

  index = 1;
  buffer.append(pointer_cast<const char*>(&index), sizeof(index));
  value.value().AppendToString(&buffer);

  index = 2;
  buffer.append(pointer_cast<const char*>(&index), sizeof(index));
  buffer.append(pointer_cast<const char*>(&string_field_defined), sizeof(string_field_defined));
  if (string_field_defined) {
    value.set_string_value(strval);
    value.value().AppendToString(&buffer);
  }

  crc::Crc* crc = crc::GetCrc32cInstance();
  crc->Compute(buffer.c_str(), buffer.size(), out, nullptr);
}

} // namespace

// Simple test to check that our checksum scans work as expected.
TEST_F(TabletServerTest, TestChecksumScan) {
  uint64_t total_crc = 0;

  ChecksumRequestPB req;
  req.set_tablet_id(kTabletId);
  ChecksumResponsePB resp;
  RpcController controller;
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));

  // No rows.
  ASSERT_EQ(total_crc, resp.checksum());

  // First row.
  int32_t key = 1;
  InsertTestRowsRemote(0, key, 1);
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  CalcTestRowChecksum(&total_crc, key);
  uint64_t first_crc = total_crc; // Cache first record checksum.

  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  ASSERT_EQ(total_crc, resp.checksum());

  // Second row (null string field).
  key = 2;
  InsertTestRowsRemote(
      0, key, 1, 1, /*proxy=*/nullptr, kTabletId, /*write_hybrid_times_collector=*/nullptr,
      /*ts=*/nullptr, /*string_field_defined=*/false);
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  CalcTestRowChecksum(&total_crc, key, /*string_field_defined=*/false);

  ASSERT_FALSE(resp.has_error()) << resp.error().DebugString();
  ASSERT_EQ(total_crc, resp.checksum());

  // Finally, delete row 2, so we're back to the row 1 checksum.
  ASSERT_NO_FATALS(DeleteTestRowsRemote(key, 1));
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  ASSERT_NE(total_crc, resp.checksum());
  ASSERT_EQ(first_crc, resp.checksum());
}

TEST_F(TabletServerTest, TestCallHome) {
  const auto webserver_dir = GetWebserverDir();
  CHECK_OK(env_->CreateDir(webserver_dir));
  TestCallHome<TabletServer, TserverCallHome>(
      webserver_dir, {"ysql_node_stats"} /*additional_collections*/, mini_server_->server());
}

// This tests whether the enabling/disabling of callhome is happening dynamically
// during runtime.
TEST_F(TabletServerTest, TestCallHomeFlag) {
  const auto webserver_dir = GetWebserverDir();
  CHECK_OK(env_->CreateDir(webserver_dir));
  TestCallHomeFlag<TabletServer, TserverCallHome>(webserver_dir, mini_server_->server());
}

TEST_F(TabletServerTest, TestGFlagsCallHome) {
  CHECK_OK(env_->CreateDir(GetWebserverDir()));
  TestGFlagsCallHome<TabletServer, TserverCallHome>(mini_server_->server());
}

TEST_F(TabletServerTest, ThreadMetricsStability) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_metrics_retirement_age_ms) = 1;
  auto ScrapeAndVerifyThreadMetrics = [&]() {
    MetricPrometheusOptions opts;
    opts.export_help_and_type = ExportHelpAndType::kFalse;
    std::stringstream output;
    PrometheusWriter writer(&output, opts);
    ASSERT_OK(mini_server_->server()->metric_registry()->WriteForPrometheus(&writer, opts));

    ASSERT_TRUE(output.str().contains("threads_running_"));
    ASSERT_TRUE(output.str().contains("threads_started_"));
  };

  // Verify thread metrics are not retired despite FLAGS_metrics_retirement_age_ms = 0.
  // Requires 3 scrapes because metric retirement is a two-phase process:
  // - After scrape 1: cleanup thread marks unreferenced metrics for deletion
  // - After scrape 2: cleanup thread deletes previously marked metrics
  // - Scrape 3: confirms metrics still exist (if they were unreferenced, they'd be gone)

  ScrapeAndVerifyThreadMetrics();
  SleepFor(MonoDelta::FromSeconds(1));

  ScrapeAndVerifyThreadMetrics();
  SleepFor(MonoDelta::FromSeconds(1));

  ScrapeAndVerifyThreadMetrics();
}

#if YB_TCMALLOC_ENABLED
TEST_F(TabletServerTest, TestUntrackedMemory) {
  auto server_metric_entity = mini_server_->server()->metric_entity();
  ASSERT_TRUE(server_metric_entity->TEST_ContainsMetricName("untracked_memory"));

  const size_t kBufferSize = 10_MB;
  std::unique_ptr<char[]> alloc(new char[kBufferSize]);
  // Clang in release mode can optimize out the above allocation unless
  // we do something with the pointer... so we just log it.
  VLOG(8) << static_cast<void*>(alloc.get());
  ASSERT_LE(kBufferSize, server_metric_entity->FindOrNull<FunctionGauge<uint64_t>>(
      METRIC_untracked_memory)->value());
}
#endif

// FlushTablets RPC validation tests, parameterized by operation (FLUSH / COMPACT).
class FlushTabletsTest
    : public TabletServerTest,
      public ::testing::WithParamInterface<FlushTabletsRequestPB::Operation> {
 protected:
  FlushTabletsRequestPB::Operation Op() const { return GetParam(); }

  struct FlushResult {
    FlushTabletsResponsePB resp;
    Status rpc_status;
  };

  FlushResult CallFlushTablets(
      tablet::FlushCompactFlags flags,
      const std::vector<string>& vector_index_ids = {}) {
    FlushTabletsRequestPB req;
    req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
    req.add_tablet_ids(kTabletId);
    req.set_operation(Op());
    req.set_flags(flags);
    for (const auto& id : vector_index_ids) {
      req.add_vector_index_ids(id);
    }
    FlushResult result;
    RpcController rpc;
    result.rpc_status = admin_proxy_->FlushTablets(req, &result.resp, &rpc);
    return result;
  }

  void AssertSuccess(
      tablet::FlushCompactFlags flags,
      const std::vector<string>& vector_index_ids = {}) {
    auto result = CallFlushTablets(flags, vector_index_ids);
    ASSERT_OK(result.rpc_status);
    ASSERT_FALSE(result.resp.has_error()) << result.resp.error().DebugString();
  }

  void AssertInvalidArgument(
      tablet::FlushCompactFlags flags,
      const std::vector<string>& vector_index_ids = {}) {
    auto result = CallFlushTablets(flags, vector_index_ids);
    ASSERT_OK(result.rpc_status);
    ASSERT_TRUE(result.resp.has_error()) << "Expected error but got success";
    auto status = StatusFromPB(result.resp.error().status());
    ASSERT_TRUE(status.IsInvalidArgument()) << "Expected InvalidArgument, got: " << status;
  }
};

INSTANTIATE_TEST_SUITE_P(
    FlushAndCompact,
    FlushTabletsTest,
    ::testing::Values(FlushTabletsRequestPB::FLUSH, FlushTabletsRequestPB::COMPACT),
    [](const auto& info) {
      return info.param == FlushTabletsRequestPB::FLUSH ? "Flush" : "Compact";
    });

// Tests common to both FLUSH and COMPACT.

TEST_P(FlushTabletsTest, Default) {
  AssertSuccess(tablet::FLUSH_COMPACT_DEFAULT);
}

TEST_P(FlushTabletsTest, All) {
  AssertSuccess(tablet::FLUSH_COMPACT_ALL);
}

TEST_P(FlushTabletsTest, VectorIndexOnly) {
  AssertSuccess(tablet::FLUSH_COMPACT_VECTOR_INDEX_ONLY);
}

TEST_P(FlushTabletsTest, VectorIndexExcludedWithIdsRejected) {
  AssertInvalidArgument(
      tablet::FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED, {"fake-vector-index-id"});
}

TEST_P(FlushTabletsTest, DefaultWithVectorIds) {
  AssertSuccess(tablet::FLUSH_COMPACT_DEFAULT, {"nonexistent-vector-index"});
}

// FLUSH accepts REGULAR_FOR_TEST_ONLY; COMPACT rejects it.
TEST_P(FlushTabletsTest, RegularForTestOnly) {
  if (Op() == FlushTabletsRequestPB::FLUSH) {
    AssertSuccess(tablet::FLUSH_COMPACT_REGULAR_FOR_TEST_ONLY);
  } else {
    AssertInvalidArgument(tablet::FLUSH_COMPACT_REGULAR_FOR_TEST_ONLY);
  }
}

// COMPACT accepts VECTOR_INDEX_EXCLUDED; FLUSH rejects it.
TEST_P(FlushTabletsTest, VectorIndexExcluded) {
  if (Op() == FlushTabletsRequestPB::COMPACT) {
    AssertSuccess(tablet::FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED);
  } else {
    AssertInvalidArgument(tablet::FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED);
  }
}

TEST_F(TabletServerTest, ApplyXClusterGuardedInfoIfNewer) {
  // The fixture's TServer has no master, so nothing else applies xCluster-guarded info.  It also
  // never gets a xCluster-guarded lease, so do not let the lack of one hide the applied roles.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_enforce_xcluster_guarded_lease) = false;
  auto* server = mini_server_->server();
  const NamespaceId kNamespace = "namespace";

  auto make_info = [&](int64_t term, uint64_t count, XClusterNamespaceInfoPB::XClusterRole role,
                       uint32_t oid_cache_invalidations_count) {
    XClusterGuardedInfoPB info;
    info.mutable_xcluster_guarded_info_version()->set_term(term);
    info.mutable_xcluster_guarded_info_version()->set_count(count);
    (*info.mutable_xcluster_info_per_namespace())[kNamespace].set_role(role);
    info.set_oid_cache_invalidations_count(oid_cache_invalidations_count);
    return info;
  };
  auto get_role = [&]() { return server->GetXClusterContext().GetXClusterRole(kNamespace); };
  auto get_oid_cache_invalidations_count = [&]() {
    return server->GetXClusterContext().GetOidCacheInvalidationsCount();
  };

  server->ApplyXClusterGuardedInfoIfNewer(
      make_info(2, 5, XClusterNamespaceInfoPB::AUTOMATIC_SOURCE, 10));
  EXPECT_EQ(get_role(), XClusterNamespaceInfoPB::AUTOMATIC_SOURCE);
  EXPECT_EQ(ASSERT_RESULT(get_oid_cache_invalidations_count()), 10);

  // An earlier version is ignored, even with a higher count.
  server->ApplyXClusterGuardedInfoIfNewer(
      make_info(1, 6, XClusterNamespaceInfoPB::AUTOMATIC_TARGET, 11));
  EXPECT_EQ(get_role(), XClusterNamespaceInfoPB::AUTOMATIC_SOURCE);
  EXPECT_EQ(ASSERT_RESULT(get_oid_cache_invalidations_count()), 10);

  // The same version is ignored.
  server->ApplyXClusterGuardedInfoIfNewer(
      make_info(2, 5, XClusterNamespaceInfoPB::AUTOMATIC_TARGET, 11));
  EXPECT_EQ(get_role(), XClusterNamespaceInfoPB::AUTOMATIC_SOURCE);
  EXPECT_EQ(ASSERT_RESULT(get_oid_cache_invalidations_count()), 10);

  // A higher count in the same term is applied.
  server->ApplyXClusterGuardedInfoIfNewer(
      make_info(2, 6, XClusterNamespaceInfoPB::AUTOMATIC_TARGET, 11));
  EXPECT_EQ(get_role(), XClusterNamespaceInfoPB::AUTOMATIC_TARGET);
  EXPECT_EQ(ASSERT_RESULT(get_oid_cache_invalidations_count()), 11);

  // A higher term is applied, even with a lower count.
  server->ApplyXClusterGuardedInfoIfNewer(
      make_info(3, 0, XClusterNamespaceInfoPB::AUTOMATIC_SOURCE, 12));
  EXPECT_EQ(get_role(), XClusterNamespaceInfoPB::AUTOMATIC_SOURCE);
  EXPECT_EQ(ASSERT_RESULT(get_oid_cache_invalidations_count()), 12);
}

// A connection can cache a prefetch for a database this tserver has not heard of yet, and the
// fill that records the version is the only one there will be: the response cache keeps what
// that fill left behind, every later connection is served from it, and the observer that records
// runs only on a fill. A version arriving before the slot is therefore held and written once the
// slot exists, or the node would hold a prefetch it never admits to holding and no connection
// would read an older version for that database until its catalog version moved again.
TEST_F(TabletServerTest, LastPrefetchedCatalogVersionSurvivesALateSlot) {
  auto* server = mini_server_->server();
  constexpr uint32_t kDbOid = 16384;
  constexpr uint64_t kVersion = 5;

  // Nothing knows this database yet, so there is nowhere to record into.
  server->SetYsqlDbLastPrefetchedCatalogVersion(kDbOid, kVersion);

  DBCatalogVersionDataPB data;
  auto* entry = data.add_db_catalog_versions();
  entry->set_db_oid(kDbOid);
  entry->set_current_version(kVersion);
  entry->set_last_breaking_version(1);
  server->SetYsqlDBCatalogVersions(data);

  GetTserverCatalogVersionInfoRequestPB req;
  GetTserverCatalogVersionInfoResponsePB resp;
  req.set_db_oid(kDbOid);
  ASSERT_OK(server->get_ysql_db_oid_to_cat_version_info_map(req, &resp));
  ASSERT_EQ(resp.entries_size(), 1);
  ASSERT_EQ(
      server->SharedObject()->ysql_db_last_prefetched_catalog_version(resp.entries(0).shm_index()),
      kVersion)
      << "a version cached before the database had a slot was dropped rather than held";
}

TEST_F(TabletServerTest, LastPrefetchedCatalogVersionHoldsNoMoreThanThereAreSlots) {
  auto* server = mini_server_->server();
  constexpr uint32_t kFirstDbOid = 20000;
  constexpr uint32_t kOneTooMany = 50000;

  // Fill the held map to the number of slots that exist. None of these has a slot, so a node that
  // never allocates one -- global catalog version mode -- would otherwise keep taking more.
  for (uint32_t i = 0; i < TServerSharedData::kMaxNumDbCatalogVersions; ++i) {
    server->SetYsqlDbLastPrefetchedCatalogVersion(kFirstDbOid + i, 5);
  }
  // Refused: there is no slot left for it to be waiting for.
  server->SetYsqlDbLastPrefetchedCatalogVersion(kOneTooMany, 7);
  // Accepted: already held, and a newer version is what a later slot should receive.
  server->SetYsqlDbLastPrefetchedCatalogVersion(kFirstDbOid, 9);

  DBCatalogVersionDataPB data;
  for (auto db_oid : {kOneTooMany, kFirstDbOid}) {
    auto* entry = data.add_db_catalog_versions();
    entry->set_db_oid(db_oid);
    entry->set_current_version(1);
    entry->set_last_breaking_version(1);
  }
  server->SetYsqlDBCatalogVersions(data);

  const auto recorded = [server](uint32_t db_oid) -> Result<uint64_t> {
    GetTserverCatalogVersionInfoRequestPB req;
    GetTserverCatalogVersionInfoResponsePB resp;
    req.set_db_oid(db_oid);
    RETURN_NOT_OK(server->get_ysql_db_oid_to_cat_version_info_map(req, &resp));
    SCHECK_EQ(resp.entries_size(), 1, IllegalState, "no slot");
    return server->SharedObject()->ysql_db_last_prefetched_catalog_version(
        resp.entries(0).shm_index());
  };
  ASSERT_EQ(ASSERT_RESULT(recorded(kOneTooMany)), 0)
      << "a version was held past the number of slots that can ever exist";
  ASSERT_EQ(ASSERT_RESULT(recorded(kFirstDbOid)), 9)
      << "a database already held stopped taking newer versions once the map was full";
}

}  // namespace yb::tserver
