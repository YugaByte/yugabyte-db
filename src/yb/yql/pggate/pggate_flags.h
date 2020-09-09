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

#ifndef YB_YQL_PGGATE_PGGATE_FLAGS_H
#define YB_YQL_PGGATE_PGGATE_FLAGS_H

#include <gflags/gflags.h>

DECLARE_int32(pgsql_rpc_keepalive_time_ms);
DECLARE_int32(pggate_rpc_timeout_secs);
DECLARE_int32(pggate_ybclient_reactor_threads);
DECLARE_string(pggate_proxy_bind_address);
DECLARE_string(pggate_master_addresses);
DECLARE_int32(pggate_tserver_shm_fd);
DECLARE_bool(TEST_pggate_ignore_tserver_shm);
DECLARE_int32(ysql_request_limit);
DECLARE_int32(ysql_prefetch_limit);
DECLARE_double(ysql_backward_prefetch_scale_factor);
DECLARE_int32(ysql_session_max_batch_size);
DECLARE_bool(ysql_non_txn_copy);
DECLARE_int32(ysql_max_read_restart_attempts);
DECLARE_bool(TEST_ysql_disable_transparent_cache_refresh_retry);
DECLARE_int32(ysql_output_buffer_size);
DECLARE_int32(ysql_select_parallelism);

DECLARE_bool(ysql_suppress_unsupported_error);

DECLARE_bool(ysql_beta_features);
DECLARE_bool(ysql_beta_feature_extension);
DECLARE_bool(ysql_beta_feature_tablegroup);
DECLARE_bool(ysql_enable_manual_sys_table_txn_ctl);
DECLARE_bool(ysql_serializable_isolation_for_ddl_txn);

#endif  // YB_YQL_PGGATE_PGGATE_FLAGS_H
