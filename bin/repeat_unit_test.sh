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
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

show_usage() {
  cat <<-EOT
Usage:
  For C++ tests:
    ${0##*/} <build_type> <test_binary_name> [<test_filter>] [<options>]
  For Java tests:
    ${0##*/} <maven_module_name> <java_package_class_and_method> --java [<options>]

Runs the given test in a loop locally, collects statistics about successes/failures, and saves
logs.

Options:
  -h, --help
    Show usage.
  -p, --parallelism
    Run this many instances of the test in parallel.
  -n, --num-iter
    Run this many iterations of the test.
  -v <verbosity>
    Verbosity option passed to the test.
  --verbose
    Output extra debug information from this script. Does not affect the test.
  -k, --keep-all-logs
    Keep all logs, not just failing tests' logs.
  --skip-address-already-in-use
    Skip "address already in use" errors that frequently cause false positive test failures.
    The current approach is somewhat simplistic in that we attribute the test failure to the
    "address already in use" issue, even if other issues may be present, but that's OK as long as
    we believe "address already in use" only happens a small percentage of the time.
  --test-args "<arguments>"
    Pass additional arguments to the test.
  --clang
    Use binaries built with Clang (e.g. to distinguish between debug/release builds made with
    gcc vs. clang). If YB_COMPILER_TYPE is set, it will take precedence, and this option will not
    be allowed.
  --skip-log-compression
    Don't compress kept logs.
  --stop-at-failure, --stop-on-failure, --saf, --sof
    Stop running further iterations after the first failure happens.
  --java
    Specify this when running a Java test.
EOT
}

delete_tmp_files() {
  # Be extra careful before we nuke a directory.
  if [[ -n ${TEST_TMPDIR:-} && $TEST_TMPDIR =~ ^/tmp/ ]]; then
    set +e
    rm -rf "$TEST_TMPDIR"
    set -e
  fi
}

set -euo pipefail

# shellcheck source=build-support/common-test-env.sh
. "${0%/*}"/../build-support/common-test-env.sh

unset TEST_TMPDIR

trap delete_tmp_files EXIT

script_name=${BASH_SOURCE##*/}
script_name_no_ext=${script_name%.sh}
skip_address_already_in_use=false

if [[ $# -eq 0 ]]; then
  show_usage >&2
  exit 1
fi
positional_args=()
more_test_args=""
log_dir=""
declare -i default_parallelism=$DEFAULT_REPEATED_TEST_PARALLELISM
declare -i parallelism=0
declare -i iteration=0
declare -i num_iter=1000
keep_all_logs=false
skip_log_compression=false
original_args=( "$@" )
yb_compiler_type_arg=""
verbose=false
stop_at_failure=false
is_java_test=false

while [[ $# -gt 0 ]]; do
  if [[ ${#positional_args[@]} -eq 0 ]] && is_valid_build_type "$1"; then
    build_type=$1
    shift
    continue
  fi
  case ${1//_/-} in
    --build-type)
      build_type=$2
      validate_build_type "$build_type"
      shift
    ;;
    -h|--help)
      show_usage >&2
      exit 1
    ;;
    -v)
      more_test_args+=" -v=$2"
      shift
    ;;
    --verbose)
      verbose=true
    ;;
    -p|--parallelism)
      parallelism=$2
      validate_numeric_arg_range "parallelism" "$parallelism" \
        "$MIN_REPEATED_TEST_PARALLELISM" "$MAX_REPEATED_TEST_PARALLELISM"
      shift
    ;;
    --log-dir)
      # Used internally for parallel execution
      log_dir="$2"
      if [[ ! -d $log_dir ]]; then
        echo "Specified log directory '$log_dir' does not exist" >&2
        exit 1
      fi
      shift
    ;;
    -n|--num-iter)
      num_iter=$2
      shift
    ;;
    --iteration)
      # Used internally for parallel execution
      iteration=$2
      shift
    ;;
    -k|--keep-all-logs)
      keep_all_logs=true
    ;;
    --skip-log-compression)
      skip_log_compression=true
    ;;
    --skip-address-already-in-use)
      skip_address_already_in_use=true
    ;;
    --test-args)
      more_test_args+=" $2"
      shift
    ;;
    --clang)
      if [[ -n ${YB_COMPILER_TYPE:-} && ${YB_COMPILER_TYPE:-} != "clang" ]]; then
        fatal "YB_COMPILER_TYPE is set to '$YB_COMPILER_TYPE'," \
              "but --clang is specified on the command line"
      fi
      yb_compiler_type_arg="clang"
    ;;
    --java)
      is_java_test=true
    ;;
    --stop-at-failure|--stop-on-failure|--saf|--sof)
      stop_at_failure=true
    ;;
    *)
      if [[ $1 == "--" ]]; then
        fatal "'--' is not a valid positional argument, something is wrong."
      fi
      positional_args+=( "$1" )
    ;;
  esac
  shift
