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

#include <map>
#include "yb/server/webserver.h"
#include "yb/util/web_callback_registry.h"
#include "yb/server/pgsql_webserver_wrapper.h"
#include "yb/util/jsonwriter.h"
#include "yb/gutil/map-util.h"
#include "yb/common/ybc-internal.h"
#include "yb/util/metrics.h"
#include "yb/util/signal_util.h"

DECLARE_string(metric_node_name);

using yb::Webserver;
using yb::JsonWriter;
using yb::PrometheusWriter;
using yb::WebCallbackRegistry;

static ybpgmEntry *ybpgm_table;
static int ybpgm_num_entries;
static int *num_backends;
yb::MetricEntity::AttributeMap prometheus_attr;
static void (*pullYsqlStatementStats)(void *);
static void (*resetYsqlStatementStats)();
static rpczEntry **rpczResultPointer;

static postgresCallbacks pgCallbacks;
static void PgMetricsHandler(const Webserver::WebRequest& req,
                             Webserver::WebResponse* resp) {
  std::stringstream *output = &resp->output;
  JsonWriter::Mode json_mode;
  string arg = FindWithDefault(req.parsed_args, "compact", "false");
  json_mode = ParseLeadingBoolValue(arg.c_str(), false) ?
              JsonWriter::COMPACT : JsonWriter::PRETTY;

  JsonWriter writer(output, json_mode);
  writer.StartArray();
  writer.StartObject();
  writer.String("type");
  writer.String("server");
  writer.String("id");
  writer.String("yb.ysqlserver");
  writer.String("metrics");
  writer.StartArray();

  for (const auto* entry = ybpgm_table, *end = entry + ybpgm_num_entries; entry != end; ++entry) {
    writer.StartObject();
    writer.String("name");
    writer.String(entry->name);
    writer.String("count");
    writer.Int64(entry->calls);
    writer.String("sum");
    writer.Int64(entry->total_time);
    writer.String("rows");
    writer.Int64(entry->rows);
    writer.EndObject();
  }

  writer.EndArray();
  writer.EndObject();
  writer.EndArray();
}

static void DoWriteStatArrayElemToJson(JsonWriter* writer, YsqlStatementStat* stat) {
  writer->String("query");
  writer->String(stat->query);

  writer->String("calls");
  writer->Int64(stat->calls);

  writer->String("total_time");
  writer->Double(stat->total_time);

  writer->String("min_time");
  writer->Double(stat->min_time);

  writer->String("max_time");
  writer->Double(stat->max_time);

  writer->String("mean_time");
  writer->Double(stat->mean_time);

  writer->String("stddev_time");
  // Based on logic in pg_stat_statements_internal().
  double stddev = (stat->calls > 1) ?
    (sqrt(stat->sum_var_time / stat->calls)) : 0.0;
  writer->Double(stddev);

  writer->String("rows");
  writer->Int64(stat->rows);
}

static void PgStatStatementsHandler(const Webserver::WebRequest& req,
                                    Webserver::WebResponse* resp) {
  std::stringstream *output = &resp->output;
  JsonWriter::Mode json_mode;
  string arg = FindWithDefault(req.parsed_args, "compact", "false");
  json_mode = ParseLeadingBoolValue(arg.c_str(), false) ?
              JsonWriter::COMPACT : JsonWriter::PRETTY;
  JsonWriter writer(output, json_mode);

  writer.StartObject();

  writer.String("statements");
  if (pullYsqlStatementStats) {
    writer.StartArray();
    pullYsqlStatementStats(&writer);
    writer.EndArray();
  } else {
    writer.String("PG Stat Statements module is disabled.");
  }

  writer.EndObject();
}

static void PgStatStatementsResetHandler(const Webserver::WebRequest& req,
                                         Webserver::WebResponse* resp) {
  std::stringstream *output = &resp->output;
  JsonWriter::Mode json_mode;
  string arg = FindWithDefault(req.parsed_args, "compact", "false");
  json_mode = ParseLeadingBoolValue(arg.c_str(), false) ?
              JsonWriter::COMPACT : JsonWriter::PRETTY;
  JsonWriter writer(output, json_mode);

  writer.StartObject();

  writer.String("statements");
  if (resetYsqlStatementStats) {
    resetYsqlStatementStats();
    writer.String("PG Stat Statements reset.");
  } else {
    writer.String("PG Stat Statements module is disabled.");
  }

  writer.EndObject();
}

static void WriteAsJsonTimestampAndRunningForMs(JsonWriter *writer, const std::string& prefix,
                                                int64 start_timestamp, int64 snapshot_timestamp,
                                                bool active) {
  writer->String(prefix + "_start_time");
  writer->String(pgCallbacks.getTimestampTzToStr(start_timestamp));

  if (!active) {
    return;
  }

  writer->String(prefix + "_running_for_ms");
  writer->Int64(pgCallbacks.getTimestampTzDiffMs(start_timestamp, snapshot_timestamp));
}

