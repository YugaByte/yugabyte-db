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

#######################################
# protoc-gen-insertions
#######################################

add_executable(protoc-gen-insertions protoc-gen-insertions.cc)
target_link_libraries(protoc-gen-insertions gutil protobuf protoc ${YB_BASE_LIBS})

#######################################
# histogram_proto
#######################################

PROTOBUF_GENERATE_CPP(
  HISTOGRAM_PROTO_SRCS HISTOGRAM_PROTO_HDRS HISTOGRAM_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES histogram.proto)
ADD_YB_LIBRARY(histogram_proto
  SRCS ${HISTOGRAM_PROTO_SRCS}
  DEPS protobuf
  NONLINK_DEPS ${HISTOGRAM_PROTO_TGTS})

#######################################
# pb_util_proto
#######################################

PROTOBUF_GENERATE_CPP(
  PB_UTIL_PROTO_SRCS PB_UTIL_PROTO_HDRS PB_UTIL_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES pb_util.proto)
ADD_YB_LIBRARY(pb_util_proto
  SRCS ${PB_UTIL_PROTO_SRCS}
  DEPS protobuf
  NONLINK_DEPS ${PB_UTIL_PROTO_TGTS})

#######################################
# version_info_proto
#######################################

PROTOBUF_GENERATE_CPP(
  VERSION_INFO_PROTO_SRCS VERSION_INFO_PROTO_HDRS VERSION_INFO_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES version_info.proto)
ADD_YB_LIBRARY(version_info_proto
  SRCS ${VERSION_INFO_PROTO_SRCS}
  DEPS protobuf
  NONLINK_DEPS ${VERSION_INFO_PROTO_TGTS})

#######################################
# op_id_proto
#######################################

PROTOBUF_GENERATE_CPP(
  OP_ID_PROTO_SRCS OP_ID_PROTO_HDRS OP_ID_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES opid.proto)
ADD_YB_LIBRARY(opid_proto
  SRCS ${OP_ID_PROTO_SRCS}
  DEPS protobuf
  NONLINK_DEPS ${OP_ID_PROTO_TGTS})

#########################################
# encryption_proto
#########################################
PROTOBUF_GENERATE_CPP(
  ENCRYPTION_PROTO_SRCS ENCRYPTION_PROTO_HDRS ENCRYPTION_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES encryption.proto)
ADD_YB_LIBRARY(encryption_proto
  SRCS ${ENCRYPTION_PROTO_SRCS}
  DEPS protobuf
  NONLINK_DEPS ${ENCRYPTION_PROTO_TGTS})

############################################################
# Version stamp
############################################################

# Unlike CMAKE_CURRENT_BINARY_DIR, CMAKE_BINARY_DIR is always the root of
# the build directory.
set(VERSION_STAMP_FILE ${CMAKE_BINARY_DIR}/version_metadata.json)

list(APPEND GEN_VERSION_INFO_COMMAND "${BUILD_SUPPORT_DIR}/gen_version_info.py")
list(APPEND GEN_VERSION_INFO_COMMAND "--build-type=${CMAKE_BUILD_TYPE}")
if(YB_GIT_HASH)
  message(STATUS "Provided git hash: ${YB_GIT_HASH}")
  list(APPEND GEN_VERSION_INFO_COMMAND "--git-hash=${YB_GIT_HASH}")
endif()
list(APPEND GEN_VERSION_INFO_COMMAND "${VERSION_STAMP_FILE}")
add_custom_target(gen_version_info
    COMMAND ${GEN_VERSION_INFO_COMMAND}
    BYPRODUCTS "${VERSION_STAMP_FILE}")

#######################################
# yb_util
#######################################

if (APPLE)
  set(SEMAPHORE_CC "semaphore_macosx.cc")
else ()
  set(SEMAPHORE_CC "semaphore.cc")
endif()

