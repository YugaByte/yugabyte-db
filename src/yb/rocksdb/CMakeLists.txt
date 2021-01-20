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

# RocksDB build.

PROTOBUF_GENERATE_CPP(
  ROCKSDB_PROTO_SRCS ROCKSDB_PROTO_HDRS ROCKSDB_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES db/version_edit.proto)

set_source_files_properties(${ROCKSDB_PROTO_SRCS} PROPERTIES
                            COMPILE_FLAGS "-Wno-shorten-64-to-32")

### RocksDB sources
set(ROCKSDB_SRCS
    db/auto_roll_logger.cc
    db/builder.cc
    db/column_family.cc
    db/compacted_db_impl.cc
    db/compaction.cc
    db/compaction_iterator.cc
    db/compaction_job.cc
    db/compaction_picker.cc
    db/convenience.cc
    db/db_filesnapshot.cc
    db/dbformat.cc
    db/db_impl.cc
    db/db_impl_debug.cc
    db/db_impl_readonly.cc
    db/db_impl_experimental.cc
    db/db_info_dumper.cc
    db/db_iter.cc
    db/experimental.cc
    db/event_helpers.cc
    db/file_indexer.cc
    db/file_numbers.cc
    db/filename.cc
    db/flush_job.cc
    db/flush_scheduler.cc
    db/forward_iterator.cc
    db/internal_stats.cc
    db/log_reader.cc
    db/log_writer.cc
    db/managed_iterator.cc
    db/memtable_allocator.cc
    db/memtable.cc
    db/memtable_list.cc
    db/merge_helper.cc
    db/merge_operator.cc
    db/metadata.cc
    db/repair.cc
    db/snapshot_impl.cc
    db/table_cache.cc
    db/table_properties_collector.cc
    db/transaction_log_impl.cc
    db/version_builder.cc
    db/version_edit.cc
    db/version_set.cc
    db/wal_manager.cc
    db/write_batch.cc
    db/write_batch_base.cc
    db/write_controller.cc
    db/write_thread.cc
    db/xfunc_test_points.cc
    db/db_iterator_wrapper.cc
    memtable/hash_linklist_rep.cc
    memtable/hash_skiplist_rep.cc
    memtable/skiplistrep.cc
    memtable/vectorrep.cc
    port/stack_trace.cc
    port/port_posix.cc
    table/adaptive_table_factory.cc
    table/block_based_filter_block.cc
    table/block_based_table_builder.cc
    table/block_based_table_factory.cc
    table/block_based_table_reader.cc
    table/block_builder.cc
    table/block.cc
    table/block_hash_index.cc
    table/block_prefix_index.cc
    table/bloom_block.cc
    table/flush_block_policy.cc
    table/format.cc
    table/fixed_size_filter_block.cc
    table/full_filter_block.cc
    table/get_context.cc
    table/index_builder.cc
    table/index_reader.cc
    table/iterator.cc
    table/merger.cc
    table/meta_blocks.cc
    table/sst_file_writer.cc
    table/plain_table_builder.cc
    table/plain_table_factory.cc
    table/plain_table_index.cc
    table/plain_table_key_coding.cc
    table/plain_table_reader.cc
    table/table_properties.cc
    table/two_level_iterator.cc
    tools/dump/db_dump_tool.cc
    util/arena.cc
    util/bloom.cc
    util/cache.cc
    util/coding.cc
    util/comparator.cc
    util/compaction_job_stats_impl.cc
    util/concurrent_arena.cc
    util/crc32c.cc
    util/delete_scheduler.cc
    util/dynamic_bloom.cc
    util/env.cc
    util/env_hdfs.cc
    util/env_posix.cc
    util/io_posix.cc
    util/thread_posix.cc
    util/sst_file_manager_impl.cc
    util/file_util.cc
    util/file_reader_writer.cc
    util/filter_policy.cc
    util/hash.cc
    util/histogram.cc
    util/instrumented_mutex.cc
    util/timeout_error.cc
    utilities/convenience/info_log_finder.cc
    utilities/checkpoint/checkpoint.cc
    utilities/compaction_filters/remove_emptyvalue_compactionfilter.cc
    utilities/document/document_db.cc
    utilities/document/json_document_builder.cc
    utilities/document/json_document.cc
    utilities/env_mirror.cc
    utilities/flashcache/flashcache.cc
    utilities/geodb/geodb_impl.cc
    utilities/leveldb_options/leveldb_options.cc
    utilities/memory/memory_util.cc
    utilities/merge_operators/put.cc
    utilities/merge_operators/string_append/stringappend2.cc
    utilities/merge_operators/string_append/stringappend.cc
    utilities/merge_operators/uint64add.cc
    utilities/options/options_util.cc
    utilities/redis/redis_lists.cc
    utilities/spatialdb/spatial_db.cc
    utilities/table_properties_collectors/compact_on_deletion_collector.cc
    utilities/transactions/optimistic_transaction_impl.cc
    utilities/transactions/optimistic_transaction_db_impl.cc
    utilities/transactions/transaction_base.cc
    utilities/transactions/transaction_db_impl.cc
    utilities/transactions/transaction_db_mutex_impl.cc
    utilities/transactions/transaction_lock_mgr.cc
    utilities/transactions/transaction_impl.cc
    utilities/transactions/transaction_util.cc
    utilities/ttl/db_ttl_impl.cc
    utilities/write_batch_with_index/write_batch_with_index.cc
    utilities/write_batch_with_index/write_batch_with_index_internal.cc
    util/event_logger.cc
    util/log_buffer.cc
    util/logging.cc
    util/memenv.cc
    util/murmurhash.cc
    util/mutable_cf_options.cc
    util/options.cc
    util/options_builder.cc
    util/options_helper.cc
    util/options_parser.cc
    util/options_sanity_check.cc
    util/perf_context.cc
    util/random.cc
    util/rate_limiter.cc
    util/slice_transform.cc
    util/statistics.cc
    util/sync_point.cc
    util/thread_local.cc
    util/thread_status_impl.cc
    util/thread_status_updater.cc
    util/thread_status_updater_debug.cc
    util/thread_status_util.cc
    util/thread_status_util_debug.cc
    util/xfunc.cc
    util/xxhash.cc
    ${ROCKSDB_PROTO_SRCS}
)

