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

#ifndef YB_SERVER_PGSQL_WEBSERVER_WRAPPER_H
#define YB_SERVER_PGSQL_WEBSERVER_WRAPPER_H

#ifdef __cplusplus
#include <atomic>
using std::atomic_ullong;
#else
#include <stdatomic.h>
#endif

#include "yb/common/ybc_util.h"

#ifdef __cplusplus
extern "C" {
#endif

struct WebserverWrapper;

typedef struct ybpgmEntry {
    char name[100];
    atomic_ullong calls;
    atomic_ullong total_time;
    atomic_ullong rows;
} ybpgmEntry;

typedef struct rpczEntry {
    char *query;
    char *application_name;
    int proc_id;
    unsigned int db_oid;
    char *db_name;
    int64 process_start_timestamp;
    int64 transaction_start_timestamp;
    int64 query_start_timestamp;
    char *backend_type;
    uint8 backend_active;
    char *backend_status;
    char *host;
    char *port;
} rpczEntry;

typedef struct YsqlStatementStat {
  char   *query;

  // Prefix of Counters in pg_stat_statements.c.

  int64  calls;        /* # of times executed */
  double total_time;   /* total execution time, in msec */
  double min_time;     /* minimum execution time in msec */
  double max_time;     /* maximum execution time in msec */
  double mean_time;    /* mean execution time in msec */
  double sum_var_time; /* sum of variances in execution time in msec */
  int64  rows;         /* total # of retrieved or affected rows */
} YsqlStatementStat;

typedef struct {
  void (*pullRpczEntries)();
  void (*freeRpczEntries)();
  int64 (*getTimestampTz)();
  int64 (*getTimestampTzDiffMs)(int64, int64);
  const char *(*getTimestampTzToStr)(int64);
} postgresCallbacks;

struct WebserverWrapper *CreateWebserver(char *listen_addresses, int port);
void RegisterMetrics(ybpgmEntry *tab, int num_entries, char *metric_node_name);
void RegisterRpczEntries(postgresCallbacks *callbacks, int *num_backends_ptr,
                         rpczEntry **rpczEntriesPointer);
YBCStatus StartWebserver(struct WebserverWrapper *webserver);
void RegisterGetYsqlStatStatements(void (*getYsqlStatementStats)(void *));
void RegisterResetYsqlStatStatements(void (*fn)());
void WriteStatArrayElemToJson(void* p1, void* p2);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // YB_SERVER_PGSQL_WEBSERVER_WRAPPER_H
