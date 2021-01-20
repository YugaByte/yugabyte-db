#!/usr/bin/env bash

# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations under
# the License.
#

# Common bash code for test scripts.

declare -i global_exit_code=0

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  echo "${BASH_SOURCE[0]} must be sourced, not executed" >&2
  exit 1
fi

# shellcheck source=build-support/common-build-env.sh
. "${BASH_SOURCE[0]%/*}/common-build-env.sh"

NON_GTEST_TESTS=(
  merge-test
  non_gtest_failures-test
  c_test
  compact_on_deletion_collector_test
  db_sanity_test
  merge_test
)
make_regex_from_list NON_GTEST_TESTS "${NON_GTEST_TESTS[@]}"

# There gtest suites have internal dependencies between tests, so those tests can't be run
# separately.
TEST_BINARIES_TO_RUN_AT_ONCE=(
  tests-rocksdb/thread_local_test
)
make_regex_from_list TEST_BINARIES_TO_RUN_AT_ONCE "${TEST_BINARIES_TO_RUN_AT_ONCE[@]}"

VALID_TEST_BINARY_DIRS_PREFIX="tests"
VALID_TEST_BINARY_DIRS_RE="^${VALID_TEST_BINARY_DIRS_PREFIX}-[0-9a-zA-Z\-]+"

# gdb command to print a backtrace from a core dump. Taken from:
# http://www.commandlinefu.com/commands/view/6004/print-stack-trace-of-a-core-file-without-needing-to-enter-gdb-interactively

DEFAULT_TEST_TIMEOUT_SEC=${DEFAULT_TEST_TIMEOUT_SEC:-600}
INCREASED_TEST_TIMEOUT_SEC=$(( DEFAULT_TEST_TIMEOUT_SEC * 2 ))

# This timeout will be applied by the process_tree_supervisor.py script.
# shellcheck disable=SC2034
PROCESS_TREE_SUPERVISOR_TEST_TIMEOUT_SEC=$(( 30 * 60 ))

# We grep for these log lines and show them in the main log on test failure. This regular expression
# is used with "grep -E".
RELEVANT_LOG_LINES_RE="^[[:space:]]*(Value of|Actual|Expected):|^Expected|^Failed|^Which is:"
RELEVANT_LOG_LINES_RE+="|: Failure$|ThreadSanitizer: data race|Check failure"
readonly RELEVANT_LOG_LINES_RE

# Some functions use this to output to stdout/stderr along with a file.
append_output_to=/dev/null

readonly TEST_RESTART_PATTERN="Address already in use|\
pthread .*: Device or resource busy|\
FATAL: could not create shared memory segment: No space left on device"

# We use this to submit test jobs for execution on Spark.
readonly INITIAL_SPARK_DRIVER_CORES=8

# This is used to separate relative binary path from gtest_filter for C++ tests in what we call
# a "test descriptor" (a string that identifies a particular test).
#
# This must match the constant with the same name in run_tests_on_spark.py.
readonly TEST_DESCRIPTOR_SEPARATOR=":::"

readonly JENKINS_NFS_BUILD_REPORT_BASE_DIR="/n/jenkins/build_stats"

# https://github.com/google/sanitizers/wiki/SanitizerCommonFlags
readonly SANITIZER_COMMON_OPTIONS=""

# shellcheck disable=SC2034
declare -i -r MIN_REPEATED_TEST_PARALLELISM=1

# shellcheck disable=SC2034
declare -i -r MAX_REPEATED_TEST_PARALLELISM=100

# shellcheck disable=SC2034
declare -i -r DEFAULT_REPEATED_TEST_PARALLELISM=4

# shellcheck disable=SC2034
declare -i -r DEFAULT_REPEATED_TEST_PARALLELISM_TSAN=1

readonly MVN_COMMON_SKIPPED_OPTIONS_IN_TEST=(
  -Dmaven.javadoc.skip
)

# -------------------------------------------------------------------------------------------------
# Functions
# -------------------------------------------------------------------------------------------------

# Sanitize the given string so we can make it a path component.
sanitize_for_path() {
  echo "$1" | sed 's/\//__/g; s/[:.]/_/g;'
}

# shellcheck disable=SC2120
set_common_test_paths() {
  expect_num_args 0 "$@"

  if [[ -z ${YB_TEST_LOG_ROOT_DIR:-} ]]; then
    # Allow putting all the test logs into a separate directory by setting YB_TEST_LOG_ROOT_SUFFIX.
    YB_TEST_LOG_ROOT_DIR="$BUILD_ROOT/yb-test-logs${YB_TEST_LOG_ROOT_SUFFIX:-}"
  fi

  mkdir_safe "$YB_TEST_LOG_ROOT_DIR"

  # This is needed for tests to find the lsof program.
  add_path_entry /usr/sbin
}

validate_relative_test_binary_path() {
  expect_num_args 1 "$@"
  local rel_test_binary=$1
  if [[ ! $rel_test_binary =~ ^[a-zA-Z0-9_-]+/[a-zA-Z0-9_.-]+$ ]]; then
    fatal "Expected a relative test binary path to consist of two components separated by '/'." \
          "Got: '$rel_test_binary'."
  fi
  local rel_test_binary_dirname=${rel_test_binary%/*}
  if [[ ! $rel_test_binary_dirname =~ $VALID_TEST_BINARY_DIRS_RE ]]; then
    fatal "Expected the directory name a test binary is contained in to match regexp: " \
          "$VALID_TEST_BINARY_DIRS_RE. Relative test binary path: $rel_test_binary" >&2
  fi
}

create_abs_test_binary_path() {
  expect_num_args 1 "$@"
  local rel_test_binary_path=$1
  local abs_test_binary_path=$BUILD_ROOT/$rel_test_binary_path
  if [[ ! -f $abs_test_binary_path && -f "$abs_test_binary_path.sh" ]]; then
    abs_test_binary_path+=".sh"
  fi
  echo "$abs_test_binary_path"
}

validate_abs_test_binary_path() {
  if [[ ! -f $abs_test_binary_path ]]; then
    fatal "Test binary '$abs_test_binary_path' does not exist"
  fi
  if [[ ! -x $abs_test_binary_path ]]; then
    fatal "Test binary '$abs_test_binary_path' is not executable"
  fi
}

# Validates a "test descriptor" string. This is our own concept to identify tests within our test
# suite. A test descriptor has one of the two forms:
#   - <relative_test_binary>$TEST_DESCRIPTOR_SEPARATOR<test_name_within_binary>
#   - <relative_test_binary>
# We've chosen triple colon as a separator that would rarely occur otherwise.
# Examples:
#   - bin/tablet-test$TEST_DESCRIPTOR_SEPARATORTestTablet/5.TestDeleteWithFlushAndCompact
# The test name within the binary is what can be passed to Google Test as the --gtest_filter=...
# argument. Some test binaries have to be run at once, e.g. non-gtest test binary or
# test binaries with internal dependencies between tests. For those there is on
# "$TEST_DESCRIPTOR_SEPARATOR" separator or the <test_name_within_binary> part.
validate_test_descriptor() {
  expect_num_args 1 "$@"
  local test_descriptor=$1
  if [[ $test_descriptor =~ $TEST_DESCRIPTOR_SEPARATOR ]]; then
    if [[ $test_descriptor =~ $TEST_DESCRIPTOR_SEPARATOR.*$TEST_DESCRIPTOR_SEPARATOR ]]; then
      fatal "The '$TEST_DESCRIPTOR_SEPARATOR' separator occurs twice within the test descriptor:" \
            "$test_descriptor"
    fi
    # Remove $TEST_DESCRIPTOR_SEPARATOR and all that follows and get a relative binary path.
    validate_relative_test_binary_path "${test_descriptor%$TEST_DESCRIPTOR_SEPARATOR*}"
  else
    validate_relative_test_binary_path "$test_descriptor"
  fi
}

# Some tests are not based on the gtest framework and don't generate an XML output file.
# Also, RocksDB's thread_list_test is like that on Mac OS X.
is_known_non_gtest_test() {
  local test_name=$1

  if [[ "$OSTYPE" =~ ^darwin && "$test_name" == thread_list_test || \
        "$test_name" =~ $NON_GTEST_TESTS_RE ]]; then
    return 0  # "true return value" in bash
  else
    return 1  # "false return value" in bash
  fi
}

# This is used by yb_test.sh.
# Takes relative test name such as "bin/client-test" or "bin/db_test".
is_known_non_gtest_test_by_rel_path() {
  if [[ $# -ne 1 ]]; then
    fatal "is_known_non_gtest_test_by_rel_path takes exactly one argument" \
         "(test binary path relative to the build directory)"
  fi
  local rel_test_path=$1
  validate_relative_test_binary_path "$rel_test_path"

  local test_binary_basename=${rel_test_path##*/}

  # Remove .sh extensions for bash-based tests.
  local test_binary_basename_no_ext=${test_binary_basename%[.]sh}

  is_known_non_gtest_test "$test_binary_basename_no_ext"
}

