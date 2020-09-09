# Copyright (c) YugaByte, Inc.
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

#########################################
# cqlserver_service_proto
#########################################

YRPC_GENERATE(
  CQLSERVER_YRPC_SRCS CQLSERVER_YRPC_HDRS CQLSERVER_YRPC_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../../../..
  PROTO_FILES cql_service.proto)
set(CQLSERVER_YRPC_LIBS
  yrpc
  yb_common_proto
  protobuf
  remote_bootstrap_proto
  rpc_header_proto
  wire_protocol_proto
  ${OPENSSL_CRYPTO_LIBRARY})
ADD_YB_LIBRARY(cql_service_proto
  SRCS ${CQLSERVER_YRPC_SRCS}
  DEPS ${CQLSERVER_YRPC_LIBS}
  NONLINK_DEPS ${CQLSERVER_YRPC_TGTS})

#########################################
# CQL server
#########################################

set(CQLSERVER_SRCS
  cql_message.cc
  cql_processor.cc
  cql_rpc.cc
  cql_server.cc
  cql_server_options.cc
  cql_service.cc
  cql_statement.cc
  system_query_cache.cc
)

add_library(yb-cql ${CQLSERVER_SRCS})
target_link_libraries(yb-cql
  ql_api
  protobuf
  log
  yrpc
  yb_client
  cql_service_proto
  server_common
  server_process
  lz4
  snappy)

# Tests
set(YB_TEST_LINK_LIBS yb-cql integration-tests ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(cqlserver-test)
