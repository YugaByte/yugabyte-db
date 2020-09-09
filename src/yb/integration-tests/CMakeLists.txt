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

set(INTEGRATION_TESTS_SRCS
  cluster_itest_util.cc
  cluster_verifier.cc
  external_mini_cluster.cc
  external_mini_cluster_fs_inspector.cc
  mini_cluster.cc
  test_workload.cc
  load_generator.cc
  yb_table_test_base.cc
  yb_mini_cluster_test_base.cc
  redis_table_test_base.cc
  ${INTEGRATION_TESTS_SRCS_EXTENSIONS}
)

add_library(integration-tests ${INTEGRATION_TESTS_SRCS})
target_link_libraries(integration-tests
  tserver
  tserver_test_util
  master
  ql_api
  ysck
  yb-admin_lib
  yb_client
  yb_client_test_util
  yb_fs
  yb-redisserver-test
  yb_test_util
  yb-redis
  yb_pgwrapper
  ${INTEGRATION_TESTS_LIB_EXTENSIONS})

add_dependencies(integration-tests
  yb-tserver
  yb-master)

add_library(cdc_test_util cdc_test_util.cc)
target_link_libraries(cdc_test_util yb_test_util cdc_service_proto tserver)

add_library(cql_test_util cql_test_base.cc cql_test_util.cc)
target_link_libraries(cql_test_util cassandra integration-tests yb-cql yb_test_util)

# Tests
set(YB_TEST_LINK_LIBS
    integration-tests yb_client yb_tools_util ql-dml-test-base rpc_test_util ${YB_MIN_TEST_LIBS})

ADD_YB_TEST(alter_table-test)
ADD_YB_TEST(alter_table-randomized-test)
ADD_YB_TEST(registration-test)
ADD_YB_TEST(clock_synchronization-itest)
ADD_YB_TEST(client_failover-itest)
ADD_YB_TEST(client-stress-test)
ADD_YB_TEST(cluster_trace-test)
ADD_YB_TEST(tablet_server-itest)
ADD_YB_TEST(tablet-split-itest)
# Tests which fail on purpose for checking Jenkins test failures reporting, disabled
# (commented out) by default:
# ADD_YB_TEST(test_failures-test)
# ADD_YB_TEST(non_gtest_failures-test.sh)

# Not sure if we really need RUN_SERIAL here as this might not be a resource-intensive test.
ADD_YB_TEST(compaction-test)
ADD_YB_TEST(logging-test)
ADD_YB_TEST(master_replication-itest)
ADD_YB_TEST(master_sysnamespace-itest)
ADD_YB_TEST(master_path_handlers-itest)
ADD_YB_TEST(master_failover-itest)
ADD_YB_TEST(master_config-itest)
ADD_YB_TEST(network_failure-test)
ADD_YB_TEST(system_table_fault_tolerance)
ADD_YB_TEST(raft_consensus-itest)
ADD_YB_TEST(flush-test)
ADD_YB_TEST(ts_tablet_manager-itest)
ADD_YB_TEST(ts_recovery-itest)
ADD_YB_TEST(create-table-stress-test)
ADD_YB_TEST(master-partitioned-test)
ADD_YB_TEST(delete_table-test)
ADD_YB_TEST(external_mini_cluster-test)
ADD_YB_TEST(linked_list-test)
ADD_YB_TEST(all_types-itest)
ADD_YB_TEST(remote_bootstrap-itest)
ADD_YB_TEST(tablet_replacement-itest)
ADD_YB_TEST(create-table-itest)
ADD_YB_TEST(placement_info-itest)
ADD_YB_TEST(kv_table-test)
ADD_YB_TEST(kv_table_ts_failover-test)
ADD_YB_TEST(kv_table_ts_failover_write_if-test)
ADD_YB_TEST(flush_under_load-test)
ADD_YB_TEST(stepdown_under_load-test)
ADD_YB_TEST(full_stack-insert-scan-test)
ADD_YB_TEST(redis_table-test)
ADD_YB_TEST(update_scan_delta_compact-test)
ADD_YB_TEST(log_version-test)
ADD_YB_TEST(load_balancer-test)
ADD_YB_TEST(load_balancer_multi_table-test)
ADD_YB_TEST(load_balancer_respect_affinity-test)

set(YB_TEST_LINK_LIBS_SAVED ${YB_TEST_LINK_LIBS})
set(YB_TEST_LINK_LIBS ${YB_TEST_LINK_LIBS} cassandra cql_test_util)
set(CASS_DRIVER_VERSION "2.9.0-yb-4")
set(CASS_DRIVER_INTERNAL_INCLUDE
    "${YB_THIRDPARTY_DIR}/src/cassandra-cpp-driver-${CASS_DRIVER_VERSION}/src")

ADD_YB_TEST(cassandra_cpp_driver-test)
YB_TEST_TARGET_INCLUDE_DIRECTORIES(cassandra_cpp_driver-test PUBLIC
  ${CASS_DRIVER_INTERNAL_INCLUDE}
  "${CASS_DRIVER_INTERNAL_INCLUDE}/third_party/sparsehash/src")

if("${COMPILER_FAMILY}" STREQUAL "gcc8")
  YB_TEST_TARGET_COMPILE_OPTIONS(cassandra_cpp_driver-test PUBLIC "-Wno-class-memaccess")
endif()

ADD_YB_TEST(cql-index-test)
YB_TEST_TARGET_INCLUDE_DIRECTORIES(cql-index-test PUBLIC ${CASS_DRIVER_INTERNAL_INCLUDE})
YB_TEST_TARGET_LINK_LIBRARIES(cql-index-test yb-cql)

ADD_YB_TEST(cql-test)
YB_TEST_TARGET_INCLUDE_DIRECTORIES(cql-test PUBLIC ${CASS_DRIVER_INTERNAL_INCLUDE})
YB_TEST_TARGET_LINK_LIBRARIES(cql-test yb-cql)

set(YB_TEST_LINK_LIBS ${YB_TEST_LINK_LIBS_SAVED})

# Additional tests
if(YB_ENT_CURRENT_SOURCE_DIR)
  # Set the test source file folder.
  set(CMAKE_CURRENT_LIST_DIR ${YB_ENT_CURRENT_SOURCE_DIR})

  foreach(test ${INTEGRATION_TESTS_EXTENSIONS_TESTS})
    ADD_YB_TEST(${test})
  endforeach()
endif()