done

yb_compiler_type_from_env=${YB_COMPILER_TYPE:-}
if [[ -n $yb_compiler_type_arg ]]; then
  YB_COMPILER_TYPE=$yb_compiler_type_arg
fi

set_cmake_build_type_and_compiler_type

# shellcheck disable=SC2119
set_build_root

set_sanitizer_runtime_options

declare -i -r num_pos_args=${#positional_args[@]}
if "$is_java_test"; then
  if [[ $num_pos_args -ne 2 ]]; then
    fatal "Expected two positional arguments for Java tests, not including build type:" \
          "<maven_module_name> <class_name_with_package>"
  fi
else
  if [[ $num_pos_args -lt 1 || $num_pos_args -gt 2 ]]; then
    show_usage >&2
      fatal "Expected one or two positional arguments for C++ tests, not including build type:" \
            "<test_binary_name> [<test_filter>]"
  fi
fi

test_binary_name=${positional_args[0]}

gtest_filter_arg=""
if [[ $num_pos_args -eq 2 ]]; then
  test_filter=${positional_args[1]}
  gtest_filter_info="gtest_filter is $test_filter (from the command line)"
elif [[ -n ${YB_GTEST_FILTER:-} ]]; then
  test_filter=$YB_GTEST_FILTER
  gtest_filter_info="gtest_filter is $test_filter (from the YB_GTEST_FILTER env var)"
else
  test_filter="all_tests"
  gtest_filter_info="gtest_filter is not set, running all tests in the test program"
fi

# -------------------------------------------------------------------------------------------------
# End of argument parsing
# -------------------------------------------------------------------------------------------------

if [[ $test_filter != "all_tests" ]]; then
  gtest_filter_arg="--gtest_filter=$test_filter"
fi

if ! "$is_java_test"; then
  abs_test_binary_path=$( find_test_binary "$test_binary_name" )
  rel_test_binary=${abs_test_binary_path#$BUILD_ROOT/}
  if [[ $rel_test_binary == "$abs_test_binary_path" ]]; then
    fatal "Expected absolute test binary path ('$abs_test_binary_path') to start with" \
          "BUILD_ROOT ('$BUILD_ROOT')"
  fi
fi

timestamp=$( get_timestamp_for_filenames )
if [[ -z $log_dir ]]; then
  log_dir=$HOME/logs/$script_name_no_ext/$test_binary_name/$test_filter/$timestamp
  mkdir -p "$log_dir"
fi

# We create this file when the first failure happens.
failure_flag_file_path="$log_dir/failure_flag"

if [[ $iteration -gt 0 ]]; then
  if "$stop_at_failure" && [[ -f $failure_flag_file_path ]]; then
    exit
  fi
  # One iteration with a specific "id" ($iteration).
  test_log_path_prefix=$log_dir/$iteration
  test_log_path=$test_log_path_prefix.log
  if "$is_java_test"; then
    export YB_SUREFIRE_REPORTS_DIR=$test_log_path_prefix.reports
  fi
  export YB_FATAL_DETAILS_PATH_PREFIX=$test_log_path_prefix.fatal_failure_details

  set +e
  current_timestamp=$( get_timestamp_for_filenames )
  export TEST_TMPDIR=/tmp/yb_tests__${current_timestamp}__$RANDOM.$RANDOM.$RANDOM
  mkdir -p "$TEST_TMPDIR"
  set_expected_core_dir "$TEST_TMPDIR"
  if ! "$is_java_test"; then
    determine_test_timeout
  fi

  # TODO: deduplicate the setup here against run_one_cxx_test() in common-test-env.sh.
  if "$is_java_test"; then
    test_wrapper_cmd_line=(
      "$YB_BUILD_SUPPORT_DIR"/run-test.sh "${positional_args[@]}"
    )
  else
    # We allow word splitting in YB_EXTRA_GTEST_FLAGS on purpose.
    # shellcheck disable=SC2206
    test_cmd_line=( "$abs_test_binary_path" "$gtest_filter_arg" $more_test_args
                    ${YB_EXTRA_GTEST_FLAGS:-} )
    test_wrapper_cmd_line=(
      "$BUILD_ROOT"/bin/run-with-timeout $(( timeout_sec + 1 )) "${test_cmd_line[@]}"
    )
  fi

  declare -i start_time_sec
  start_time_sec=$( date +%s )
  (
    cd "$TEST_TMPDIR"
    if "$verbose"; then
      log "Iteration $iteration logging to $test_log_path"
    fi
    ulimit -c unlimited
    ( set -x; "${test_wrapper_cmd_line[@]}" ) &>"$test_log_path"
  )
  exit_code=$?
  declare -i end_time_sec
  end_time_sec=$( date +%s )
  declare -i elapsed_time_sec=$(( end_time_sec - start_time_sec ))
  set -e
  comment=""
  keep_log=$keep_all_logs
  pass_or_fail="PASSED"
  if ! did_test_succeed "$exit_code" "$test_log_path"; then
    if ! "$is_java_test"; then
      process_core_file
    fi
    if "$skip_address_already_in_use" && \
       ( grep -Eq '\bAddress already in use\b' "$test_log_path" ||
         grep -Eq '\bWebserver: Could not start on address\b' "$test_log_path" ); then
      # TODO: perhaps we should not skip some types of errors that did_test_succeed finds in the
      # logs (ASAN/TSAN, check failures, etc.), even if we see "address already in use".
      comment=" [assuming \"Address already in use\" is a false positive]"
    else
      pass_or_fail="FAILED"
      keep_log=true
    fi
  fi
  if "$keep_log"; then
    if ! "$skip_log_compression"; then
      if "$is_java_test"; then
        # Compress Java test log.
        mv "$test_log_path" "$YB_SUREFIRE_REPORTS_DIR"
        pushd "$log_dir"
        test_log_path="${YB_SUREFIRE_REPORTS_DIR}_combined_logs.txt"

        while IFS='' read -r file_path; do
          (
            echo
            echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"
            echo "================================================================================"
            echo "Contents of ${file_path##*/} (copied here by $script_name_no_ext)"
            echo "================================================================================"
            echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"
            echo
          ) >>"$test_log_path"
          cat "$file_path" >>"$test_log_path"
        done < <(
          find "$YB_SUREFIRE_REPORTS_DIR" -maxdepth 1 -mindepth 1 | sort
        )

        gzip "$test_log_path"
        test_log_path+=".gz"

        # Ignore errors here -- they could happen e.g. if someone opens one of the uncompressed
        # log files while the test is still running and keeps it open.
        set +e
        rm -rf "$YB_SUREFIRE_REPORTS_DIR"
        set -e
        popd
      else
        # Compress C++ test log.
        if [[ -f $test_log_path ]]; then
          gzip "$test_log_path"
        else
          pass_or_fail="FAILED"
          comment+=" [test log '$test_log_path' not found]"
        fi
        test_log_path+=".gz"
      fi
    fi
    comment+="; test log path: $test_log_path"
  else
    rm -f "$test_log_path"
    if "$is_java_test"; then
      set +e
      rm -rf "$YB_SUREFIRE_REPORTS_DIR"
      set -e
    fi
  fi

  echo "$pass_or_fail: iteration $iteration, $elapsed_time_sec sec$comment"

  if [[ $pass_or_fail == "FAILED" ]]; then
    touch "$failure_flag_file_path"
  fi
else
  # $iteration is 0
  # This is the top-level invocation spawning parallel execution of many iterations.

  if [[ $build_type == "tsan" ]]; then
    default_parallelism=$DEFAULT_REPEATED_TEST_PARALLELISM_TSAN
  else
    default_parallelism=$DEFAULT_REPEATED_TEST_PARALLELISM
  fi

  if [[ $parallelism -eq 0 ]]; then
    parallelism=$default_parallelism
    log "Using test parallelism of $parallelism by default (build type: $build_type)"
  else
    log "Using test parallelism of $parallelism (build type: $build_type)"
  fi

  if [[ -n $yb_compiler_type_from_env ]]; then
    log "YB_COMPILER_TYPE env variable was set to '$yb_compiler_type_from_env' by the caller."
  fi
  log "$gtest_filter_info"
  if [[ -n ${YB_EXTRA_GTEST_FLAGS:-} ]]; then
    log "Extra test flags from YB_EXTRA_GTEST_FLAGS: $YB_EXTRA_GTEST_FLAGS"
  elif "$verbose"; then
    log "YB_EXTRA_GTEST_FLAGS is not set"
  fi
  log "Saving repeated test execution logs to: $log_dir"
  ln -sfn "$log_dir" "$HOME/logs/latest_test"
  seq 1 "$num_iter" | \
    xargs -P $parallelism -n 1 "$0" "${original_args[@]}" --log_dir "$log_dir" --iteration
fi
