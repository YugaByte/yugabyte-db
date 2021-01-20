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

set(TABLET_SRCS
  abstract_tablet.cc
  apply_intents_task.cc
  cleanup_aborts_task.cc
  cleanup_intents_task.cc
  remove_intents_task.cc
  running_transaction.cc
  tablet_snapshots.cc
  tablet.cc
  tablet_bootstrap.cc
  tablet_bootstrap_if.cc
  tablet_component.cc
  tablet_metrics.cc
  tablet_peer_mm_ops.cc
  tablet_peer.cc
  transaction_coordinator.cc
  transaction_loader.cc
  transaction_participant.cc
  transaction_status_resolver.cc
  operation_order_verifier.cc
  operations/operation.cc
  operations/change_metadata_operation.cc
  operations/history_cutoff_operation.cc
  operations/operation_driver.cc
  operations/operation_tracker.cc
  operations/snapshot_operation.cc
  operations/split_operation.cc
  operations/truncate_operation.cc
  operations/update_txn_operation.cc
  operations/write_operation.cc
  maintenance_manager.cc
  mvcc.cc
  tablet_metadata.cc
  tablet_retention_policy.cc
  preparer.cc
  ${TABLET_SRCS_EXTENSIONS})

set(TABLET_DEPS pq_utils)

PROTOBUF_GENERATE_CPP(
  TABLET_PROTO_SRCS TABLET_PROTO_HDRS TABLET_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES
    tablet.proto
    metadata.proto)
set(TABLET_PROTO_LIBS
  protobuf
  fs_proto
  consensus_metadata_proto
  yb_common)
ADD_YB_LIBRARY(tablet_proto
  SRCS ${TABLET_PROTO_SRCS}
  DEPS ${TABLET_PROTO_LIBS} opid_proto
  NONLINK_DEPS ${TABLET_PROTO_TGTS})

ADD_YB_LIBRARY(
  tablet
  SRCS ${TABLET_SRCS}
  DEPS ${TABLET_DEPS})

ADD_YB_LIBRARY(
  tablet_error
  SRCS tablet_error.cc
  DEPS tablet_proto yb_util)

target_link_libraries(tablet
  tablet_proto
  yb_common
  gutil
  server_common
  yb_fs
  yb_rocksutil
  yb_util
  consensus
  rocksdb
  yb_docdb
  yb_client
  yb_pggate_util
  tablet_error)

add_library(tablet_test_util
            local_tablet_writer.cc
            tablet-test-util.cc)
target_link_libraries(tablet_test_util tablet yb_common yb_test_util yb_util)

set(YB_TEST_LINK_LIBS tablet tablet_test_util ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(tablet-test)
ADD_YB_TEST(tablet-split-test)
ADD_YB_TEST(tablet-metadata-test)
ADD_YB_TEST(verifyrows-tablet-test)
ADD_YB_TEST(tablet-pushdown-test)
ADD_YB_TEST(tablet-schema-test)
ADD_YB_TEST(tablet_bootstrap-test)
ADD_YB_TEST(maintenance_manager-test)
ADD_YB_TEST(mvcc-test)
ADD_YB_TEST(composite-pushdown-test)
ADD_YB_TEST(tablet_peer-test)
ADD_YB_TEST(tablet_random_access-test)