# Collects Google Test tests from the given test binary.
# Input:
#   rel_test_binary
#     Test binary path relative to the build directory. Copied from the arguemnt.
#   YB_GTEST_FILTER
#     If this is set, this is passed to the test binary as --gtest_filter=... along with
#     --gtest_list_tests, so that we only run a subset of tests. The reason this is an all-uppercase
#     variable is that we sometimes set it as an environment variable passed down to run-test.sh.
# Output variables:
#   tests
#     This is an array that should be initialized by the caller. This adds new "test descriptors" to
#     this array.
#   num_binaries_to_run_at_once
#     The number of binaries to be run in one shot. This variable is incremented if it exists.
#   num_test_cases
#     Total number of test cases collected. Test cases are Google Test's way of grouping tests
#     (test functions) together. Usually they are methods of the same test class.
#     This variable is incremented if it exists.
#   num_tests
#     Total number of tests (test functions) collected. This variable is incremented if it exists.
# Also, this function unsets test_log_path if it was previously set.
collect_gtest_tests() {
  expect_num_args 0 "$@"

  validate_relative_test_binary_path "$rel_test_binary"

  if [[ $rel_test_binary == "bin/db_sanity_test" ]]; then
    # db_sanity_check is not a test, but a command-line tool.
    return
  fi

  if is_known_non_gtest_test_by_rel_path "$rel_test_binary" || \
     [[ "$rel_test_binary" =~ $TEST_BINARIES_TO_RUN_AT_ONCE_RE ]]; then
    tests+=( "$rel_test_binary" )
    if [ -n "${num_binaries_to_run_at_once:-}" ]; then
      (( num_binaries_to_run_at_once+=1 ))
    fi
    return
  fi

  local gtest_list_stderr_path
  gtest_list_stderr_path=$( mktemp -t "gtest_list_tests.stderr.XXXXXXXXXX" )

  local abs_test_binary_path
  abs_test_binary_path=$( create_abs_test_binary_path "$rel_test_binary" )
  validate_abs_test_binary_path "$abs_test_binary_path"

  local gtest_list_tests_tmp_dir
  gtest_list_tests_tmp_dir=$( mktemp -t -d "gtest_list_tests_tmp_dir.XXXXXXXXXX" )

  mkdir_safe "$gtest_list_tests_tmp_dir"  # on Mac OS X, mktemp does not create the directory
  local gtest_list_tests_cmd=( "$abs_test_binary_path" --gtest_list_tests )
  if [[ -n ${YB_GTEST_FILTER:-} ]]; then
    gtest_list_tests_cmd+=( "--gtest_filter=$YB_GTEST_FILTER" )
  fi

  # We need to list the tests without any user-specified sanitizer options, because that might
  # produce unintended output.
  local old_sanitizer_extra_options=${YB_SANITIZER_EXTRA_OPTIONS:-}
  local YB_SANITIZER_EXTRA_OPTIONS=""
  set_sanitizer_runtime_options

  set +e
  pushd "$gtest_list_tests_tmp_dir" >/dev/null
  local gtest_list_tests_result  # this has to be on a separate line to capture the exit code
  gtest_list_tests_result=$(
    "${gtest_list_tests_cmd[@]}" 2>"$gtest_list_stderr_path"
  )
  gtest_list_tests_exit_code=$?
  popd >/dev/null
  set -e

  YB_SANITIZER_EXTRA_OPTIONS=$old_sanitizer_extra_options
  set_sanitizer_runtime_options

  set_expected_core_dir "$gtest_list_tests_tmp_dir"
  test_log_path="$gtest_list_stderr_path"
  process_core_file
  unset test_log_path
  rm -rf "$gtest_list_tests_tmp_dir"

  if [[ -z $gtest_list_tests_result ]]; then
    if [[ -s $gtest_list_stderr_path ]]; then
      log "Standard error from ${gtest_list_tests_cmd[*]}:"
      cat "$gtest_list_stderr_path"
    fi
    fatal "Empty output from ${gtest_list_tests_cmd[*]}," \
          "exit code: $gtest_list_tests_exit_code."
  fi

  # https://nixtricks.wordpress.com/2013/01/09/sed-delete-the-lines-lying-in-between-two-patterns/
  # Use the following command to delete the lines lying between PATTERN-1 and PATTERN-2, including
  # the lines containing these patterns:
  # sed '/PATTERN-1/,/PATTERN-2/d' input.txt
  sed '/^Suppressions used:$/,/^-\{53\}$/d' \
    <"$gtest_list_stderr_path" | sed '/^-\{53\}$/d; /^\s*$/d;' | \
    ( grep -Ev "^(\
Starting tracking the heap|\
Dumping heap profile to .* \\(Exiting\\))|\
Shared library .* loaded at address 0x[0-9a-f]+$" || true ) \
    >"$gtest_list_stderr_path.filtered"

  # -s tests if the given file is non-empty
  if [[ -s "$gtest_list_stderr_path.filtered" ]]; then
    (
      echo
      echo "'$abs_test_binary_path' produced non-empty stderr output when run with" \
           "--gtest_list_tests:"
      echo "[ ==== STDERR OUTPUT START ========================================================== ]"
      cat "$gtest_list_stderr_path.filtered"
      echo "[ ==== STDERR OUTPUT END ============================================================ ]"
      echo "Please add the test '$rel_test_binary' to the appropriate list in"
      echo "common-test-env.sh or fix the underlying issue in the code."
    ) >&2

    rm -f "$gtest_list_stderr_path" "$gtest_list_stderr_path.filtered"
    exit 1
  fi

  rm -f "$gtest_list_stderr_path"

  if [ "$gtest_list_tests_exit_code" -ne 0 ]; then
    echo "'$rel_test_binary' does not seem to be a gtest test (--gtest_list_tests failed)" >&2
    echo "Please add this test to the appropriate blacklist in common-test-env.sh" >&2
    exit 1
  fi

  local test_list_item
  local test
  local IFS=$'\n'  # so that we can iterate through lines in $gtest_list_tests_result
  for test_list_item in $gtest_list_tests_result; do
    if [[ "$test_list_item" =~ ^\ \  ]]; then
      # This is a "test": an individual piece of test code to run, described by TEST or TEST_F
      # in a Google Test program.
      test=${test_list_item#  }  # Remove two leading spaces
      test=${test%%#*}  # Remove everything after a "#" comment
      test=${test%  }  # Remove two trailing spaces
      if [[ -n ${total_num_tests:-} ]]; then
        (( total_num_tests+=1 ))
      fi
      tests+=( "${rel_test_binary}$TEST_DESCRIPTOR_SEPARATOR$test_case$test" )
      if [[ -n ${num_tests:-} ]]; then
        (( num_tests+=1 ))
      fi
    else
      # This is a "test case", a "container" for a number of tests, as in
      # TEST(TestCaseName, TestName). There is no executable item here.
      test_case=${test_list_item%%#*}  # Remove everything after a "#" comment
      test_case=${test_case%  }  # Remove two trailing spaces
      if [ -n "${num_test_cases:-}" ]; then
        (( num_test_cases+=1 ))
      fi
    fi
  done
}

using_nfs() {
  if [[ $YB_SRC_ROOT =~ ^/n/ ]]; then
    return 0
  fi
  return 1
}

create_test_tmpdir() {
  export TEST_TMPDIR="/tmp/yb_test.tmp.$RANDOM.$RANDOM.$RANDOM.pid$$"
  mkdir_safe "$TEST_TMPDIR"
}

# Set a number of variables in preparation to running a test.
# Inputs:
#   test_descriptor
#     Identifies the test. Consists fo the relative test binary, optionally followed by
#     "$TEST_DESCRIPTOR_SEPARATOR" and a gtest-compatible test description.
#   test_attempt_index
#     If this is set to a number, it is appended (followed by an underscore) to test output
#     directory, work directory, JUnit xml file name, and other file/directory paths that are
#     expected to be unique. This allows to run each test multiple times within the same test suite
#     run.
# Outputs:
#   rel_test_binary
#     Relative path of the test binary
#   test_name
#     Test name within the test binary, to be passed to --gtest_filter=...
#   junit_test_case_id
#     Test ID for JUnit report in format: <Test suite name>.<Test case name>, used to generate
#     XML in case it is not possible to parse test case info from test log file (non-gtest binary,
#     crashed to early).
#   run_at_once
#     Whether we need to run all tests in this binary at once ("true" or "false")
#   what_test_str
#     A human-readable string describing what tests we are running within the test binary.
#   rel_test_log_path_prefix
#     A prefix of various log paths for this test relative to the root directory we're using for
#     all test logs and temporary files (YB_TEST_LOG_ROOT_DIR).
#   is_gtest_test
#     Whether this is a Google Test test ("true" or "false")
#   TEST_TMPDIR (exported)
#     The temporary directory for the test to use. The test should also run in this directory so
#     that the core file would be generated there, if any.
#   test_cmd_line
#     An array representing the test command line to run, including the JUnit-compatible test result
#     XML file location and --gtest_filter=..., if applicable.
#   test_log_path
#     The final log path (after any applicable filtering and appending additional debug info).
#   xml_output_file
#     The location at which JUnit-compatible test result XML file should be generated.
#   test_failed
#     This is being set to "false" initially.
#   test_log_url
#     Test log URL served by Jenkins. This is only set when running on Jenkins.
#
# This function also removes the old XML output file at the path "$xml_output_file".

prepare_for_running_cxx_test() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set \
    YB_TEST_LOG_ROOT_DIR \
    test_descriptor
  validate_test_descriptor "$test_descriptor"

  local test_attempt_suffix=""
  if [[ -n ${test_attempt_index:-} ]]; then
    if [[ ! ${test_attempt_index} =~ ^[0-9]+$ ]]; then
      fatal "test_attempt_index is expected to be a number, found '$test_attempt_index'"
    fi
    test_attempt_suffix="__attempt_$test_attempt_index"
  fi

  if [[ "$test_descriptor" =~ $TEST_DESCRIPTOR_SEPARATOR ]]; then
    rel_test_binary=${test_descriptor%$TEST_DESCRIPTOR_SEPARATOR*}
    test_name=${test_descriptor#*$TEST_DESCRIPTOR_SEPARATOR}
    run_at_once=false
    junit_test_case_id=$test_name
  else
    # Run all test cases in the given test binary
    rel_test_binary=$test_descriptor
    test_name=""
    run_at_once=true
    junit_test_case_id="${rel_test_binary##*/}.all"
  fi

  local test_binary_sanitized
  test_binary_sanitized=$( sanitize_for_path "$rel_test_binary" )

  local test_name_sanitized
  test_name_sanitized=$( sanitize_for_path "$test_name" )

  test_dir=$YB_TEST_LOG_ROOT_DIR/$test_binary_sanitized
  mkdir_safe "$test_dir"

  rel_test_log_path_prefix="$test_binary_sanitized"
  if "$run_at_once"; then
    # Make this similar to the case when we run tests separately. Pretend that the test binary name
    # is the test name.
    rel_test_log_path_prefix+="/${rel_test_binary##*/}"
  else
    rel_test_log_path_prefix+="/$test_name_sanitized"
  fi

  # Use a separate directory to store results of every test attempt.
  rel_test_log_path_prefix+=$test_attempt_suffix

  # At this point $rel_test_log_path contains the common prefix of the log (.log), the temporary
  # directory (.tmp), and the XML report (.xml) for this test relative to the yb-test-logs
  # directory.

  local abs_test_binary_path
  abs_test_binary_path=$( create_abs_test_binary_path "$rel_test_binary" )

  # Word splitting is intentional here, so YB_EXTRA_GTEST_FLAGS is not quoted.
  # shellcheck disable=SC2206
  test_cmd_line=( "$abs_test_binary_path" ${YB_EXTRA_GTEST_FLAGS:-} )

  test_log_path_prefix="$YB_TEST_LOG_ROOT_DIR/$rel_test_log_path_prefix"
  register_test_artifact_files \
      "$test_log_path_prefix.*" \
      "${test_log_path_prefix}_test_report.json"
  xml_output_file="$test_log_path_prefix.xml"
  if is_known_non_gtest_test_by_rel_path "$rel_test_binary"; then
    is_gtest_test=false
  else
    test_cmd_line+=( "--gtest_output=xml:$xml_output_file" )
    is_gtest_test=true
  fi

  if ! "$run_at_once" && "$is_gtest_test"; then
    test_cmd_line+=( "--gtest_filter=$test_name" )
  fi

  create_test_tmpdir
  test_log_path="$test_log_path_prefix.log"

  # gtest won't overwrite old junit test files, resulting in a build failure
  # even when retries are successful.
  rm -f "$xml_output_file"

  test_failed=false

  if [[ -n ${BUILD_URL:-} ]]; then
    if [[ -z $test_log_url_prefix ]]; then
      echo "Expected test_log_url_prefix to be set when running on Jenkins." >&2
      exit 1
    fi
    test_log_url="$test_log_url_prefix/$rel_test_log_path_prefix.log"
  fi
}

# Set the directory to look for core files in to the given path. On Mac OS X, the input argument is
# ignored and the standard location is used instead.
set_expected_core_dir() {
  expect_num_args 1 "$@"
  local new_core_dir=$1
  if is_mac; then
    core_dir=/cores
  else
    core_dir="$new_core_dir"
  fi
}

# shellcheck disable=SC2120
set_debugger_input_for_core_stack_trace() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set \
    core_path \
    executable_path
  if is_mac; then
    debugger_cmd=( lldb "$executable_path" -c "$core_path" )
    debugger_input="thread backtrace ${thread_for_backtrace:-all}"  # backtrace
    # TODO: dump the current thread AND all threads, like we already do with gdb.
  else
    debugger_cmd=(
      gdb -q -n
      -ex 'bt'
      -ex 'thread apply all bt'
      -batch "$executable_path" "$core_path"
    )
    debugger_input=""
  fi
}

analyze_existing_core_file() {
  expect_num_args 2 "$@"
  local core_path=$1
  local executable_path=$2
  ensure_file_exists "$core_path"
  ensure_file_exists "$executable_path"
  ( echo "Found a core file at '$core_path', backtrace:" | tee -a "$append_output_to" ) >&2

  # The core might have been generated by yb-master or yb-tserver launched as part of an
  # ExternalMiniCluster.
  # TODO(mbautin): don't run gdb/lldb the second time in case we get the binary name right the
  #                first time around.

  local debugger_cmd debugger_input

  # shellcheck disable=SC2119
  set_debugger_input_for_core_stack_trace

  local debugger_output
  debugger_output=$( echo "$debugger_input" | "${debugger_cmd[@]}" 2>&1 )
  if [[ "$debugger_output" =~ Core\ was\ generated\ by\ .(yb-master|yb-tserver) ]]; then
    # The test program may be launching masters and tablet servers through an ExternalMiniCluster,
    # and the core file was generated by one of those masters and tablet servers.
    executable_path="$BUILD_ROOT/bin/${BASH_REMATCH[1]}"
    set_debugger_input_for_core_stack_trace
  fi

  set +e
  (
    set -x
    echo "$debugger_input" |
      "${debugger_cmd[@]}" 2>&1 |
      grep -Ev "^\[New LWP [0-9]+\]$" |
      "$YB_SRC_ROOT"/build-support/dedup_thread_stacks.py |
      tee -a "$append_output_to"
  ) >&2
  set -e
  echo >&2
  echo >&2
}

# Looks for a core dump file in the specified directory, shows a stack trace using gdb, and removes
# the core dump file to save space.
# Inputs:
#   rel_test_binary
#     Test binary path relative to $BUILD_ROOT
#   core_dir
#     The directory to look for a core dump file in.
#   test_log_path
#     If this is defined and is not empty, any output generated by this function is appended to the
#     file at this path.
process_core_file() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set \
    rel_test_binary
  local abs_test_binary_path=$BUILD_ROOT/$rel_test_binary

  local append_output_to=/dev/null
  if [[ -n ${test_log_path:-} ]]; then
    append_output_to=$test_log_path
  fi

  local thread_for_backtrace="all"
  if is_mac; then
    # Finding core files on Mac OS X.
    if [[ -f ${test_log_path:-} ]]; then
      set +e
      local signal_received_line
      signal_received_line=$(
        grep -E 'SIG[A-Z]+.*received by PID [0-9]+' "$test_log_path" | head -1
      )
      set -e
      local core_pid=""
      # Parsing a line like this:
      # *** SIGSEGV (@0x0) received by PID 46158 (TID 0x70000028d000) stack trace: ***
      if [[ $signal_received_line =~ received\ by\ PID\ ([0-9]+)\ \(TID\ (0x[0-9a-fA-F]+)\) ]]; then
        core_pid=${BASH_REMATCH[1]}
        # TODO: find a way to only show the backtrace of the relevant thread.  We can't just pass
        # the thread id to "thread backtrace", apparently it needs the thread index of some sort
        # that is not identical to thread id.
        # thread_for_backtrace=${BASH_REMATCH[2]}
      fi
      if [[ -n $core_pid ]]; then
        local core_path=$core_dir/core.$core_pid
        log "Determined core_path=$core_path"
        if [[ ! -f $core_path ]]; then
          log "Core file not found at presumed location '$core_path'."
        fi
      else
        if grep -Eq 'SIG[A-Z]+.*received by PID' "$test_log_path"; then
          log "Warning: could not determine test pid from '$test_log_path'" \
              "(matched log line: $signal_received_line)"
        fi
        return
      fi
    else
      log "Warning: test_log_path is not set, or file does not exist ('${test_log_path:-}')," \
          "cannot look for core files on Mac OS X"
      return
    fi
  else
    # Finding core files on Linux.
    if [[ ! -d $core_dir ]]; then
      echo "${FUNCNAME[0]}: directory '$core_dir' does not exist" >&2
      exit 1
    fi
    local core_path=$core_dir/core
    if [[ ! -f $core_path ]]; then
      # If there is just one file named "core.[0-9]+" in the core directory, pick it up and assume
      # it belongs to the test. This is necessary on some systems, e.g. CentOS workstations with no
      # special core location overrides.
      local core_candidates=()
      for core_path in "$core_dir"/core.*; do
        if [[ -f $core_path && $core_path =~ /core[.][0-9]+$ ]]; then
          core_candidates+=( "$core_path" )
        fi
      done
      if [[ ${#core_candidates[@]} -eq 1 ]]; then
        core_path=${core_candidates[0]}
      elif [[ ${#core_candidates[@]} -gt 1 ]]; then
        log "Found too many core files in '$core_dir', not analyzing: ${core_candidates[*]}"
        return
      fi
    fi
  fi

  if [[ -f $core_path ]]; then
    local core_binary_path=$abs_test_binary_path
    analyze_existing_core_file "$core_path" "$core_binary_path"
    rm -f "$core_path"  # to save disk space
  fi
}

# Used for both C++ and Java tests.
stop_process_tree_supervisor() {
  # process_supervisor_log_path should be set in run-test.sh.
  expect_vars_to_be_set process_supervisor_log_path

  process_supervisor_success=true
  if [[ ${process_tree_supervisor_pid:-0} -eq 0 ]]; then
    return
  fi

  if ! kill -SIGUSR1 "$process_tree_supervisor_pid"; then
    log "Warning: could not stop the process tree supervisor (pid $process_tree_supervisor_pid)." \
        "This could be because it has already terminated."
  fi

  set +e
  wait "$process_tree_supervisor_pid"
  declare -i supervisor_exit_code=$?
  set -e
  if [[ $supervisor_exit_code -eq $SIGUSR1_EXIT_CODE ]]; then
    # This could happen when we killed the supervisor script before it had a chance to set the
    # signal handler for SIGUSR1.
    supervisor_exit_code=0
  fi

  declare -i return_code=0
  if [[ $supervisor_exit_code -ne 0 ]]; then
    log "Process tree supervisor exited with code $supervisor_exit_code"
    process_supervisor_success=false
  fi

  # process_supervisor_log_path should be set in run-test.sh.
  # shellcheck disable=SC2154
  if [[ -f $process_supervisor_log_path ]]; then
    if is_jenkins || [[ $process_supervisor_success != "true" ]] || \
       grep -q YB_STRAY_PROCESS "$process_supervisor_log_path"; then
      log "Process supervisor log dump ($process_supervisor_log_path):"
      cat "$process_supervisor_log_path" >&2
    else
      log "Omitting process supervisor log, nothing interesting there"
    fi

    # TODO: Re-enable the logic below for treating stray processes as test failures, when the
    # following tests are fixed:
    # - org.yb.pgsql.TestPgRegressFeature.testPgRegressFeature
    # - org.yb.pgsql.TestPgRegressTypesNumeric.testPgRegressTypes
    # See https://github.com/YugaByte/yugabyte-db/issues/946 for details.
    if false && grep -q YB_STRAY_PROCESS "$process_supervisor_log_path"; then
      log "Stray processes reported in $process_supervisor_log_path, considering the test failed."
      log "The JUnit-compatible XML file will be updated to reflect this error."
      process_supervisor_success=false

      if [[ -f ${process_tree_supervisor_append_log_to_on_error:-} ]]; then
        log "Appending process supervisor log to $process_tree_supervisor_append_log_to_on_error"
        (
          echo "Process supervisor log:";
          cat "$process_supervisor_log_path"
        ) >>"$process_tree_supervisor_append_log_to_on_error"
      fi
    fi
    rm -f "$process_supervisor_log_path"
  fi

  # To make future calls to this function a no-op.
  process_tree_supervisor_pid=0
}

# Checks if the there is no XML file at path "$xml_output_file". This may happen if a gtest test
# suite fails before it has a chance to generate an XML output file. In that case, this function
# generates a substitute XML output file using parse_test_failure.py, but only if the log file
# exists at its expected location.
handle_cxx_test_xml_output() {
  expect_vars_to_be_set \
    rel_test_binary \
    test_log_path \
    xml_output_file

  if [[ ! -f "$xml_output_file" || ! -s "$xml_output_file" ]]; then
    # XML does not exist or empty (most probably due to test crash during XML generation)
    if is_known_non_gtest_test_by_rel_path "$rel_test_binary"; then
      echo "$rel_test_binary is a known non-gtest binary, OK that it did not produce XML" \
           "output" >&2
    else
      echo "$rel_test_binary failed to produce an XML output file at $xml_output_file" >&2
      test_failed=true
    fi
    if [[ ! -f "$test_log_path" ]]; then
      echo "Test log path '$test_log_path' does not exist"
      test_failed=true
      # parse_test_failure will also generate XML file in this case.
    fi
    echo "Generating an XML output file using parse_test_failure.py: $xml_output_file" >&2
    "$YB_SRC_ROOT"/build-support/parse_test_failure.py -x \
        "$junit_test_case_id" "$test_log_path" >"$xml_output_file"
  fi

  process_tree_supervisor_append_log_to_on_error=$test_log_path

  stop_process_tree_supervisor

  if ! "$process_supervisor_success"; then
    log "Process tree supervisor reported that the test failed"
    test_failed=true
  fi
  if "$test_failed"; then
    log "Test failed, updating $xml_output_file"
  else
    log "Test succeeded, updating $xml_output_file"
  fi
  update_test_result_xml_cmd=(
    "$YB_SRC_ROOT"/build-support/update_test_result_xml.py
    --result-xml "$xml_output_file"
    --mark-as-failed "$test_failed"
  )
  if [[ -n ${test_log_url:-} ]]; then
    update_test_result_xml_cmd+=( --log-url "$test_log_url" )
  fi
  ( set -x; "${update_test_result_xml_cmd[@]}" )

  # Useful for distributed builds in an NFS environment.
  chmod g+w "$xml_output_file"
}

# Sets test log URL prefix if we're running in Jenkins.
set_test_log_url_prefix() {
  test_log_url_prefix=""
  local build_dir_name=${BUILD_ROOT##*/}
  if [[ -n ${BUILD_URL:-} ]]; then
    build_url_no_trailing_slash=${BUILD_URL%/}
    test_log_url_prefix="${build_url_no_trailing_slash}/artifact/build/$build_dir_name/yb-test-logs"
  fi
}

# shellcheck disable=SC2120
determine_test_timeout() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set rel_test_binary
  if [[ -n ${YB_TEST_TIMEOUT:-} ]]; then
    timeout_sec=$YB_TEST_TIMEOUT
  else
    if [[ $rel_test_binary == "tests-pgwrapper/create_initial_sys_catalog_snapshot" || \
          $rel_test_binary == "tests-pgwrapper/pg_libpq-test" || \
          $rel_test_binary == "tests-pgwrapper/pg_libpq_err-test" || \
          $rel_test_binary == "tests-pgwrapper/pg_mini-test" || \
          $rel_test_binary == "tests-pgwrapper/pg_wrapper-test" ]]; then
      timeout_sec=$INCREASED_TEST_TIMEOUT_SEC
    else
      timeout_sec=$DEFAULT_TEST_TIMEOUT_SEC
    fi
  fi
}

try_set_ulimited_ulimit() {
  # Setting the ulimit may fail with an error message, and that's what we want. We will still
  # run the test.  In case we do manage to set the core file size limit, the caller can restore the
  # previous limit after exiting a subshell.
  if ! ulimit -c unlimited; then
    # Print some diagnostics if we fail to set core file size limit.
    log "Command 'ulimit -c unlimited' failed. Current 'ulimit -c' output: $( ulimit -c )"
  fi
}

# YB_CTEST_VERBOSE makes test output go to stderr, and then we separately make it show up on
# the console by giving ctest the --verbose option. This is intended for development. When we
# run tests on Jenkins or when running all tests using "ctest -j8" from the build root, one
# should leave YB_CTEST_VERBOSE unset.
is_ctest_verbose() {
  [[ ${YB_CTEST_VERBOSE:-0} == "1" ]]
}

run_one_cxx_test() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set \
    BUILD_ROOT \
    TEST_TMPDIR \
    test_cmd_line \
    test_failed \
    rel_test_binary

  # We expect the exact string "false" here for added safety.
  if [[ $test_failed != "false" ]]; then
    fatal "Expected test_failed to be false before running test, found: $test_failed"
  fi

  # shellcheck disable=SC2119
  determine_test_timeout

  local test_wrapper_cmd_line=(
    "$BUILD_ROOT"/bin/run-with-timeout $(( timeout_sec + 1 )) "${test_cmd_line[@]}"
  )
  if [[ $TEST_TMPDIR == "/" || $TEST_TMPDIR == "/tmp" ]]; then
    # Let's be paranoid because we'll be deleting everything inside this directory.
    fatal "Invalid TEST_TMPDIR: '$TEST_TMPDIR': must be a unique temporary directory."
  fi
  pushd "$TEST_TMPDIR" >/dev/null

  export YB_FATAL_DETAILS_PATH_PREFIX=$test_log_path_prefix.fatal_failure_details

  about_to_start_running_test

  local attempts_left
  for attempts_left in {1..0}; do
    # Clean up anything that might have been left from the previous attempt.
    rm -rf "${TEST_TMPDIR:-/tmp/yb_never_delete_entire_filesystem}"/*

    set +e
    (
      try_set_ulimited_ulimit

      if is_ctest_verbose; then
        ( set -x; "${test_wrapper_cmd_line[@]}" 2>&1 ) | tee "$test_log_path"
        # Propagate the exit code of the test process, not any of the filters. This will only exit
        # this subshell, not the entire script calling this function.
        exit "${PIPESTATUS[0]}"
      else
        "${test_wrapper_cmd_line[@]}" &>"$test_log_path"
      fi
    )
    test_exit_code=$?
    set -e

    # Useful for distributed builds in an NFS environment.
    chmod g+w "$test_log_path"

    # Test did not fail, no need to retry.
    if [[ $test_exit_code -eq 0 ]]; then
      break
    fi

    # See if the test failed due to "Address already in use" and log a message if we still have more
    # attempts left.
    if [[ $attempts_left -gt 0 ]] && \
       grep -Eq "$TEST_RESTART_PATTERN" "$test_log_path"; then
      log "Found one of the intermittent error patterns in the log, restarting the test (once):"
      grep -E "$TEST_RESTART_PATTERN" "$test_log_path" >&2
    else
      # Avoid retrying any other kinds of failures.
      break
    fi
  done

  popd >/dev/null

  if [[ $test_exit_code -ne 0 ]]; then
    test_failed=true
  fi
}

# shellcheck disable=SC2120
handle_cxx_test_failure() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set \
    TEST_TMPDIR \
    global_exit_code \
    rel_test_log_path_prefix \
    test_exit_code \
    test_cmd_line \
    test_log_path

  if ! "$test_failed" & ! did_test_succeed "$test_exit_code" "$test_log_path"; then
    test_failed=true
  fi

  if "$test_failed"; then
    (
      echo
      echo "TEST FAILURE"
      echo "Test command: ${test_cmd_line[*]}"
      echo "Test exit status: $test_exit_code"
      echo "Log path: $test_log_path"
      if [[ -n ${BUILD_URL:-} ]]; then
        if [[ -z ${test_log_url_prefix:-} ]]; then
          fatal "Expected test_log_url_prefix to be set if BUILD_URL is defined"
        fi
        # Produce a URL like
        # https://jenkins.dev.yugabyte.com/job/yugabyte-with-custom-test-script/47/artifact/build/debug/yb-test-logs/bin__raft_consensus-itest/RaftConsensusITest_TestChurnyElections.log
        echo "Log URL: $test_log_url_prefix/$rel_test_log_path_prefix.log"
      fi

      # Show some context from the test log, but only do so if we are not already showing the entire
      # test log when invoking tests directly in yb_build.sh.
      if ! is_ctest_verbose && [[ -f $test_log_path ]] &&
         grep -Eq "$RELEVANT_LOG_LINES_RE" "$test_log_path"; then
        echo "Relevant log lines:"
        grep -E -C 3 "$RELEVANT_LOG_LINES_RE" "$test_log_path"
      fi
    ) >&2
    set_expected_core_dir "$TEST_TMPDIR"
    process_core_file
    unset core_dir
    global_exit_code=1
  fi
}

delete_successful_output_if_needed() {
  expect_vars_to_be_set \
    TEST_TMPDIR \
    test_log_path
  if is_jenkins && ! "$test_failed"; then
    # Delete test output after a successful test run to minimize network traffic and disk usage on
    # Jenkins.
    rm -rf "$test_log_path" "$TEST_TMPDIR"
    return
  fi
  if is_src_root_on_nfs; then
    log "Removing temporary test data directory: $TEST_TMPDIR"
    rm -rf "$TEST_TMPDIR"
  fi
}

run_postproces_test_result_script() {
  local args=(
    --yb-src-root "$YB_SRC_ROOT"
    --build-root "$BUILD_ROOT"
    --extra-error-log-path "${YB_TEST_EXTRA_ERROR_LOG_PATH:-}"
  )
  if [[ -n ${YB_FATAL_DETAILS_PATH_PREFIX:-} ]]; then
    args+=(
      --fatal-details-path-prefix "$YB_FATAL_DETAILS_PATH_PREFIX"
    )
  fi
  "$VIRTUAL_ENV/bin/python" "$YB_SRC_ROOT/python/yb/postprocess_test_result.py" \
    "${args[@]}" "$@"
}

run_cxx_test_and_process_results() {
  run_one_cxx_test
  handle_cxx_test_failure
  handle_cxx_test_xml_output

  run_postproces_test_result_script \
    --test-log-path "$test_log_path" \
    --junit-xml-path "$xml_output_file" \
    --language cxx \
    --cxx-rel-test-binary "$rel_test_binary" \
    --test-failed "$test_failed"

  delete_successful_output_if_needed
}

set_sanitizer_runtime_options() {
  expect_vars_to_be_set BUILD_ROOT YB_COMPILER_TYPE

  # Don't allow setting these options directly from outside. We will allow controlling them through
  # our own "extra options" environment variables.
  export ASAN_OPTIONS=""
  export TSAN_OPTIONS=""
  export LSAN_OPTIONS=""
  export UBSAN_OPTIONS=""

  local -r build_root_basename=${BUILD_ROOT##*/}

  # TODO: is YB_COMPILER_TYPE defined here? We could use it to detect if we are using GCC instead of
  # parsing the build root basename.

  # We don't add a hyphen in the end of the following regex, because there is a "tsan_slow" build
  # type.
  if [[ $build_root_basename =~ ^(asan|tsan) &&
        ! $build_root_basename =~ ^.*-gcc[0-9]*-.*$ ]]; then
    # Suppressions require symbolization. We'll default to using the symbolizer in thirdparty.
    # If ASAN_SYMBOLIZER_PATH is already set but that file does not exist, we'll report that and
    # still use the default way to find the symbolizer.
    local asan_symbolizer_candidate_paths=""
    if [[ -z ${ASAN_SYMBOLIZER_PATH:-} || ! -f ${ASAN_SYMBOLIZER_PATH:-} ]]; then
      if [[ -n "${ASAN_SYMBOLIZER_PATH:-}" ]]; then
        log "ASAN_SYMBOLIZER_PATH is set to '$ASAN_SYMBOLIZER_PATH' but that file does not " \
            "exist, reverting to default behavior."
      fi

      local candidate_bin_dirs=(
        "$YB_THIRDPARTY_DIR/installed/bin"
        "$YB_THIRDPARTY_DIR/clang-toolchain/bin"
      )
      # When building with clang10 or clang11, normally $YB_RESOLVED_CC_COMPILER would contain the
      # path of clang within LLVM's bin directory.
      if [[ -z ${cc_executable:-} ]]; then
        find_compiler_by_type
        if [[ -z ${cc_executable:-} ]]; then
          fatal "find_compiler_by_type did not set the cc_executable variable"
        fi
      fi
      if [[ $cc_executable == */clang ]]; then
        candidate_bin_dirs+=( "${cc_executable%/clang}" )
      fi
      for asan_symbolizer_candidate_bin_dir in "${candidate_bin_dirs[@]}"; do
        local asan_symbolizer_candidate_path=$asan_symbolizer_candidate_bin_dir/llvm-symbolizer
        if [[ -f "$asan_symbolizer_candidate_path" ]]; then
          ASAN_SYMBOLIZER_PATH=$asan_symbolizer_candidate_path
          break
        fi
        asan_symbolizer_candidate_paths+=" $asan_symbolizer_candidate_path"
      done
    fi

    if [[ -n ${ASAN_SYMBOLIZER_PATH:-} ]]; then
      if [[ ! -f $ASAN_SYMBOLIZER_PATH ]]; then
        log "ASAN symbolizer at '$ASAN_SYMBOLIZER_PATH' still does not exist."
        ( set -x; ls -l "$ASAN_SYMBOLIZER_PATH" )
      elif [[ ! -x $ASAN_SYMBOLIZER_PATH ]]; then
        log "ASAN symbolizer at '$ASAN_SYMBOLIZER_PATH' is not binary, updating permissions."
        ( set -x; chmod a+x "$ASAN_SYMBOLIZER_PATH" )
      fi

      export ASAN_SYMBOLIZER_PATH
    else
      fatal "ASAN_SYMBOLIZER_PATH has not been set and llvm-symbolizer not found at any of" \
            "the candidate paths:$asan_symbolizer_candidate_paths"
    fi
  fi

  if [[ $build_root_basename =~ ^asan- ]]; then
    # Enable leak detection even under LLVM 3.4, where it was disabled by default.
    # This flag only takes effect when running an ASAN build.
    export ASAN_OPTIONS="detect_leaks=1 disable_coredump=0"

    # Set up suppressions for LeakSanitizer
    LSAN_OPTIONS="suppressions=$YB_SRC_ROOT/build-support/lsan-suppressions.txt"

    # If we print out object addresses somewhere else, we can match them to LSAN-reported
    # addresses of leaked objects.
    LSAN_OPTIONS+=" report_objects=1"
    export LSAN_OPTIONS

    # Enable stack traces for UBSAN failures
    UBSAN_OPTIONS="print_stacktrace=1"
    local ubsan_suppressions_path=$YB_SRC_ROOT/build-support/ubsan-suppressions.txt
    ensure_file_exists "$ubsan_suppressions_path"
    UBSAN_OPTIONS+=" suppressions=$ubsan_suppressions_path"
    export UBSAN_OPTIONS
  fi

  # Don't add a hyphen after the regex so we can handle both tsan and tsan_slow.
  if [[ $build_root_basename =~ ^tsan ]]; then
    # Configure TSAN (ignored if this isn't a TSAN build).
    #
    # Deadlock detection (new in clang 3.5) is disabled because:
    # 1. The clang 3.5 deadlock detector crashes in some YB unit tests. It
    #    needs compiler-rt commits c4c3dfd, 9a8efe3, and possibly others.
    # 2. Many unit tests report lock-order-inversion warnings; they should be
    #    fixed before reenabling the detector.
    TSAN_OPTIONS="detect_deadlocks=0"
    TSAN_OPTIONS+=" suppressions=$YB_SRC_ROOT/build-support/tsan-suppressions.txt"
    TSAN_OPTIONS+=" history_size=7"
    TSAN_OPTIONS+=" external_symbolizer_path=$ASAN_SYMBOLIZER_PATH"
    if [[ ${YB_SANITIZERS_ENABLE_COREDUMP:-0} == "1" ]]; then
      TSAN_OPTIONS+=" disable_coredump=false"
    fi
    export TSAN_OPTIONS
  fi

  local extra_opts=${YB_SANITIZER_EXTRA_OPTIONS:-}
  export ASAN_OPTIONS="$SANITIZER_COMMON_OPTIONS ${ASAN_OPTIONS:-} $extra_opts"
  export LSAN_OPTIONS="$SANITIZER_COMMON_OPTIONS ${LSAN_OPTIONS:-} $extra_opts"
  export TSAN_OPTIONS="$SANITIZER_COMMON_OPTIONS ${TSAN_OPTIONS:-} $extra_opts"
  export UBSAN_OPTIONS="$SANITIZER_COMMON_OPTIONS ${UBSAN_OPTIONS:-} $extra_opts"
}

did_test_succeed() {
  expect_num_args 2 "$@"
  local -i -r exit_code=$1
  local -r log_path=$2
  if [[ $exit_code -ne 0 ]]; then
    log "Test failure reason: exit code: $exit_code"
    return 1  # "false" value in bash, meaning the test failed
  fi

  if [[ ! -f "$log_path" ]]; then
    log "Test failure reason: Log path '$log_path' not found."
    return 1
  fi

  if grep -q 'Running 0 tests from 0 test cases' "$log_path" && \
     ! grep -Eq 'YOU HAVE [[:digit:]]+ DISABLED TEST' "$log_path"; then
    log 'Test failure reason: No tests were run, and no disabled tests found, invalid test filter?'
    return 1
  fi

  if grep -q 'LeakSanitizer: detected memory leaks' "$log_path"; then
    log 'Test failure reason: Detected memory leaks'
    return 1
  fi

  if grep -q 'AddressSanitizer: heap-use-after-free' "$log_path"; then
    log 'Test failure reason: Detected use of freed memory'
    return 1
  fi

  if grep -q 'AddressSanitizer: undefined-behavior' "$log_path"; then
    log 'Test failure reason: Detected ASAN undefined behavior'
    return 1
  fi

  if grep -q 'UndefinedBehaviorSanitizer: undefined-behavior' "$log_path"; then
    log 'Test failure reason: Detected UBSAN undefined behavior'
    return 1
  fi

  if grep -q 'ThreadSanitizer' "$log_path"; then
    log 'Test failure reason: ThreadSanitizer failures'
    return 1
  fi

  if grep -Eq 'Leak check.*detected leaks' "$log_path"; then
    log 'Test failure reason: Leak check failures'
    return 1
  fi

  if grep -Eq 'Segmentation fault: ' "$log_path"; then
    log 'Test failure reason: Segmentation fault'
    return 1
  fi

  if grep -Eq '^\[  FAILED  \]' "$log_path"; then
    log 'Test failure reason: GTest failures'
    return 1
  fi

  # When signals show up in the test log, that's usually not good. We can see how many of these we
  # get and gradually prune false positives.
  local signal_str
  for signal_str in \
      SIGHUP \
      SIGINT \
      SIGQUIT \
      SIGILL \
      SIGTRAP \
      SIGIOT \
      SIGBUS \
      SIGFPE \
      SIGKILL \
      SIGUSR1 \
      SIGSEGV \
      SIGUSR2 \
      SIGPIPE \
      SIGALRM \
      SIGTERM \
      SIGSTKFLT \
      SIGCHLD \
      SIGCONT \
      SIGSTOP \
      SIGTSTP \
      SIGTTIN \
      SIGTTOU \
      SIGURG \
      SIGXCPU \
      SIGXFSZ \
      SIGVTALRM \
      SIGPROF \
      SIGWINCH \
      SIGIO \
      SIGPWR; do
    if grep -q " $signal_str " "$log_path"; then
      log "Test failure reason: Caught signal: $signal_str"
      return 1
    fi
  done

  if grep -q 'Check failed: ' "$log_path"; then
    log 'Test failure reason: Check failed'
    return 1
  fi

  if grep -Eq '^\[INFO\] BUILD FAILURE$' "$log_path"; then
    log "Test failure reason: Java build or tests failed"
    return 1
  fi

  return 0
}

find_test_binary() {
  expect_num_args 1 "$@"
  local binary_name=$1
  expect_vars_to_be_set BUILD_ROOT
  result=$(
    find "$BUILD_ROOT/$VALID_TEST_BINARY_DIRS_PREFIX-"* \
         -name "$binary_name" \
         -print \
         -quit
  )
  if [[ -f $result ]]; then
    echo "$result"
    return
  else
    fatal "Could not find test binary '$binary_name' inside $BUILD_ROOT"
  fi
}

show_disk_usage() {
  heading "Disk usage (df -h)"

  df -h

  echo
  horizontal_line
  echo
}

find_spark_submit_cmd() {
  if [[ -n ${YB_SPARK_SUBMIT_CMD_OVERRIDE:-} ]]; then
    spark_submit_cmd_path=$YB_SPARK_SUBMIT_CMD_OVERRIDE
    return
  fi

  if is_mac; then
    spark_submit_cmd_path=$YB_MACOS_PY3_SPARK_SUBMIT_CMD
    return
  fi

  if [[ $build_type == "tsan" || $build_type == "asan" ]]; then
    spark_submit_cmd_path=$YB_ASAN_TSAN_PY3_SPARK_SUBMIT_CMD
    return
  fi

  spark_submit_cmd_path=$YB_LINUX_PY3_SPARK_SUBMIT_CMD
}

spark_available() {
  find_spark_submit_cmd
  log "spark_submit_cmd_path=$spark_submit_cmd_path"
  if [[ -x $spark_submit_cmd_path ]]; then
    return 0  # true
  fi
  log "File $spark_submit_cmd_path not found or not executable, Spark is unavailable"
  return 1  # false
}

run_tests_on_spark() {
  if ! spark_available; then
    fatal "Spark is not available, can't run tests on Spark"
  fi
  log "Running tests on Spark"
  local return_code
  local run_tests_args=(
    --build-root "$BUILD_ROOT"
    --save_report_to_build_dir
  )
  if is_jenkins && [[ -d $JENKINS_NFS_BUILD_REPORT_BASE_DIR ]]; then
    run_tests_args+=( "--reports-dir" "$JENKINS_NFS_BUILD_REPORT_BASE_DIR" --write_report )
  fi

  set +e
  (
    set -x
    # Run Spark and filter out some boring output (or we'll end up with 6000 lines, two per test).
    # We still keep "Finished" lines ending with "(x/y)" where x is divisible by 10. Example:
    # Finished task 2791.0 in stage 0.0 (TID 2791) in 10436 ms on <ip> (executor 3) (2900/2908)
    time "$spark_submit_cmd_path" \
      --driver-cores "$INITIAL_SPARK_DRIVER_CORES" \
      "$YB_SRC_ROOT/build-support/run_tests_on_spark.py" \
      "${run_tests_args[@]}" "$@" 2>&1 | \
      grep -Ev "TaskSetManager: (Starting task|Finished task .* \([0-9]+[1-9]/[0-9]+\))" \
           --line-buffered
    exit "${PIPESTATUS[0]}"
  )
  return_code=$?
  set -e
  log "Finished running tests on Spark (timing information available above)," \
      "exit code: $return_code"
  return $return_code
}

# Check that all test binaries referenced by CMakeLists.txt files exist. Takes one optional
# parameter, which is the pattern to look for in the ctest output.
check_test_existence() {
  set +e
  local pattern=${1:-Failed}
  YB_CHECK_TEST_EXISTENCE_ONLY=1 ctest -j "$YB_NUM_CPUS" "$@" 2>&1 | grep -E "$pattern"
  local ctest_exit_code=${PIPESTATUS[0]}
  set -e
  return "$ctest_exit_code"
}

# Validate and adjust the cxx_test_name variable if necessary, by adding a prefix based on the
# directory name. Also sets test_binary_name.
fix_cxx_test_name() {
  local dir_prefix=${cxx_test_name%%_*}
  test_binary_name=${cxx_test_name#*_}
  local possible_corrections=()
  local possible_binary_paths=()
  if [[ ! -f $BUILD_ROOT/tests-$dir_prefix/$test_binary_name ]]; then
    local tests_dir
    for tests_dir in "$BUILD_ROOT/tests-"*; do
      local test_binary_path=$tests_dir/$cxx_test_name
      if [[ -f $test_binary_path ]]; then
        local new_cxx_test_name=${tests_dir##*/tests-}_$cxx_test_name
        possible_corrections+=( "$new_cxx_test_name" )
        possible_binary_paths+=( "$test_binary_path" )
      fi
    done
    case ${#possible_corrections[@]} in
      0)
        log "$cxx_test_name does not look like a valid test name, but we could not auto-detect" \
            "the right directory prefix (based on files in $BUILD_ROOT/tests-*) to put in front" \
            "of it."
      ;;
      1)
        log "Auto-correcting $cxx_test_name -> ${possible_corrections[0]}"
        test_binary_name=$cxx_test_name
        cxx_test_name=${possible_corrections[0]}
      ;;
      *)
        local most_recent_binary_path
        # We are OK with using the ls command here and not the find command to find the latest
        # file.
        # shellcheck disable=SC2012
        most_recent_binary_path=$( ls -t "${possible_binary_paths[@]}" | head -1 )
        if [[ ! -f $most_recent_binary_path ]]; then
          fatal "Failed to detect the most recently modified file out of:" \
                "${possible_binary_paths[@]}"
        fi
        new_cxx_test_dir=${most_recent_binary_path%/*}
        new_cxx_test_name=${new_cxx_test_dir##*/tests-}_$cxx_test_name
        log "Ambiguous ways to correct $cxx_test_name: ${possible_corrections[*]}," \
            "using the one corresponding to the most recent file: $new_cxx_test_name"
        cxx_test_name=$new_cxx_test_name
      ;;
    esac
  fi
}

# Fixes cxx_test_name based on gtest_filter
fix_gtest_cxx_test_name() {
  if [[ $cxx_test_name != GTEST_* || $make_program != *ninja ]]; then
    return
  fi
  local gtest_name=${cxx_test_name#*_}
  local targets
  targets=$("$make_program" -t targets all)
  local IFS=""
  while read -r line; do
    # Sample lines from "ninja -t targets all"
    # CMakeFiles/latest_symlink: CUSTOM_COMMAND
    # src/yb/rocksdb/CMakeFiles/edit_cache.util: CUSTOM_COMMAND
    # src/yb/rocksdb/edit_cache: phony
    # src/yb/rocksdb/rocksdb_transaction_test: phony
    # cmake_object_order_depends_target_transaction_test: phony
    # tests-rocksdb/transaction_test: CXX_EXECUTABLE_LINKER__transaction_test
    if [[ ${line#*:} == " phony" ]]; then
      local target_name=${line%%:*}
      local stripped_target_name="${target_name//[_-]/}"
      if [[ $stripped_target_name == "$gtest_name" ]]; then
        log "Found target $target_name for $gtest_name"
        cxx_test_name=$target_name
        return
      fi
    fi
  done <<< "$targets"

  log "Unable to find test matching gtest_filter $YB_GTEST_FILTER"
}

# This is called immediately before we start running the test. The Spark-based test runner watches
# for this "flag file" to appear to catch a case where run-test.sh gets totally stuck on macOS
# before even getting to the test.
about_to_start_running_test() {
  if [[ -n ${YB_TEST_STARTED_RUNNING_FLAG_FILE:-} ]]; then
    touch "$YB_TEST_STARTED_RUNNING_FLAG_FILE"
    log "Created file $YB_TEST_STARTED_RUNNING_FLAG_FILE to indicate that the test is starting"
  fi
}

# Arguments: <maven_module_name> <test_class_and_maybe_method>
# The second argument could have slashes instead of dots, and could have an optional .java
# extension.
# Examples:
# - yb-client org.yb.client.TestYBClient
# - yb-client org.yb.client.TestYBClient#testAllMasterChangeConfig
#
# <maven_module_name> could also be the module directory relative to $YB_SRC_ROOT, e.g.
# java/yb-cql.
run_java_test() {
  expect_num_args 2 "$@"
  local module_name_or_rel_module_dir=$1
  local test_class_and_maybe_method=${2%.java}
  test_class_and_maybe_method=${test_class_and_maybe_method%.scala}
  if [[ $module_name_or_rel_module_dir == */* ]]; then
    # E.g. java/yb-cql.
    local module_dir=$YB_SRC_ROOT/$module_name_or_rel_module_dir
    module_name=${module_name_or_rel_module_dir##*/}
  else
    # E.g. yb-cql.
    local module_name=$module_name_or_rel_module_dir
    local module_dir=$YB_SRC_ROOT/java/$module_name
  fi
  ensure_directory_exists "$module_dir"
  local java_project_dir=${module_dir%/*}
  if [[ $java_project_dir != */java ]]; then
    fatal "Expected the Java module directory '$module_dir' to have the form of" \
          ".../java/<module_name>. Trying to run test: $test_class_and_maybe_method."
  fi

  local test_method_name=""
  if [[ $test_class_and_maybe_method == *\#* ]]; then
    test_method_name=${test_class_and_maybe_method##*#}
  fi
  local test_class=${test_class_and_maybe_method%#*}

  if [[ -z ${BUILD_ROOT:-} ]]; then
    fatal "Running Java tests requires that BUILD_ROOT be set"
  fi
  set_mvn_parameters

  set_sanitizer_runtime_options
  mkdir -p "$YB_TEST_LOG_ROOT_DIR/java"

  # This can't include $YB_TEST_INVOCATION_ID -- previously, when we did that, it looked like some
  # Maven processes were killed, although it is not clear why, because they should have already
  # completed by the time we start looking for $YB_TEST_INVOCATION_ID in test names and killing
  # processes.
  local timestamp
  timestamp=$( get_timestamp_for_filenames )

  local surefire_rel_tmp_dir=surefire${timestamp}_${RANDOM}_${RANDOM}_${RANDOM}_$$

  # This should change the directory to "java".
  cd "$module_dir"/..

  # We specify tempDir to use a separate temporary directory for each test.
  # http://maven.apache.org/surefire/maven-surefire-plugin/test-mojo.html
  mvn_opts=(
    -Dtest="$test_class_and_maybe_method"
    --projects "$module_name"
    -DtempDir="$surefire_rel_tmp_dir"
    "${MVN_COMMON_SKIPPED_OPTIONS_IN_TEST[@]}"
  )
  append_common_mvn_opts

  local report_suffix
  if [[ -n $test_method_name ]]; then
    report_suffix=${test_class}__${test_method_name}
  else
    report_suffix=$test_class
  fi
  report_suffix=${report_suffix//[/_}
  report_suffix=${report_suffix//]/_}

  local should_clean_test_dir=false
  local surefire_reports_dir
  if [[ -n ${YB_SUREFIRE_REPORTS_DIR:-} ]]; then
    surefire_reports_dir=$YB_SUREFIRE_REPORTS_DIR
  else
    surefire_reports_dir=$module_dir/target/surefire-reports_${report_suffix}
    if should_run_java_test_methods_separately && [[ -n $test_method_name ]]; then
      # This report directory only has data for one test method (see how we come up with
      # report_suffix above if test method name is defined ), so it is OK to clean it before running
      # the test.
      should_clean_test_dir=true
    fi
  fi
  declare -r surefire_reports_dir
  declare -r should_clean_test_dir

  unset report_suffix

  mvn_opts+=(
    "-Dyb.surefire.reports.directory=$surefire_reports_dir"
    surefire:test
  )

  if is_jenkins; then
    # When running on Jenkins we would like to see more debug output.
    mvn_opts+=( -X )
  fi

  if ! which mvn >/dev/null; then
    fatal "Maven not found on PATH. PATH: $PATH"
  fi

  # Note: "$surefire_reports_dir" contains the test method name as well.
  local junit_xml_path=$surefire_reports_dir/TEST-$test_class.xml
  local test_report_json_path=$surefire_reports_dir/TEST-${test_class}_test_report.json
  local log_files_path_prefix=$surefire_reports_dir/$test_class
  local test_log_path=$log_files_path_prefix-output.txt

  # For the log file prefix, remember the pattern with a trailing "*" -- we will expand it
  # after the test has run.
  register_test_artifact_files \
    "$junit_xml_path" \
    "$test_log_path" \
    "$test_report_json_path" \
    "${log_files_path_prefix}*"
  log "Using surefire reports directory: $surefire_reports_dir"
  log "Test log path: $test_log_path"

  local mvn_output_path=""
  about_to_start_running_test
  if [[ ${YB_REDIRECT_MVN_OUTPUT_TO_FILE:-0} == 1 ]]; then
    mvn_output_path=$surefire_reports_dir/${test_class}__mvn_output.log
  fi

  local attempts_left
  for attempts_left in {1..0}; do
    if "$should_clean_test_dir" && [[ -d $surefire_reports_dir ]]; then
      log "Cleaning the existing contents of: $surefire_reports_dir"
      ( set -x; rm -f "$surefire_reports_dir"/* )
    fi
    if [[ ${YB_REDIRECT_MVN_OUTPUT_TO_FILE:-0} == 1 ]]; then
      mkdir -p "$surefire_reports_dir"
      set +e
      time ( try_set_ulimited_ulimit; set -x; mvn "${mvn_opts[@]}" ) &>"$mvn_output_path"
    else
      set +e
      time ( try_set_ulimited_ulimit; set -x; mvn "${mvn_opts[@]}" )
    fi
    local mvn_exit_code=$?
    set -e
    log "Maven exited with code $mvn_exit_code"
    if [[ $mvn_exit_code -eq 0 || $attempts_left -eq 0 ]]; then
      break
    fi
    if [[ ! -f $test_log_path ]]; then
      log "Warning: test log path not found: $test_log_path, not re-trying the test."
      break
    elif grep -Eq "$TEST_RESTART_PATTERN" "$test_log_path"; then
      log "Found an intermittent error in $test_log_path, retrying the test"
      grep -E "$TEST_RESTART_PATTERN" "$test_log_path" >&2
    else
      break
    fi
  done

  if [[ -f $junit_xml_path ]]; then
    # No reason to include mini cluster logs in the JUnit XML. In fact, these logs can interfere
    # with XML parsing by Jenkins.
    local filtered_junit_xml_path=$junit_xml_path.filtered
    grep -Ev "\
^(m|ts|initdb|postgres)[0-9]*[|]pid[0-9]+[|]|\
^[[:space:]]+at|\
^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2},[0-9]{3} |\
^redis[.]clients[.]jedis[.]exceptions[.]JedisConnectionException: |\
^Caused by: |\
^[[:space:]][.]{3} +[0-9]+ more|\
^java[.]lang[.][a-zA-Z.]+Exception: " "$junit_xml_path" >"$filtered_junit_xml_path"
    # See if we have got a valid XML file after filtering.
    if xmllint "$filtered_junit_xml_path" >/dev/null; then
      mv -f "$filtered_junit_xml_path" "$junit_xml_path"
      log "Filtered JUnit XML file '$junit_xml_path' is valid"
    else
      log "Failed to filter JUnit XML file '$junit_xml_path': invalid after filtering"
      if xmllint "$junit_xml_path"; then
        log "JUnit XML file '$junit_xml_path' is valid with no filtering done"
      else
        log "JUnit XML file '$junit_xml_path' is already invalid even with no filtering!"
      fi
      rm -f "$filtered_junit_xml_path"
    fi
  fi

  process_tree_supervisor_append_log_to_on_error=$test_log_path
  stop_process_tree_supervisor

  if ! "$process_supervisor_success"; then
    log "Process tree supervisor script reported an error, marking the test as failed in" \
        "$junit_xml_path"
    "$YB_SRC_ROOT"/build-support/update_test_result_xml.py \
      --result-xml "$junit_xml_path" \
      --mark-as-failed true \
      --extra-message "Process supervisor script reported errors (e.g. unterminated processes)."
  fi

  if is_jenkins ||
     [[ ${YB_REMOVE_SUCCESSFUL_JAVA_TEST_OUTPUT:-} == "1" && $mvn_exit_code -eq 0 ]]; then
    # If the test is successful, all expected files exist, and no failures are found in the JUnit
    # test XML, delete the test log files to save disk space in the Jenkins archive.
    #
    # We redirect both stdout and stderr for "grep -E" commands to /dev/null because the files we
    # are looking for might not exist. There could be a better way to do this that would not hide
    # real errors.
    if [[ -f $test_log_path && -f $junit_xml_path ]] && \
       ! grep -E "YB Java test failed" \
         "$test_log_path" "$log_files_path_prefix".*.{stdout,stderr}.txt &>/dev/null && \
       ! grep -E "(errors|failures)=\"?[1-9][0-9]*\"?" "$junit_xml_path" &>/dev/null; then
      log "Removing $test_log_path and related per-test-method logs: test succeeded."
      (
        set -x
        rm -f "$test_log_path" \
              "$log_files_path_prefix".*.{stdout,stderr}.txt \
              "$mvn_output_path"
      )
    else
      log "Not removing $test_log_path and related per-test-method logs: some tests failed," \
          "or could not find test output or JUnit XML output."
      log_file_existence "$test_log_path"
      log_file_existence "$junit_xml_path"
    fi
  fi

  if should_gzip_test_logs; then
    gzip_if_exists "$test_log_path" "$mvn_output_path"
    local per_test_method_log_path
    for per_test_method_log_path in "$log_files_path_prefix".*.{stdout,stderr}.txt; do
      gzip_if_exists "$per_test_method_log_path"
    done
  fi

  declare -i java_test_exit_code=$mvn_exit_code
  if [[ $java_test_exit_code -eq 0 ]] && ! "$process_supervisor_success"; then
    java_test_exit_code=1
  fi

  run_postproces_test_result_script \
    --test-log-path "$test_log_path" \
    --junit-xml-path "$junit_xml_path" \
    --language java \
    --class-name "$test_class" \
    --test-name "$test_method_name" \
    --java-module-dir "$module_dir"

  return "$java_test_exit_code"
}

collect_java_tests() {
  set_common_test_paths
  log "Collecting the list of all Java test methods and parameterized test methods"
  unset YB_SUREFIRE_REPORTS_DIR
  local mvn_opts=(
    -DcollectTests
    "${MVN_COMMON_SKIPPED_OPTIONS_IN_TEST[@]}"
  )
  append_common_mvn_opts
  java_test_list_path=$BUILD_ROOT/java_test_list.txt
  local collecting_java_tests_log_prefix=$BUILD_ROOT/collecting_java_tests
  local stdout_log="$collecting_java_tests_log_prefix.out"
  local stderr_log="$collecting_java_tests_log_prefix.err"
  cp /dev/null "$java_test_list_path"
  cp /dev/null "$stdout_log"
  cp /dev/null "$stderr_log"

  local java_project_dir
  for java_project_dir in "${yb_java_project_dirs[@]}"; do
    pushd "$java_project_dir"
    local log_msg="Collecting Java tests in directory $PWD"
    echo "$log_msg" >>"$stdout_log"
    echo "$log_msg" >>"$stderr_log"
    set +e
    (
      # shellcheck disable=SC2030
      export YB_RUN_JAVA_TEST_METHODS_SEPARATELY=1
      set -x
      # The string "YUGABYTE_JAVA_TEST: " is specified in the Java code as COLLECTED_TESTS_PREFIX.
      #
      time mvn "${mvn_opts[@]}" surefire:test \
        2>>"$stderr_log" | \
        tee -a "$stdout_log" | \
        grep -E '^YUGABYTE_JAVA_TEST: ' | \
        sed 's/^YUGABYTE_JAVA_TEST: //g' >>"$java_test_list_path"
    )
    # shellcheck disable=SC2181
    if [[ $? -ne 0 ]]; then
      local log_file_path
      for log_file_path in "$stdout_log" "$stderr_log"; do
        if [[ -f $log_file_path ]]; then
          heading "Contents of $log_file_path (dumping here because of an error)"
          cat "$log_file_path"
        else
          heading "$log_file_path does not exist"
        fi
      done
      fatal "Failed collecting Java tests. See '$stdout_log' and '$stderr_log' contents above."
    fi
    set -e
    popd
  done
  log "Collected the list of Java tests to '$java_test_list_path'"
}

run_all_java_test_methods_separately() {
  # Create a subshell to be able to export environment variables temporarily.
  (
    # shellcheck disable=SC2030,SC2031
    export YB_RUN_JAVA_TEST_METHODS_SEPARATELY=1
    export YB_REDIRECT_MVN_OUTPUT_TO_FILE=1
    declare -i num_successes=0
    declare -i num_failures=0
    declare -i total_tests=0
    collect_java_tests
    declare -i start_time_sec
    start_time_sec=$(date +%s)
    while IFS='' read -r java_test_name; do
      if resolve_and_run_java_test "$java_test_name"; then
        log "Java test succeeded: $java_test_name"
        (( num_successes+=1 ))
      else
        log "Java test failed: $java_test_name"
        # shellcheck disable=SC2034
        global_exit_code=1
        (( num_failures+=1 ))
      fi
      (( total_tests+=1 ))
      declare -i success_pct=$(( num_successes * 100 / total_tests ))
      declare -i current_time_sec
      current_time_sec=$(date +%s)
      declare -i elapsed_time_sec=$(( current_time_sec - start_time_sec ))
      declare -i avg_test_time_sec=$(( elapsed_time_sec / total_tests ))
      heading "Current Java test stats: " \
              "$num_successes successful," \
              "$num_failures failed," \
              "$total_tests total," \
              "success rate: $success_pct%," \
              "elapsed time: $elapsed_time_sec sec," \
              "avg test time: $avg_test_time_sec sec"
    done < <( sort "$java_test_list_path" )
  )
}

run_python_doctest() {
  python_root=$YB_SRC_ROOT/python
  local PYTHONPATH
  export PYTHONPATH=$python_root

  local IFS=$'\n'
  local file_list
  file_list=$( cd "$YB_SRC_ROOT" && git ls-files '*.py' )

  local python_file
  for python_file in $file_list; do
    local basename=${python_file##*/}
    if [[ $python_file == managed/* ||
          $python_file == cloud/* ||
          $python_file == src/postgres/src/test/locale/sort-test.py ||
          $python_file == bin/test_bsopt.py ||
          $python_file == thirdparty/* ]]; then
      continue
    fi
    if [[ $basename == .ycm_extra_conf.py ||
          $basename == split_long_command_line.py ||
          $python_file =~ managed/.* ]]; then
      continue
    fi
    python3 -m doctest "$python_file"
  done
}

run_python_tests() {
  activate_virtualenv
  (
    export PYTHONPATH=$YB_SRC_ROOT/python
    run_python_doctest
  )
  check_python_script_syntax
}

should_run_java_test_methods_separately() {
  # shellcheck disable=SC2031
  [[ ${YB_RUN_JAVA_TEST_METHODS_SEPARATELY:-0} == "1" ]]
}

# Finds the directory (Maven module) the given Java test belongs to and runs it.
#
# Argument: the test to run in the following form:
# com.yugabyte.jedis.TestReadFromFollowers#testSameZoneOps[0]
resolve_and_run_java_test() {
  expect_num_args 1 "$@"
  local java_test_name=$1
  # shellcheck disable=SC2119
  set_common_test_paths
  log "Running Java test $java_test_name"
  local module_dir
  local language
  local java_test_method_name=""
  if [[ $java_test_name == *\#* ]]; then
    java_test_method_name=${java_test_name##*#}
  fi
  local java_test_class=${java_test_name%#*}
  local rel_java_src_path=${java_test_class//./\/}
  if ! is_jenkins; then
    log "Java test class: $java_test_class"
    log "Java test method name and optionally a parameter set index: $java_test_method_name"
  fi

  local module_name=""
  local rel_module_dir=""
  local java_project_dir

  for java_project_dir in "${yb_java_project_dirs[@]}"; do
    for module_dir in "$java_project_dir"/*; do
      if [[ "$module_dir" == */target ]]; then
        continue
      fi
      if [[ -d $module_dir ]]; then
        for language in java scala; do
          candidate_source_path=$module_dir/src/test/$language/$rel_java_src_path.$language
          if [[ -f $candidate_source_path ]]; then
            local current_module_name=${module_dir##*/}
            if [[ -n $module_name ]]; then
              fatal "Could not determine module for Java/Scala test '$java_test_name': both" \
                    "'$module_name' and '$current_module_name' are valid candidates."
            fi
            module_name=$current_module_name
            rel_module_dir=${module_dir##$YB_SRC_ROOT/}
          fi
        done
      fi
    done
  done

  if [[ -z $module_name ]]; then
    # Could not find the test source assuming we are given the complete package. Let's assume we
    # only have the class name.
    module_name=""
    local rel_source_path=""
    local java_project_dir
    for java_project_dir in "${yb_java_project_dirs[@]}"; do
      for module_dir in "$java_project_dir"/*; do
        if [[ "$module_dir" == */target ]]; then
          continue
        fi
        if [[ -d $module_dir ]]; then
          local module_test_src_root="$module_dir/src/test"
          if [[ -d $module_test_src_root ]]; then
            local candidate_files=()
            local line
            while IFS='' read -r line; do
              candidate_files+=( "$line" )
            done < <(
              cd "$module_test_src_root" &&
              find . '(' -name "$java_test_class.java" -or -name "$java_test_class.scala" ')'
            )
            if [[ ${#candidate_files[@]} -gt 0 ]]; then
              local current_module_name=${module_dir##*/}
              if [[ -n $module_name ]]; then
                fatal "Could not determine module for Java/Scala test '$java_test_name': both" \
                      "'$module_name' and '$current_module_name' are valid candidates."
              fi
              module_name=$current_module_name
              rel_module_dir=${module_dir##$YB_SRC_ROOT/}

              if [[ ${#candidate_files[@]} -gt 1 ]]; then
                fatal "Ambiguous source files for Java/Scala test '$java_test_name': " \
                      "${candidate_files[*]}"
              fi

              rel_source_path=${candidate_files[0]}
            fi
          fi
        fi
      done
    done

    if [[ -z $module_name ]]; then
      fatal "Could not find module name for Java/Scala test '$java_test_name'"
    fi

    local java_class_with_package=${rel_source_path%.java}
    java_class_with_package=${java_class_with_package%.scala}
    java_class_with_package=${java_class_with_package#./java/}
    java_class_with_package=${java_class_with_package#./scala/}
    java_class_with_package=${java_class_with_package//\//.}
    if [[ $java_class_with_package != *.$java_test_class ]]; then
      fatal "Internal error: could not find Java package name for test class $java_test_name. " \
            "Found source file: $rel_source_path, and extracted Java class with package from it:" \
            "'$java_class_with_package'. Expected that Java class name with package" \
            "('$java_class_with_package') would end dot and Java test class name" \
            "('.$java_test_class') but that is not the case."
    fi
    java_test_name=$java_class_with_package
    if [[ -n $java_test_method_name ]]; then
      java_test_name+="#$java_test_method_name"
    fi
  fi

  if [[ ${num_test_repetitions:-1} -eq 1 ]]; then
    # This will return an error code appropriate to the test result.
    run_java_test "$rel_module_dir" "$java_test_name"
  else
    # TODO: support enterprise case by passing rel_module_dir here.
    run_repeat_unit_test "$module_name" "$java_test_name" --java
  fi
}

# Allows remembering all the generated test log files in a file whose path is specified by the
# YB_TEST_ARTIFACT_LIST_PATH variable.
# Example patterns for Java test results (relative to $YB_SRC_ROOT, and broken over multiple lines):
#
# java/yb-jedis-tests/target/surefire-reports_redis.clients.jedis.tests.commands.
# AllKindOfValuesCommandsTest__expireAt/TEST-redis.clients.jedis.tests.commands.
# AllKindOfValuesCommandsTest{.xml,-output.txt,_test_report.json}
register_test_artifact_files() {
  if [[ -n ${YB_TEST_ARTIFACT_LIST_PATH:-} ]]; then
    local file_path
    (
      for file_path in "$@"; do
        echo "$file_path"
      done
    ) >>"$YB_TEST_ARTIFACT_LIST_PATH"
  fi
}

run_cmake_unit_tests() {
  local old_dir=$PWD
  cd "$YB_SRC_ROOT"

  ( set -x; cmake -P cmake_modules/YugabyteCMakeUnitTest.cmake )

  local cmake_files=( CMakeLists.txt )
  local IFS=$'\n'
  local src_subdir
  local line
  for src_subdir in src ent/src; do
    ensure_directory_exists "$YB_SRC_ROOT/$src_subdir"
    while IFS='' read -r line; do
      cmake_files+=( "$line" )
    done < <( find "$YB_SRC_ROOT/$src_subdir" -name "CMakeLists*.txt" )
  done
  while IFS='' read -r line; do
    cmake_files+=( "$line" )
  done < <( find "$YB_SRC_ROOT/cmake_modules" -name "*.cmake" )

  local error=false
  for cmake_file in "${cmake_files[@]}"; do
    ensure_file_exists "$cmake_file"
    local disallowed_pattern
    for disallowed_pattern in YB_USE_ASAN YB_USE_UBSAN YB_USE_TSAN; do
      if grep -q "$disallowed_pattern" "$cmake_file"; then
        log "Found disallowed pattern $disallowed_pattern in $cmake_file"
        error=true
      fi
    done
  done
  if "$error"; then
    fatal "Found some disallowed patterns in CMake files"
  else
    log "Validated ${#cmake_files[@]} CMake files using light-weight grep checks"
  fi
  cd "$old_dir"
}

# -------------------------------------------------------------------------------------------------
# Initialization
# -------------------------------------------------------------------------------------------------

# shellcheck disable=SC2034
readonly jenkins_job_and_build=${JOB_NAME:-unknown_job}__${BUILD_NUMBER:-unknown_build}