set(CMAKE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} \
   -Wextra -Wsign-compare -Wshadow -Woverloaded-virtual \
   -Wno-missing-field-initializers -Wno-unused-parameter -Wno-unused-variable")

if (IS_CLANG)
  set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -Wshorten-64-to-32 -Wno-unevaluated-expression -Wno-parentheses-equality")
endif()

if (NOT APPLE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DROCKSDB_FALLOCATE_PRESENT")
endif()

if ("${YB_BUILD_TYPE}" STREQUAL "asan")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DROCKSDB_MALLOC_USABLE_SIZE")
endif()

add_library(rocksdb ${ROCKSDB_SRCS})
target_link_libraries(rocksdb gflags gutil snappy z yb_common yb_util opid_proto)

add_library(rocksdb_tools
  tools/ldb_cmd.cc
  tools/ldb_tool.cc
  tools/sst_dump_tool.cc)
target_link_libraries(rocksdb_tools rocksdb yb_rocksutil)

macro(ADD_YB_ROCKSDB_TOOL tool)
  add_executable(${tool} tools/${tool}.cc)
  target_link_libraries(${tool} rocksdb_tools)
  set_target_properties(${tool} PROPERTIES COMPILE_FLAGS "-DDISABLE_STATUS_NODISCARD")
endmacro()

set(ROCKSDB_TEST_UTIL_SRCS
  table/mock_table.cc
  util/mock_env.cc
  util/testharness.cc
  util/testutil.cc
)

if (NOT "${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  set(ROCKSDB_TEST_UTIL_SRCS ${ROCKSDB_TEST_UTIL_SRCS} db/db_test_util.cc)
endif()

add_library(rocksdb_test_util ${ROCKSDB_TEST_UTIL_SRCS})
target_link_libraries(rocksdb_test_util gmock rocksdb yb_rocksutil)

ADD_YB_ROCKSDB_TOOL(sst_dump)
add_executable(db_bench tools/db_bench.cc tools/db_bench_tool.cc)
target_link_libraries(db_bench rocksdb)
ADD_YB_ROCKSDB_TOOL(db_sanity_test)
ADD_YB_ROCKSDB_TOOL(db_stress)
ADD_YB_ROCKSDB_TOOL(write_stress)
ADD_YB_ROCKSDB_TOOL(db_repl_stress)
target_link_libraries(db_repl_stress rocksdb_test_util)
add_executable(rocksdb_undump tools/dump/rocksdb_undump.cc)
target_link_libraries(rocksdb_undump rocksdb_tools)

set(YB_TEST_LINK_LIBS rocksdb_test_util rocksdb ${YB_MIN_TEST_LIBS} rocksdb_tools)

ADD_YB_TEST(db/compaction_job_test)
ADD_YB_TEST(db/compaction_picker_test)
ADD_YB_TEST(db/comparator_db_test)
ADD_YB_TEST(db/dbformat_test)
ADD_YB_TEST(db/file_indexer_test)
ADD_YB_TEST(db/filename_test)
ADD_YB_TEST(db/flush_job_test)
ADD_YB_TEST(db/inlineskiplist_test)
ADD_YB_TEST(db/log_test)
ADD_YB_TEST(db/manual_compaction_test)
ADD_YB_TEST(db/merge_test)
ADD_YB_TEST(db/options_file_test)
ADD_YB_TEST(db/perf_context_test)
ADD_YB_TEST(db/prefix_test)
ADD_YB_TEST(db/skiplist_test)
ADD_YB_TEST(db/table_properties_collector_test)
ADD_YB_TEST(db/user_op_id_test)
ADD_YB_TEST(db/version_builder_test)
ADD_YB_TEST(db/version_set_test)
ADD_YB_TEST(db/wal_manager_test)
ADD_YB_TEST(db/write_batch_test)
ADD_YB_TEST(db/write_controller_test)
ADD_YB_TEST(table/block_based_filter_block_test)
ADD_YB_TEST(table/block_hash_index_test)
ADD_YB_TEST(table/block_test)
ADD_YB_TEST(table/full_filter_block_test)
ADD_YB_TEST(table/fixed_size_filter_block_test)
ADD_YB_TEST(table/merger_test)
ADD_YB_TEST(table/table_test)
ADD_YB_TEST(tools/sst_dump_test)
YB_TEST_TARGET_LINK_LIBRARIES(sst_dump_test rocksdb_tools)
ADD_YB_TEST(util/arena_test)
ADD_YB_TEST(util/autovector_test)
ADD_YB_TEST(util/bloom_test)
ADD_YB_TEST(util/cache_test)
ADD_YB_TEST(util/coding_test)
ADD_YB_TEST(util/crc32c_test)
ADD_YB_TEST(util/dynamic_bloom_test)
ADD_YB_TEST(util/env_test)
ADD_YB_TEST(util/event_logger_test)
ADD_YB_TEST(util/filelock_test)
ADD_YB_TEST(util/histogram_test)
ADD_YB_TEST(util/memenv_test)
ADD_YB_TEST(util/mock_env_test)
ADD_YB_TEST(util/options_test)
ADD_YB_TEST(util/rate_limiter_test)
ADD_YB_TEST(util/slice_transform_test)
ADD_YB_TEST(util/thread_list_test)
ADD_YB_TEST(utilities/document/document_db_test)
ADD_YB_TEST(utilities/document/json_document_test)
ADD_YB_TEST(utilities/geodb/geodb_test)
ADD_YB_TEST(utilities/merge_operators/string_append/stringappend_test)
ADD_YB_TEST(utilities/options/options_util_test)
ADD_YB_TEST(utilities/redis/redis_lists_test)
ADD_YB_TEST(utilities/spatialdb/spatial_db_test)
ADD_YB_TEST(utilities/table_properties_collectors/compact_on_deletion_collector_test)
ADD_YB_TEST(utilities/transactions/optimistic_transaction_test)
ADD_YB_TEST(utilities/ttl/ttl_test)
ADD_YB_TEST(utilities/write_batch_with_index/write_batch_with_index_test)

if (NOT "${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
ADD_YB_TEST(db/auto_roll_logger_test)
ADD_YB_TEST(db/column_family_test)
ADD_YB_TEST(db/compact_files_test)
ADD_YB_TEST(db/compaction_job_stats_test)
ADD_YB_TEST(db/corruption_test)
ADD_YB_TEST(db/db_block_cache_test)
ADD_YB_TEST(db/db_bloom_filter_test)
ADD_YB_TEST(db/db_compaction_filter_test)
ADD_YB_TEST(db/db_compaction_test)
ADD_YB_TEST(db/db_dynamic_level_test)
ADD_YB_TEST(db/db_inplace_update_test)
ADD_YB_TEST(db/db_iter_test)
ADD_YB_TEST(db/db_log_iter_test)
ADD_YB_TEST(db/db_properties_test)
ADD_YB_TEST(db/db_sst_test)
ADD_YB_TEST(db/db_table_properties_test)
ADD_YB_TEST(db/db_tailing_iter_test)
ADD_YB_TEST(db/db_test)
ADD_YB_TEST(db/db_test2)
ADD_YB_TEST(db/db_universal_compaction_test)
ADD_YB_TEST(db/db_universal_compaction_deletion_test)
ADD_YB_TEST(db/db_wal_test)
ADD_YB_TEST(db/deletefile_test)
ADD_YB_TEST(db/fault_injection_test)
ADD_YB_TEST(db/listener_test)
ADD_YB_TEST(db/plain_table_db_test)
ADD_YB_TEST(db/version_edit_test)
ADD_YB_TEST(db/write_callback_test)
ADD_YB_TEST(tools/ldb_cmd_test)
ADD_YB_TEST(tools/reduce_levels_test)
YB_TEST_TARGET_LINK_LIBRARIES(reduce_levels_test rocksdb_tools)
ADD_YB_TEST(util/delete_scheduler_test)
ADD_YB_TEST(util/thread_local_test)
ADD_YB_TEST(utilities/checkpoint/checkpoint_test)
ADD_YB_TEST(utilities/memory/memory_test)
ADD_YB_TEST(utilities/transactions/transaction_test)

endif()
