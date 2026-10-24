// Copyright (c) YugabyteDB, Inc.
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

// YB: include guard instead of pragma once: this header is installed into
// the PostgreSQL server include directory, and pragma once does not
// deduplicate identical copies of a header visible via two paths.
#ifndef YB_YQL_PGGATE_WEBSERVER_YBC_PG_WEBSERVER_WRAPPER_H
#define YB_YQL_PGGATE_WEBSERVER_YBC_PG_WEBSERVER_WRAPPER_H

#include <sys/types.h>
#ifdef __cplusplus
#include <atomic>
using int64 = int64_t;
using uint8 = uint8_t;
using uint64 = uint64_t;
#else
#include <stdatomic.h>
#endif

#include "yb/yql/pggate/util/ybc_util.h"

#ifdef __cplusplus
namespace yb::pggate {
extern "C" {
#define YB_ATOMIC_ULLONG std::atomic_ullong
#else
#define YB_ATOMIC_ULLONG atomic_ullong
#endif

struct WebserverWrapper;

#define YB_PG_METRIC_NAME_LEN 120

#define YB_PG_METRIC_NAME_LEN 120

typedef void (*YbcDbCatalogCacheMetricVisitor)(unsigned int db_oid,
                                              const char *metric_name,
                                              const char *table_name,
                                              uint64_t count,
                                              void *arg);
typedef void (*YbcIterateDbCatalogCacheMetricsFunc)(YbcDbCatalogCacheMetricVisitor visitor,
                                                   void *arg);

typedef struct YbcPgmEntry {
  char name[YB_PG_METRIC_NAME_LEN];
  char table_name[YB_PG_METRIC_NAME_LEN];
  char count_help[YB_PG_METRIC_NAME_LEN];
  char sum_help[YB_PG_METRIC_NAME_LEN];
  YB_ATOMIC_ULLONG calls;
  YB_ATOMIC_ULLONG total_time;
  YB_ATOMIC_ULLONG rows;
} YbcPgmEntry;

typedef struct YbcRpczEntry {
  char *query;
  char *application_name;
  int proc_id;
  unsigned int db_oid;
  char *db_name;
  int64 process_start_timestamp;
  int64 transaction_start_timestamp;
  int64 query_start_timestamp;
  uint64 query_id;
  pid_t leader_pid;
  char *backend_type;
  uint8 backend_active;
  char *backend_status;
  char *host;
  char *port;
} YbcRpczEntry;

typedef struct {
  void (*pullRpczEntries)();
  void (*freeRpczEntries)();
  int64 (*getTimestampTz)();
  int64 (*getTimestampTzDiffMs)(int64, int64);
  const char *(*getTimestampTzToStr)(int64);
} YbcPostgresCallbacks;

typedef struct {
  /* # of connections rejected due to the connection limit. */
  int *too_many_conn;

  /* maximum # of concurrent sql connections allowed. */
  int *max_conn;

  /* # of connections established since start of postmaster. */
  uint64_t *new_conn;
} YbcConnectionMetrics;

struct WebserverWrapper *CreateWebserver(char *listen_addresses, int port);
void DestroyWebserver(struct WebserverWrapper *webserver);
void RegisterMetrics(YbcPgmEntry *tab, int num_entries, char *metric_node_name);
void RegisterDbCatalogCacheMetrics(YbcIterateDbCatalogCacheMetricsFunc fn);
void RegisterRpczEntries(
    YbcPostgresCallbacks *callbacks, int *num_backends_ptr, YbcRpczEntry **rpczEntriesPointer,
    YbcConnectionMetrics *conn_metrics_ptr);
YbcStatus StartWebserver(struct WebserverWrapper *webserver);
void SetWebserverConfig(
    struct WebserverWrapper *webserver,
    bool enable_access_logging, bool enable_tcmalloc_logging,
    int webserver_profiler_sample_period_bytes);
void RegisterGetYsqlStatStatements(void (*getYsqlStatementStats)(void *));
void RegisterResetYsqlStatStatements(void (*fn)());
void WriteStartObjectToJson(void *p1); /* Takes void *cb_arg argument */
void WriteArrayBeginToJson(void *p1, const char* key);
/* Takes void *cb_arg, char *buf, int64_t *count arguments */
void WriteIntValueObjectToJson(void *p1, void *p2, void *p3);
void WriteArrayEndToJson(void* p1); /* Takes void *cb_arg argument */
void WriteEndObjectToJson(void *p1); /* Takes void *cb_arg argument */
void WriteStringToJson(void *p1, const char* key, const char* value);
// TODO: Add functions for writing primitve types, objects and nested arrays into Json
// arrays as required. Current set of functions are bare minimum
// required for current usecase and writing arrays of primitive types.
void WriteIntToJson(void *p1, const char* key, const int64_t value);
void WriteDoubleToJson(void *p1, const char* key, const double value);
void WriteBoolToJson(void *p1, const char* key, const bool value);
void WriteIntArrayToJson(void *p1, const char *key, const int64 *values, const size_t size);
void WriteDoubleArrayToJson(void *p1, const char *key, const double *values, const size_t size);

#ifdef __cplusplus
}  // extern "C"
}  // namespace yb::pggate
#endif

#endif  // YB_YQL_PGGATE_WEBSERVER_YBC_PG_WEBSERVER_WRAPPER_H
