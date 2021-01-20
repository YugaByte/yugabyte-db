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

set(PGWRAPPER_SRCS
    pg_wrapper.cc)
set(PGWRAPPER_LIBS
    yb_common
    yb_pggate_flags
    yb_util)
ADD_YB_LIBRARY(yb_pgwrapper
               SRCS ${PGWRAPPER_SRCS}
               DEPS ${PGWRAPPER_LIBS})

set(PQ_UTILS_SRCS
    libpq_utils.cc)
set(PQ_UTILS_DEPS
    pgsql_protocol_proto
    pq
    ql_protocol_proto
    yb_common_proto
    yb_pggate_flags)
ADD_YB_LIBRARY(pq_utils
               SRCS ${PQ_UTILS_SRCS}
               DEPS ${PQ_UTILS_DEPS})

set(PG_WRAPPER_TEST_BASE_SRCS
    libpq_test_base.cc
    pg_mini_test_base.cc
    pg_wrapper_test_base.cc)
set(PG_WRAPPER_TEST_BASE_DEPS
    pq_utils)
ADD_YB_LIBRARY(pg_wrapper_test_base
               SRCS ${PG_WRAPPER_TEST_BASE_SRCS}
               DEPS ${PG_WRAPPER_TEST_BASE_DEPS})

target_link_libraries(pg_wrapper_test_base integration-tests)

set(YB_TEST_LINK_LIBS yb_pgwrapper yb_client ql-dml-test-base pg_wrapper_test_base
   ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(pg_wrapper-test)
ADD_YB_TEST(pg_libpq-test)
ADD_YB_TEST(pg_libpq_err-test)
ADD_YB_TEST(pg_on_conflict-test)
ADD_YB_TEST(pg_mini-test)
ADD_YB_TEST(pg_txn-test)

# This is really a tool, not a test, but uses a lot of existing test infrastructure.
ADD_YB_TEST(create_initial_sys_catalog_snapshot)
set(CREATE_INITIAL_SYS_CATALOG_SNAPSHOT_TOOL_PATH ${LAST_ADDED_TEST_BINARY_PATH})
set(CREATE_INITIAL_SYS_CATALOG_SNAPSHOT_TARGET_NAME ${LAST_ADDED_TEST_BINARY_NAME})

set(gen_initial_sys_catalog_snapshot_cmd
    "${BUILD_SUPPORT_DIR}/gen_initial_sys_catalog_snapshot_wrapper")

if ("${YB_BUILD_TYPE}" STREQUAL "tsan")
  # Don't build the initial sys catalog snapshot target automatically in TSAN. But still create the
  # target so it could be built explicitly.
  add_custom_target(
    initial_sys_catalog_snapshot  # Don't add "ALL" here.
    COMMAND ${gen_initial_sys_catalog_snapshot_cmd})
else()
  add_custom_target(
    initial_sys_catalog_snapshot ALL
    COMMAND ${gen_initial_sys_catalog_snapshot_cmd})
endif()

add_dependencies(
  initial_sys_catalog_snapshot
  ${CREATE_INITIAL_SYS_CATALOG_SNAPSHOT_TARGET_NAME} yb-master yb-tserver postgres)