static void PgRpczHandler(const Webserver::WebRequest& req,
                                Webserver::WebResponse* resp) {
  std::stringstream *output = &resp->output;
  pgCallbacks.pullRpczEntries();
  int64 snapshot_timestamp = pgCallbacks.getTimestampTz();

  JsonWriter::Mode json_mode;
  string arg = FindWithDefault(req.parsed_args, "compact", "false");
  json_mode = ParseLeadingBoolValue(arg.c_str(), false) ?
              JsonWriter::COMPACT : JsonWriter::PRETTY;
  JsonWriter writer(output, json_mode);
  rpczEntry *entry = *rpczResultPointer;

  writer.StartObject();
  writer.String("connections");
  writer.StartArray();
  for (int i = 0; i < *num_backends; ++i, ++entry) {
    if (entry->proc_id > 0) {
      writer.StartObject();
      if (entry->db_oid) {
        writer.String("db_oid");
        writer.Int64(entry->db_oid);
        writer.String("db_name");
        writer.String(entry->db_name);
      }

      if (strlen(entry->query) > 0) {
        writer.String("query");
        writer.String(entry->query);
      }

      WriteAsJsonTimestampAndRunningForMs(&writer, "process",
                                          entry->process_start_timestamp,
                                          snapshot_timestamp, entry->backend_active);

      if (entry->transaction_start_timestamp > 0) {
        WriteAsJsonTimestampAndRunningForMs(&writer, "transaction",
                                            entry->transaction_start_timestamp,
                                            snapshot_timestamp, entry->backend_active);
      }

      if (entry->query_start_timestamp > 0) {
        WriteAsJsonTimestampAndRunningForMs(&writer, "query",
                                            entry->query_start_timestamp,
                                            snapshot_timestamp, entry->backend_active);
      }

      writer.String("application_name");
      writer.String(entry->application_name);
      writer.String("backend_type");
      writer.String(entry->backend_type);
      writer.String("backend_status");
      writer.String(entry->backend_status);

      if (entry->host) {
        writer.String("host");
        writer.String(entry->host);
      }

      if (entry->port) {
        writer.String("port");
        writer.String(entry->port);
      }

      writer.EndObject();
    }
  }
  writer.EndArray();
  writer.EndObject();
  pgCallbacks.freeRpczEntries();
}

static void PgPrometheusMetricsHandler(const Webserver::WebRequest& req,
                                       Webserver::WebResponse* resp) {
  std::stringstream *output = &resp->output;
  PrometheusWriter writer(output);

  // Max size of ybpgm_table name (100 incl \0 char) + max size of "_count"/"_sum" (6 excl \0).
  char copied_name[106];
  for (int i = 0; i < ybpgm_num_entries; ++i) {
    snprintf(copied_name, sizeof(copied_name), "%s%s", ybpgm_table[i].name, "_count");
    WARN_NOT_OK(writer.WriteSingleEntry(prometheus_attr,
                                        copied_name,
                                        ybpgm_table[i].calls),
                                        "Couldn't write text metrics for Prometheus");
    snprintf(copied_name, sizeof(copied_name), "%s%s", ybpgm_table[i].name, "_sum");
    WARN_NOT_OK(writer.WriteSingleEntry(prometheus_attr,
                                        copied_name,
                                        ybpgm_table[i].total_time),
                                        "Couldn't write text metrics for Prometheus");
  }
}

extern "C" {
  void WriteStatArrayElemToJson(void* p1, void* p2) {
    JsonWriter* writer = static_cast<JsonWriter*>(p1);
    YsqlStatementStat* stat = static_cast<YsqlStatementStat*>(p2);

    writer->StartObject();
    DoWriteStatArrayElemToJson(writer, stat);
    writer->EndObject();
  }

  WebserverWrapper *CreateWebserver(char *listen_addresses, int port) {
    yb::WebserverOptions opts;
    opts.bind_interface = listen_addresses;
    opts.port = port;
    // Important! Since postgres functions aren't generally thread-safe,
    // we shouldn't allow more than one worker thread at a time.
    opts.num_worker_threads = 1;
    return reinterpret_cast<WebserverWrapper *> (new Webserver(opts, "Postgres webserver"));
  }

  void RegisterMetrics(ybpgmEntry *tab, int num_entries, char *metric_node_name) {
    ybpgm_table = tab;
    ybpgm_num_entries = num_entries;

    prometheus_attr["exported_instance"] = metric_node_name;
    prometheus_attr["metric_type"] = "server";
    prometheus_attr["metric_id"] = "yb.ysqlserver";
  }

  void RegisterGetYsqlStatStatements(void (*getYsqlStatementStats)(void *)) {
    pullYsqlStatementStats = getYsqlStatementStats;
  }

  void RegisterResetYsqlStatStatements(void (*fn)()) {
    resetYsqlStatementStats = fn;
  }

  void RegisterRpczEntries(postgresCallbacks *callbacks, int *num_backends_ptr,
                           rpczEntry **rpczEntriesPointer) {
    pgCallbacks = *callbacks;
    num_backends = num_backends_ptr;
    rpczResultPointer = rpczEntriesPointer;
  }

  YBCStatus StartWebserver(WebserverWrapper *webserver_wrapper) {
    Webserver *webserver = reinterpret_cast<Webserver *> (webserver_wrapper);
    webserver->RegisterPathHandler("/metrics", "Metrics", PgMetricsHandler, false, false);
    webserver->RegisterPathHandler("/jsonmetricz", "Metrics", PgMetricsHandler, false, false);
    webserver->RegisterPathHandler("/prometheus-metrics", "Metrics", PgPrometheusMetricsHandler,
                                   false, false);
    webserver->RegisterPathHandler("/rpcz", "RPCs in progress", PgRpczHandler, false, false);
    webserver->RegisterPathHandler("/statements", "PG Stat Statements", PgStatStatementsHandler,
        false, false);
    webserver->RegisterPathHandler("/statements-reset", "Reset PG Stat Statements",
        PgStatStatementsResetHandler, false, false);
    return ToYBCStatus(yb::WithMaskedYsqlSignals([webserver]() {
      return webserver->Start();
    }));
  }
};
