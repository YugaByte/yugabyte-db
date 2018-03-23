#!/usr/bin/env bash

#
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
# Script which wraps running a test and redirects its output to a
# test log directory.
#
# Path to the test executable or script to be run.
# May be relative or absolute.

# Portions Copyright (c) YugaByte, Inc.

set -euo pipefail

cleanup() {
  local exit_code=$?
  kill_stuck_processes
  if [[ $exit_code -eq 0 ]] && "$killed_stuck_processes"; then
    exit_code=1
  fi
  exit "$exit_code"
}

if [[ ${YB_DEBUG_RUN_TEST:-} == "1" ]]; then
  log "Running ${0##*/} with 'set -x' for debugging (perhaps it previously failed with no output)."
  set -x
fi

. "${BASH_SOURCE%/*}/common-build-env.sh"
. "${BASH_SOURCE%/*}/common-test-env.sh"

if [[ -n ${YB_LIST_CTEST_TESTS_ONLY:-} ]]; then
  # This has to match CTEST_TEST_PROGRAM_RE in run_tests_on_spark.py.
  echo "ctest test: \"$1\""
  exit 0
fi

# Create group-writable files by default. Useful in an NFS environment.
umask 0002

# This will be part of the command line of all mini-cluster daemons, so we can later kill any
# of them that remain.
timestamp=$( get_timestamp_for_filenames )
export YB_TEST_INVOCATION_ID=${timestamp}_${RANDOM}_${RANDOM}_$$
trap cleanup EXIT

set_common_test_paths

if [[ $# -eq 2 && -d $YB_SRC_ROOT/java/$1 ]]; then
  # This is a Java test.
  # Arguments: <maven_module_name> <package_and_class>
  # Example: yb-client org.yb.client.TestYBClient

  run_java_test "$@"
  # See the cleanup() function above for how we kill stuck processes based on the
  # $YB_TEST_INVOCATION_ID pattern.
  exit
fi

TEST_PATH=${1:-}
if [[ -z $TEST_PATH ]]; then
  fatal "Test path must be specified as the first argument"
fi
shift

if [[ ! -f $TEST_PATH ]]; then
  fatal "Test binary '$TEST_PATH' does not exist"
fi

if [[ -n ${YB_CHECK_TEST_EXISTENCE_ONLY:-} ]]; then
  exit 0
fi

# Used for invoking a specific test within a test program, e.g. as part of a Spark-based test run.
exact_test=""
if [[ $# -gt 0 ]]; then
  exact_test=$1
fi

if [[ ! -x $TEST_PATH ]]; then
  fatal "Test binary '$TEST_PATH' is not executable"
fi

if [[ ! -d $PWD ]]; then
  log "Current directory $PWD does not exist, using /tmp as working directory"
  cd /tmp
fi

if [[ -z ${BUILD_ROOT:-} ]]; then
  # Absolute path to the root build directory. The test path is expected to be in its subdirectory.
  BUILD_ROOT=$(cd "$(dirname "$TEST_PATH")"/.. && pwd)
fi

set_common_test_paths

TEST_DIR=$(cd "$(dirname "$TEST_PATH")" && pwd)

if [ ! -d "$TEST_DIR" ]; then
  echo "Test directory '$TEST_DIR' does not exist"
  exit 1
fi

TEST_NAME_WITH_EXT=$(basename "$TEST_PATH")
abs_test_binary_path=$TEST_DIR/$TEST_NAME_WITH_EXT

# Remove path and extension, if any.
TEST_NAME=${TEST_NAME_WITH_EXT%%.*}

TEST_DIR_BASENAME="$( basename "$TEST_DIR" )"
LOG_PATH_BASENAME_PREFIX=$TEST_NAME

set_sanitizer_runtime_options

tests=()
rel_test_binary="$TEST_DIR_BASENAME/$TEST_NAME"
total_num_tests=0
num_tests=0
if [[ -z $exact_test ]]; then
  collect_gtest_tests

  if [[ ${#tests[@]} -eq 0 ]]; then
    fatal "No tests found in $rel_test_binary."
  fi
else
  # We're assuming that the test binary is always two levels below the build root, e.g.
  # tests-tablet/tablet-test.
  tests=( "$TEST_DIR_BASENAME/$TEST_NAME$TEST_DESCRIPTOR_SEPARATOR$exact_test" )
fi

set_test_log_url_prefix

global_exit_code=0

# We have a mode in which we run multiple "attempts" of the same test. It can be triggered in one
# of two ways:
# - Specifying YB_NUM_TEST_ATTEMPTS greater than 1. In that case we'll run multiple attempts of the
#   same test sequentially.
# - Setting YB_TEST_ATTEMPT_INDEX. This is what happens in distributed test runs on Spark. In this
#   case we run one test, but attach the given "attempt index" to it so that all log and output
#   files are suffixed with it.

if [[ -n ${YB_TEST_ATTEMPT_INDEX:-} ]]; then
  # This is used when running tests multiple times on Spark. We just specify an attempt index
  # externally as an environment variable, as multiple attempts for the same test could run
  # concurrently.
  if [[ ! $YB_TEST_ATTEMPT_INDEX =~ ^[0-9]+$ ]]; then
    fatal "YB_TEST_ATTEMPT_INDEX is not set to a valid integer: '${YB_TEST_ATTEMPT_INDEX}'"
  fi
  declare -i -r min_test_attempt_index=$YB_TEST_ATTEMPT_INDEX
  declare -i -r max_test_attempt_index=$YB_TEST_ATTEMPT_INDEX
else
  if [[ -n ${YB_NUM_TEST_ATTEMPTS:-} ]]; then
    if [[ ! $YB_NUM_TEST_ATTEMPTS =~ ^[0-9]+$ ]]; then
      fatal "YB_NUM_TEST_ATTEMPTS is not set to a valid integer: '${YB_NUM_TEST_ATTEMPTS}'"
    fi
    declare -i -r num_test_attempts=$YB_NUM_TEST_ATTEMPTS
    if [[ $num_test_attempts -lt 1 ]]; then
      fatal "YB_NUM_TEST_ATTEMPTS cannot be lower than 1"
    fi
  else
    declare -i -r num_test_attempts=1
  fi
  declare -i -r min_test_attempt_index=1
  declare -i -r max_test_attempt_index=$num_test_attempts
fi

if [[ -n ${YB_LIST_TESTS_ONLY:-} ]]; then
  for test_descriptor in "${tests[@]}"; do
    echo "test descriptor: $test_descriptor"
  done
  exit 0
fi

# Loop over all tests in a gtest binary, or just one element (the whole test binary) for tests that
# we have to run in one shot.
for test_descriptor in "${tests[@]}"; do
  for (( test_attempt=$min_test_attempt_index;
         test_attempt <= $max_test_attempt_index;
         test_attempt++ )); do
    if [[ $max_test_attempt_index -gt 1 ]]; then
      log "Starting test attempt $test_attempt ($test_descriptor)"
      test_attempt_index=$test_attempt
    else
      test_attempt_index=""
    fi
    prepare_for_running_test
    run_test_and_process_results
  done
done

# This was missing for quite some time prior to early Dec 2016, resulting in "$global_exit_code"
# being carefully prepared but then ignored, and people observing discrepancies between test
# failures reported in the Detective dashboard (which is mainly based on JUnit-compatible XML files
# generated by GTest tests), and "test passed" messages coming out of ctest in the Jenkins log.
# Such discrepancies might still be possible, but we will eliminate them eventually.
exit "$global_exit_code"