set(UTIL_SRCS
  ${SEMAPHORE_CC}
  allocation_tracker.cc
  atomic.cc
  bitmap.cc
  bitmap.cc
  bloom_filter.cc
  bytes_formatter.cc
  cache_metrics.cc
  capabilities.cc
  ctr_cipher_stream.cc
  coding.cc
  concurrent_value.cc
  condition_variable.cc
  countdown_latch.cc
  crc.cc
  cross_thread_mutex.cc
  crypt.cc
  curl_util.cc
  date_time.cc
  debug-util.cc
  debug/long_operation_tracker.cc
  debug/trace_event_impl.cc
  debug/trace_event_impl_constants.cc
  debug/trace_event_synthetic_delay.cc
  decimal.cc
  delayer.cc
  encrypted_file.cc
  encrypted_file_factory.cc
  encryption_util.cc
  env.cc
  env_posix.cc
  env_util.cc
  errno.cc
  ev_util.cc
  failure_detector.cc
  fast_varint.cc
  faststring.cc
  fault_injection.cc
  file_system.cc
  file_system_mem.cc
  file_system_posix.cc
  flag_tags.cc
  flags.cc
  hdr_histogram.cc
  header_manager_impl.cc
  hexdump.cc
  init.cc
  jsonreader.cc
  jsonwriter.cc
  locks.cc
  logging.cc
  malloc.cc
  math_util.cc
  mem_tracker.cc
  memcmpable_varint.cc
  memenv/memenv.cc
  memory/arena.cc
  memory/mc_types.cc
  memory/memory.cc
  metrics.cc
  monotime.cc
  mutex.cc
  net/dns_resolver.cc
  net/inetaddress.cc
  net/net_util.cc
  net/sockaddr.cc
  net/rate_limiter.cc
  net/socket.cc
  net/tunnel.cc
  ntp_clock.cc
  oid_generator.cc
  once.cc
  operation_counter.cc
  opid.cc
  os-util.cc
  path_util.cc
  pb_util-internal.cc
  pb_util.cc
  pg_util.cc
  physical_time.cc
  port_picker.cc
  priority_thread_pool.cc
  pstack_watcher.cc
  random_util.cc
  redis_util.cc
  ref_cnt_buffer.cc
  rolling_log.cc
  rw_mutex.cc
  rwc_lock.cc
  shared_mem.cc
  signal_util.cc
  slice.cc
  spinlock_profiling.cc
  split.cc
  stats/perf_level_imp.cc
  stats/iostats_context_imp.cc
  status.cc
  status_callback.cc
  stol_utils.cc
  string_case.cc
  string_trim.cc
  string_util.cc
  striped64.cc
  strongly_typed_uuid.cc
  subprocess.cc
  sync_point.cc
  test_graph.cc
  thread.cc
  thread_restrictions.cc
  threadlocal.cc
  threadpool.cc
  timestamp.cc
  trace.cc
  trilean.cc
  ulimit.cc
  ulimit_util.cc
  universe_key_manager.cc
  url-coding.cc
  user.cc
  uuid.cc
  varint.cc
  version_info.cc
  async_util.cc
  )

set(UTIL_LIBS
  boost_system
  boost_thread
  cds
  icui18n
  icuuc
  crcutil
  crypt_blowfish
  gflags
  glog
  gutil
  histogram_proto
  libev
  pb_util_proto
  protobuf
  version_info_proto
  encryption_proto
  zlib
  ${OPENSSL_CRYPTO_LIBRARY}
  ${OPENSSL_SSL_LIBRARY})

if (NOT APPLE AND NOT USING_LINUXBREW)
  set(UTIL_LIBS ${UTIL_LIBS} atomic)
endif()

if(NOT APPLE)
  set(UTIL_LIBS
    ${UTIL_LIBS}
    rt
    libbacktrace)
endif()

if("${YB_TCMALLOC_ENABLED}" STREQUAL "1")
  message("Adding tcmalloc as a dependency of yb_util")
  list(APPEND UTIL_LIBS tcmalloc)
