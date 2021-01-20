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

# docdb_proto

PROTOBUF_GENERATE_CPP(
        DOCDB_PROTO_SRCS DOCDB_PROTO_HDRS DOCDB_PROTO_TGTS
        SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
        BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
        PROTO_FILES docdb.proto)

ADD_YB_LIBRARY(docdb_proto
        SRCS ${DOCDB_PROTO_SRCS}
        DEPS protobuf yb_common_proto opid_proto
        NONLINK_DEPS ${DOCDB_PROTO_TGTS})

set(DOCDB_ENCODING_SRCS
        doc_key.cc
        doc_kv_util.cc
        key_bytes.cc
        primitive_value.cc
        primitive_value_util.cc
        intent.cc
        doc_scanspec_util.cc
        )

set(DOCDB_ENCODING_DEPS
        docdb_proto
        server_common
        yb_common
        yb_rocksutil
        yb_util
        )

ADD_YB_LIBRARY(yb_docdb_encoding
        SRCS ${DOCDB_ENCODING_SRCS}
        DEPS ${DOCDB_ENCODING_DEPS}
        )

include_directories(${YB_BUILD_ROOT}/postgres/include/server)

set(DOCDB_SRCS
        bounded_rocksdb_iterator.cc
        conflict_resolution.cc
        consensus_frontier.cc
        cql_operation.cc
        deadline_info.cc
        doc_boundary_values_extractor.cc
        docdb.cc
        docdb_debug.cc
        docdb_pgapi.cc
        docdb_util.cc
        doc_ttl_util.cc
        docdb_compaction_filter.cc
        docdb_compaction_filter_intents.cc
        docdb-internal.cc
        doc_reader.cc
        doc_reader_redis.cc
        docdb_rocksdb_util.cc
        doc_expr.cc
        doc_pgsql_scanspec.cc
        doc_ql_scanspec.cc
        doc_rowwise_iterator.cc
        doc_write_batch_cache.cc
        doc_write_batch.cc
        intent_aware_iterator.cc
        lock_batch.cc
        pgsql_operation.cc
        ql_rocksdb_storage.cc
        redis_operation.cc
        shared_lock_manager.cc
        subdocument.cc
        value.cc
        kv_debug.cc
        )

set(DOCDB_DEPS
        yb_docdb_encoding
        rocksdb
        yb_util
        yb_rocksutil
        yb_common
        docdb_proto
        server_common
        rpc_introspection_proto
        yb_pggate_util
        yb_pggate
        ql_util
        yb_pgbackend
        )

ADD_YB_LIBRARY(yb_docdb
        SRCS ${DOCDB_SRCS}
        DEPS ${DOCDB_DEPS}
        NONLINK_DEPS postgres)

# Ensure protobuf headers we depend on are generated prior to building DocDB.
add_dependencies(
        yb_docdb_encoding
        gen_src_yb_master_master_proto
        gen_src_yb_rpc_rpc_header_proto
        gen_src_yb_tablet_metadata_proto
        gen_src_yb_tablet_tablet_proto
        gen_src_yb_tserver_tserver_proto
)

# A library with common code shared between DocDB tests.
set(DOCDB_TEST_COMMON_SRCS
        docdb_test_util.cc
        docdb_test_base.cc
        in_mem_docdb.cc)

add_library(yb_docdb_test_common ${DOCDB_TEST_COMMON_SRCS})
target_link_libraries(yb_docdb_test_common
        yb_docdb
        yb_test_util)

set(YB_TEST_LINK_LIBS yb_common_test_util yb_docdb_test_common ${YB_MIN_TEST_LIBS})

ADD_YB_TEST(doc_key-test)
ADD_YB_TEST(doc_kv_util-test)
ADD_YB_TEST(doc_operation-test)
ADD_YB_TEST(docdb-test)
ADD_YB_TEST(docrowwiseiterator-test)
ADD_YB_TEST(primitive_value-test)
ADD_YB_TEST(randomized_docdb-test)
ADD_YB_TEST(shared_lock_manager-test)
ADD_YB_TEST(subdocument-test)
ADD_YB_TEST(value-test)
ADD_YB_TEST(consensus_frontier-test)
