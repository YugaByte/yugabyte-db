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
# server_common
#########################################

set(SERVER_COMMON_SRCS
  hybrid_clock.cc
  logical_clock.cc
)

add_library(server_common ${SERVER_COMMON_SRCS})
target_link_libraries(server_common
  yb_common
  gutil
  yb_fs
  yb_util
  consensus_metadata_proto)

#########################################
# server_common tests
#########################################

set(YB_TEST_LINK_LIBS server_common ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(hybrid_clock-test)
ADD_YB_TEST(logical_clock-test)
# This test is here and not in common because we need access to HybridClock.
ADD_YB_TEST(doc_hybrid_time-test)

#########################################
# server_base_proto
#########################################

YRPC_GENERATE(
  SERVER_BASE_PROTO_SRCS SERVER_BASE_PROTO_HDRS SERVER_BASE_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES server_base.proto)

add_library(server_base_proto ${SERVER_BASE_PROTO_SRCS} ${SERVER_BASE_PROTO_HDRS})
target_link_libraries(server_base_proto
  yrpc
  yb_common_proto
  protobuf
  rpc_header_proto
  version_info_proto
  wire_protocol_proto)

#########################################
# server_process
#########################################

set(SERVER_PROCESS_SRCS
  default-path-handlers.cc
  generic_service.cc
  glog_metrics.cc
  pprof-path-handlers.cc
  rpcz-path-handler.cc
  rpc_server.cc
  server_base.cc
  server_base_options.cc
  skewed_clock.cc
  tcmalloc_metrics.cc
  tracing-path-handlers.cc
  webserver.cc
  pgsql_webserver_wrapper.cc
  webserver_options.cc
  webui_util.cc
  total_mem_watcher.cc
  ${RPC_SRCS_EXTENSIONS}
)

add_library(server_process ${SERVER_PROCESS_SRCS})
target_link_libraries(server_process
  server_base_proto
  server_common
  yb_common
  yb_fs
  gutil
  yrpc
  yb_util
  squeasel)

# This module depends on tcmalloc and profiler directly, so need to make
# sure that they get linked in the right order.
if("${YB_TCMALLOC_ENABLED}" STREQUAL "1")
  message("Adding tcmalloc as a dependency of server_process")
  target_link_libraries(server_process tcmalloc profiler)
else()
  message("Not adding tcmalloc as a dependency of server_process")
endif()

#########################################
# server_process tests
#########################################

set(YB_TEST_LINK_LIBS server_process ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(webserver-test)
