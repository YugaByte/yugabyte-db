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
//--------------------------------------------------------------------------------------------------

// This file contains flag definitions that should be known to master, tserver, and pggate
// (linked into postgres).

#include <gflags/gflags.h>

#include "yb/util/flags.h"
#include "yb/util/flag_tags.h"
#include "yb/yql/pggate/pggate_flags.h"

DEFINE_int32(pgsql_rpc_keepalive_time_ms, 0,
             "If an RPC connection from a client is idle for this amount of time, the server "
             "will disconnect the client. Setting flag to 0 disables this clean up.");
TAG_FLAG(pgsql_rpc_keepalive_time_ms, advanced);

DEFINE_int32(pggate_rpc_timeout_secs, 60,
             "Timeout for RPCs from pggate to YB cluster");

DEFINE_int32(pggate_ybclient_reactor_threads, 2,
             "The number of reactor threads to be used for processing ybclient "
             "requests originating in the PostgreSQL proxy server");

DEFINE_string(pggate_proxy_bind_address, "",
              "Address to which the PostgreSQL proxy server is bound.");

DEFINE_string(pggate_master_addresses, "",
              "Addresses of the master servers to which the PostgreSQL proxy server connects.");

DEFINE_int32(pggate_tserver_shm_fd, -1,
              "File descriptor of the local tablet server's shared memory.");

DEFINE_test_flag(bool, pggate_ignore_tserver_shm, false,
              "Ignore the shared memory of the local tablet server.");

DEFINE_int32(ysql_request_limit, 1024,
             "Maximum number of requests to be sent at once");

DEFINE_int32(ysql_prefetch_limit, 1024,
             "Maximum number of rows to prefetch");

DEFINE_double(ysql_backward_prefetch_scale_factor, 0.0625 /* 1/16th */,
              "Scale factor to reduce ysql_prefetch_limit for backward scan");

DEFINE_int32(ysql_session_max_batch_size, 512,
             "Maximum batch size for buffered writes between PostgreSQL server and YugaByte DocDB "
             "services");

DEFINE_bool(ysql_non_txn_copy, false,
            "Execute COPY inserts non-transactionally.");

DEFINE_int32(ysql_max_read_restart_attempts, 20,
             "How many read restarts can we try transparently before giving up");

DEFINE_test_flag(bool, ysql_disable_transparent_cache_refresh_retry, false,
    "Never transparently retry commands that fail with cache version mismatch error");

DEFINE_int32(ysql_output_buffer_size, 262144,
             "Size of postgres-level output buffer, in bytes. "
             "While fetched data resides within this buffer and hasn't been flushed to client yet, "
             "we're free to transparently restart operation in case of restart read error.");

DEFINE_bool(ysql_suppress_unsupported_error, false,
            "Suppress ERROR on use of unsupported SQL statement and use WARNING instead");

// Top-level flag to enable all YSQL beta features.
DEFINE_bool(ysql_beta_features, false,
            "Whether to enable all ysql beta features");

// Per-feature flags -- only relevant if ysql_beta_features is false.

DEFINE_bool(ysql_beta_feature_extension, false,
            "Whether to enable the 'extension' ysql beta feature");

DEFINE_bool(ysql_beta_feature_tablegroup, true,
            "Whether to enable the incomplete 'tablegroup' ysql beta feature");

TAG_FLAG(ysql_beta_feature_tablegroup, hidden);

DEFINE_bool(ysql_enable_manual_sys_table_txn_ctl, false,
            "Enable manual transaction control for YSQL system tables. Mostly needed for testing. "
            "This flag should go away once full transactional DDL is implemented.");

DEFINE_bool(ysql_serializable_isolation_for_ddl_txn, false,
            "Whether to use serializable isolation for separate DDL-only transactions. "
            "By default, repeatable read isolation is used. "
            "This flag should go away once full transactional DDL is implemented.");

DEFINE_int32(ysql_select_parallelism, -1,
            "Number of read requests to issue in parallel to tablets of a table "
            "for SELECT.");
