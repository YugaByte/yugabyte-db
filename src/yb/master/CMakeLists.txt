# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# The following only applies to changes made to this file as part of YugaByte development.
#
# Portions Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

YB_INCLUDE_EXTENSIONS()

YRPC_GENERATE(
  MASTER_YRPC_SRCS MASTER_YRPC_HDRS MASTER_YRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES master.proto)
set(MASTER_YRPC_LIBS
  consensus_metadata_proto
  yrpc
  protobuf
  rpc_header_proto
  tablet_proto
  tserver_proto
  wire_protocol_proto
  yb_pgwrapper
  cdc_consumer_proto)
ADD_YB_LIBRARY(master_proto
  SRCS ${MASTER_YRPC_SRCS}
  DEPS ${MASTER_YRPC_LIBS}
  NONLINK_DEPS ${MASTER_YRPC_TGTS})

set(MASTER_PROTO_LIBS
  master_proto
  ${MASTER_PROTO_LIBS_EXTENSIONS})

set(MASTER_SRCS
  async_flush_tablets_task.cc
  async_rpc_tasks.cc
  call_home.cc
  backfill_index.cc
  catalog_manager.cc
  catalog_manager_util.cc
  catalog_entity_info.cc
  catalog_manager_bg_tasks.cc
  catalog_loaders.cc
  scoped_leader_shared_lock.cc
  cluster_balance.cc
  encryption_manager.cc
  permissions_manager.cc
  flush_manager.cc
  master.cc
  master_options.cc
  master_service_base.cc
  master_service.cc
  master_tablet_service.cc
  master_tserver.cc
  master-path-handlers.cc
  mini_master.cc
  master_snapshot_coordinator.cc
  sys_catalog.cc
  sys_catalog_initialization.cc
  sys_catalog_writer.cc
  system_tablet.cc
  tasks_tracker.cc
  ts_descriptor.cc
  ts_manager.cc
  yql_virtual_table.cc
  yql_vtable_iterator.cc
  util/yql_vtable_helpers.cc
  yql_auth_roles_vtable.cc
  yql_auth_role_permissions_vtable.cc
  yql_auth_resource_role_permissions_index.cc
  yql_aggregates_vtable.cc
  yql_columns_vtable.cc
  yql_empty_vtable.cc
  yql_functions_vtable.cc
  yql_indexes_vtable.cc
  yql_keyspaces_vtable.cc
  yql_peers_vtable.cc
  yql_local_vtable.cc
  yql_size_estimates_vtable.cc
  yql_tables_vtable.cc
  yql_triggers_vtable.cc
  yql_types_vtable.cc
  yql_views_vtable.cc
  yql_partitions_vtable.cc
  ${MASTER_SRCS_EXTENSIONS})

add_library(master ${MASTER_SRCS})
target_link_libraries(master
  yb_common
  tablet
  server_common
  server_process
  yrpc
  gutil
  yb_util
  tserver
  tserver_service_proto
  ${MASTER_PROTO_LIBS}
  rpc_header_proto
  master_util
  version_info_proto
  yb_pggate_flags)

set(MASTER_RPC_SRCS
  master_rpc.cc)
set(MASTER_RPC_LIBS
  yb_common
  yrpc
  gutil
  yb_util
  ${MASTER_PROTO_LIBS}
  rpc_header_proto)
ADD_YB_LIBRARY(master_rpc
  SRCS ${MASTER_RPC_SRCS}
  DEPS ${MASTER_RPC_LIBS})

set(MASTER_ERROR_SRCS
  master_error.cc)
ADD_YB_LIBRARY(master_error
  SRCS ${MASTER_ERROR_SRCS}
  DEPS ${MASTER_RPC_LIBS})

set(MASTER_UTIL_SRCS
  master_util.cc)
ADD_YB_LIBRARY(master_util
  SRCS ${MASTER_UTIL_SRCS}
  DEPS master_error ${MASTER_RPC_LIBS})

# A library with common code shared between master tests.
set(MASTER_TEST_COMMON_SRCS
  master-test_base.cc)
add_library(master_test_common ${MASTER_TEST_COMMON_SRCS})
target_link_libraries(master_test_common
  master ${MASTER_PROTO_LIBS} yb_client ${YB_MIN_TEST_LIBS})

# Tests
set(YB_TEST_LINK_LIBS master
  ${MASTER_PROTO_LIBS}
  integration-tests
  master_test_common
  cql_test_util
  yb_client
  yb_tools_util
  ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(catalog_manager-test)
ADD_YB_TEST(flush_manager-test)
ADD_YB_TEST(master-test)
ADD_YB_TEST(sys_catalog-test)

foreach(ADDITIONAL_TEST ${MASTER_ADDITIONAL_TESTS})
  ADD_YB_TEST(${ADDITIONAL_TEST})
endforeach(ADDITIONAL_TEST)

# Actual master executable
add_executable(yb-master master_main.cc)
target_link_libraries(yb-master
  master
  ${YB_BASE_LIBS})
