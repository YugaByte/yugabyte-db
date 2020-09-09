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

#######################################
# yb_rocksutil
#######################################

set(ROCKSUTIL_SRCS
    rocksdb_encrypted_file_factory.cc
    yb_rocksdb.cc
    yb_rocksdb_logger.cc
    write_batch_formatter.cc
    ${ROCKSUTIL_SRCS_EXTENSIONS})

set(ROCKSUTIL_LIBS
    gflags
    glog
    gutil
    rocksdb
    yb_util)

ADD_YB_LIBRARY(yb_rocksutil
               SRCS ${ROCKSUTIL_SRCS}
               DEPS ${ROCKSUTIL_LIBS})

add_dependencies(yb_rocksutil gen_version_info)

set(YB_TEST_LINK_LIBS yb_rocksutil ${YB_TEST_LINK_LIBS_EXTENSIONS} ${YB_MIN_TEST_LIBS})

ADD_YB_TEST(yb_rocksdb_logger-test)

ADD_YB_TEST(rocksdb_encrypted_env-test)
YB_TEST_TARGET_LINK_LIBRARIES(rocksdb_encrypted_env-test encryption_test_util)