else()
  message("Not adding tcmalloc as a dependency of yb_util")
endif()

# We need libdl for accessing library load addresses.
list(APPEND UTIL_LIBS curl dl)

ADD_YB_LIBRARY(yb_util
  SRCS ${UTIL_SRCS}
  DEPS ${UTIL_LIBS})

add_dependencies(yb_util gen_version_info)

#######################################
# yb_test_util
#######################################

add_library(yb_test_util
  logging_test_util.cc
  test_util.cc)
target_link_libraries(yb_test_util
  gflags
  glog
  gmock
  yb_util)

add_library(encryption_test_util encryption_test_util.cc header_manager_mock_impl.cc)
target_link_libraries(encryption_test_util yb_test_util)

#######################################
# yb_test_main
#######################################

add_library(yb_test_main
  test_main.cc)
target_link_libraries(yb_test_main
  gflags
  glog
  gmock
  yb_util
  yb_test_util)

if(NOT APPLE)
  target_link_libraries(yb_test_main
    rt)
endif()

#######################################
# Unit tests
#######################################

set(YB_TEST_LINK_LIBS yb_test_util gutil gmock ${YB_TEST_LINK_LIBS_EXTENSIONS} ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(atomic-test)
ADD_YB_TEST(bit-util-test)
ADD_YB_TEST(bitmap-test)
ADD_YB_TEST(blocking_queue-test)
ADD_YB_TEST(bloom_filter-test)
ADD_YB_TEST(byte_buffer-test)
ADD_YB_TEST(callback_bind-test)
ADD_YB_TEST(countdown_latch-test)
ADD_YB_TEST(crc-test RUN_SERIAL true) # has a benchmark
ADD_YB_TEST(crypt-test)
ADD_YB_TEST(debug-util-test)
ADD_YB_TEST(env-test LABELS no_tsan)
ADD_YB_TEST(errno-test)
ADD_YB_TEST(failure_detector-test)
ADD_YB_TEST(flag_tags-test)
ADD_YB_TEST(flags-test)
ADD_YB_TEST(format-test RUN_SERIAL true)
ADD_YB_TEST(hash_util-test)
ADD_YB_TEST(hdr_histogram-test)
ADD_YB_TEST(inline_slice-test)
ADD_YB_TEST(jsonreader-test)
ADD_YB_TEST(lockfree-test)
ADD_YB_TEST(lru_cache-test)
ADD_YB_TEST(map-util-test)
ADD_YB_TEST(memcmpable_varint-test LABELS no_tsan)
ADD_YB_TEST(memenv/memenv-test)
ADD_YB_TEST(memory/arena-test)
ADD_YB_TEST(memory/mc_types-test)
ADD_YB_TEST(memory/memory_usage-test)
ADD_YB_TEST(mem_tracker-test)
ADD_YB_TEST(metrics-test)
ADD_YB_TEST(monotime-test)
ADD_YB_TEST(mt-hdr_histogram-test RUN_SERIAL true)
ADD_YB_TEST(mt-metrics-test RUN_SERIAL true)
ADD_YB_TEST(mt-threadlocal-test RUN_SERIAL true)
ADD_YB_TEST(net/dns_resolver-test)
ADD_YB_TEST(net/net_util-test)
ADD_YB_TEST(net/rate_limiter-test)
ADD_YB_TEST(object_pool-test)
ADD_YB_TEST(once-test)
ADD_YB_TEST(os-util-test)
ADD_YB_TEST(path_util-test)
ADD_YB_TEST(priority_queue-test)
ADD_YB_TEST(priority_thread_pool-test)
ADD_YB_TEST(pstack_watcher-test)
ADD_YB_TEST(ref_cnt_buffer-test)
ADD_YB_TEST(random-test)
ADD_YB_TEST(random_util-test)
ADD_YB_TEST(result-test)
ADD_YB_TEST(rle-test)
ADD_YB_TEST(rolling_log-test)
ADD_YB_TEST(rw_mutex-test)
ADD_YB_TEST(rw_semaphore-test)
ADD_YB_TEST(rwc_lock-test)
if (NOT "${YB_BUILD_TYPE}" STREQUAL "asan")
  # We disable this test when Undefined Behavior Sanitizer turned on (which is enabled in ASAN
  # builds). This test involves some integer overflows.
  ADD_YB_TEST(safe_math-test)
endif()
ADD_YB_TEST(slice-test)
ADD_YB_TEST(spinlock_profiling-test)
ADD_YB_TEST(split-test)
ADD_YB_TEST(stats/iostats_context_test)
ADD_YB_TEST(status-test)
ADD_YB_TEST(stol_utils-test)
ADD_YB_TEST(string_case-test)
ADD_YB_TEST(striped64-test)
ADD_YB_TEST(strongly_typed_uuid-test)
ADD_YB_TEST(subprocess-test)
ADD_YB_TEST(sync_point-test)
ADD_YB_TEST(taskstream-test)
ADD_YB_TEST(thread-test)
ADD_YB_TEST(threadpool-test)
ADD_YB_TEST(tostring-test)
ADD_YB_TEST(trace-test)
ADD_YB_TEST(url-coding-test)
ADD_YB_TEST(user-test)
ADD_YB_TEST(bytes_formatter-test)
ADD_YB_TEST(string_trim-test)
ADD_YB_TEST(varint-test)
ADD_YB_TEST(decimal-test)
ADD_YB_TEST(net/inetaddress-test)
ADD_YB_TEST(uuid-test)
ADD_YB_TEST(fast_varint-test)
ADD_YB_TEST(shared_mem-test)

#######################################
# jsonwriter_test_proto
#######################################

PROTOBUF_GENERATE_CPP(
  JSONWRITER_TEST_PROTO_SRCS JSONWRITER_TEST_PROTO_HDRS JSONWRITER_TEST_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES jsonwriter_test.proto)
add_library(jsonwriter_test_proto ${JSONWRITER_TEST_PROTO_SRCS} ${JSONWRITER_TEST_PROTO_HDRS})
target_link_libraries(jsonwriter_test_proto
  protobuf)

#######################################
# jsonwriter-test
#######################################

ADD_YB_TEST(jsonwriter-test)
YB_TEST_TARGET_LINK_LIBRARIES(jsonwriter-test jsonwriter_test_proto)

#######################################
# proto_container_test_proto
#######################################

PROTOBUF_GENERATE_CPP(
  PROTO_CONTAINER_TEST_PROTO_SRCS PROTO_CONTAINER_TEST_PROTO_HDRS PROTO_CONTAINER_TEST_PROTO_TGTS
  SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..
  BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/../..
  PROTO_FILES
    proto_container_test.proto
    proto_container_test2.proto
    proto_container_test3.proto)
add_library(proto_container_test_proto
  ${PROTO_CONTAINER_TEST_PROTO_SRCS}
  ${PROTO_CONTAINER_TEST_PROTO_HDRS})
add_dependencies(proto_container_test_proto
  gen_src_yb_util_proto_container_test_proto
  gen_src_yb_util_proto_container_test2_proto
  gen_src_yb_util_proto_container_test3_proto
  )
target_link_libraries(proto_container_test_proto
  protobuf)

#######################################
# pb_util-test
#######################################

ADD_YB_TEST(pb_util-test)
YB_TEST_TARGET_LINK_LIBRARIES(pb_util-test proto_container_test_proto)

ADD_YB_TEST(encrypted_env-test)
YB_TEST_TARGET_LINK_LIBRARIES(encrypted_env-test encryption_test_util)

ADD_YB_TEST(ctr_cipher_stream-test)
YB_TEST_TARGET_LINK_LIBRARIES(ctr_cipher_stream-test encryption_test_util)
