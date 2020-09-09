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

#########################################
# remote_bootstrap_proto
#########################################

YRPC_GENERATE(
  REMOTE_BOOTSTRAP_YRPC_SRCS REMOTE_BOOTSTRAP_YRPC_HDRS REMOTE_BOOTSTRAP_YRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES remote_bootstrap.proto)
set(REMOTE_BOOTSTRAP_YRPC_LIBS
  consensus_proto
  yrpc
  protobuf
  rpc_header_proto
  tablet_proto
  wire_protocol_proto)
ADD_YB_LIBRARY(remote_bootstrap_proto
  SRCS ${REMOTE_BOOTSTRAP_YRPC_SRCS}
  DEPS ${REMOTE_BOOTSTRAP_YRPC_LIBS}
  NONLINK_DEPS ${REMOTE_BOOTSTRAP_YRPC_TGTS})

#########################################
# backup_proto
#########################################

YRPC_GENERATE(
  BACKUP_YRPC_SRCS BACKUP_YRPC_HDRS BACKUP_YRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES backup.proto)
set(BACKUP_YRPC_LIBS
  yrpc
  tserver_proto)
ADD_YB_LIBRARY(backup_proto
  SRCS ${BACKUP_YRPC_SRCS}
  DEPS ${BACKUP_YRPC_LIBS}
  NONLINK_DEPS ${BACKUP_YRPC_TGTS})

#########################################
# tserver_proto
#########################################

PROTOBUF_GENERATE_CPP(
  TSERVER_PROTO_SRCS TSERVER_PROTO_HDRS TSERVER_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES tserver.proto)
set(TSERVER_PROTO_LIBS
  yb_common_proto
  consensus_metadata_proto
  tablet_proto
  wire_protocol_proto
  redis_protocol_proto
  ql_protocol_proto
  docdb_proto)
ADD_YB_LIBRARY(tserver_proto
  SRCS ${TSERVER_PROTO_SRCS}
  DEPS ${TSERVER_PROTO_LIBS}
  NONLINK_DEPS ${TSERVER_PROTO_TGTS})

#########################################
# tserver_admin_proto
#########################################

YRPC_GENERATE(
  TSERVER_ADMIN_YRPC_SRCS TSERVER_ADMIN_YRPC_HDRS TSERVER_ADMIN_YRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES tserver_admin.proto)
set(TSERVER_ADMIN_YRPC_LIBS
  yrpc
  protobuf
  rpc_header_proto
  tserver_proto
  wire_protocol_proto)
ADD_YB_LIBRARY(tserver_admin_proto
  SRCS ${TSERVER_ADMIN_YRPC_SRCS}
  DEPS ${TSERVER_ADMIN_YRPC_LIBS}
  NONLINK_DEPS ${TSERVER_ADMIN_YRPC_TGTS})

#########################################
# tserver_service_proto
#########################################

YRPC_GENERATE(
  TSERVER_YRPC_SRCS TSERVER_YRPC_HDRS TSERVER_YRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES tserver_service.proto)
set(TSERVER_YRPC_LIBS
  yrpc
  yb_common_proto
  protobuf
  remote_bootstrap_proto
  rpc_header_proto
  tserver_proto
  wire_protocol_proto)
ADD_YB_LIBRARY(tserver_service_proto
  SRCS ${TSERVER_YRPC_SRCS}
  DEPS ${TSERVER_YRPC_LIBS}
  NONLINK_DEPS ${TSERVER_YRPC_TGTS})

#########################################
# tserver_util
#########################################

set(TSERVER_UTIL_SRCS
  tserver_flags.cc
  tserver_error.cc)
set(TSERVER_UTIL_LIBS
  yb_util)
ADD_YB_LIBRARY(tserver_util
  SRCS ${TSERVER_UTIL_SRCS}
  DEPS ${TSERVER_UTIL_LIBS})
add_dependencies(tserver_util gen_proto)

#########################################
# tserver
#########################################

set(TSERVER_SRCS
  heartbeater.cc
  heartbeater_factory.cc
  metrics_snapshotter.cc
  mini_tablet_server.cc
  remote_bootstrap_client.cc
  remote_bootstrap_file_downloader.cc
  remote_bootstrap_service.cc
  remote_bootstrap_session.cc
  remote_bootstrap_snapshots.cc
  service_util.cc
  tablet_server.cc
  tablet_server_options.cc
  tablet_service.cc
  tablet_split_heartbeat_data_provider.cc
  ts_tablet_manager.cc
  tserver-path-handlers.cc
  tserver_metrics_heartbeat_data_provider.cc
  ${TSERVER_SRCS_EXTENSIONS})

add_library(tserver ${TSERVER_SRCS})
target_link_libraries(tserver
  protobuf
  backup_proto
  tserver_proto
  tserver_admin_proto
  tserver_service_proto
  remote_bootstrap_proto
  master_rpc
  master_proto
  consensus_proto
  log_proto
  log
  consensus
  yrpc
  server_common
  server_process
  tablet
  yb_client
  yb_pggate_flags
  ${TSERVER_LIB_EXTENSIONS})

#########################################
# yb-tserver
#########################################

add_executable(yb-tserver tablet_server_main.cc)
target_link_libraries(yb-tserver
  tserver
  yb-cql
  yb-redis
  yb_pgwrapper
  master
  ${YB_BASE_LIBS})

#########################################
# tserver_test_util
#########################################

add_library(tserver_test_util
  tablet_server-test-base.cc
  tablet_server_test_util.cc
  universe_key_test_util.cc)
target_link_libraries(tserver_test_util tablet_test_util tserver yb_test_util)

#########################################
# tserver tests
#########################################

set(YB_TEST_LINK_LIBS
  rpc_test_util
  tserver
  tserver_test_util
  yb_client # yb::client::YBTableName
  tablet_test_util
  ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(remote_bootstrap_rocksdb_client-test)
ADD_YB_TEST(remote_bootstrap_rocksdb_session-test)
ADD_YB_TEST(remote_bootstrap_service-test)
ADD_YB_TEST(tablet_server-test)
ADD_YB_TEST(tablet_server-stress-test RUN_SERIAL true)
ADD_YB_TEST(ts_tablet_manager-test)
ADD_YB_TEST(header_manager_impl-test)

ADD_YB_TEST(encrypted_sstable-test)
YB_TEST_TARGET_LINK_LIBRARIES(encrypted_sstable-test encryption_test_util tserver_test_util tserver)

# -------------------------------------------------------------------------------------------------
# Tests in the "ent" directory
# -------------------------------------------------------------------------------------------------

if(YB_ENT_CURRENT_SOURCE_DIR)
  # Set the test source file folder.
  set(CMAKE_CURRENT_LIST_DIR ${YB_ENT_CURRENT_SOURCE_DIR})

  foreach(test ${TSERVER_EXTENSIONS_TESTS})
    ADD_YB_TEST(${test})
  endforeach()
endif()
