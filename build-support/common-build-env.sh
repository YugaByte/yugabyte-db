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

# This is common between build and test scripts.
set -euo pipefail

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  echo "${BASH_SOURCE[0]} must be sourced, not executed" >&2
  exit 1
fi

# Guard against multiple inclusions.
if [[ -n ${YB_COMMON_BUILD_ENV_SOURCED:-} ]]; then
  # Return to the executing script.
  return
fi

readonly YB_COMMON_BUILD_ENV_SOURCED=1

# -------------------------------------------------------------------------------------------------
# Initialize submodules and load yugabyte-bash-common
# -------------------------------------------------------------------------------------------------

set_yb_src_root() {
  export YB_SRC_ROOT=$1
  YB_BUILD_SUPPORT_DIR=$YB_SRC_ROOT/build-support
  if [[ ! -d $YB_SRC_ROOT ]]; then
    fatal "YB_SRC_ROOT directory '$YB_SRC_ROOT' does not exist"
  fi
  YB_COMPILER_WRAPPER_CC=$YB_BUILD_SUPPORT_DIR/compiler-wrappers/cc
  YB_COMPILER_WRAPPER_CXX=$YB_BUILD_SUPPORT_DIR/compiler-wrappers/c++
  yb_java_project_dirs=( "$YB_SRC_ROOT/java" )
}

# This script is expected to be in build-support, a subdirectory of the repository root directory.
set_yb_src_root "$( cd "$( dirname "${BASH_SOURCE[0]}" )"/.. && pwd )"

if [[ $YB_SRC_ROOT == */ ]]; then
  fatal "YB_SRC_ROOT ends with '/' (not allowed): '$YB_SRC_ROOT'"
fi

YB_BASH_COMMON_DIR=$YB_SRC_ROOT/submodules/yugabyte-bash-common

# Initialize submodules. Only do this when the source directory is a git directory.
#
# The "thirdparty" subdirectory of the source directory is a submodule with a special location
# (outside of the "submodules" subdirectory).
if [[ ! -d $YB_BASH_COMMON_DIR     || -z "$( ls -A "$YB_BASH_COMMON_DIR" )" ||
      ! -d $YB_SRC_ROOT/thirdparty || -z "$( ls -A "$YB_SRC_ROOT/thirdparty" )" ]] &&
   [[ -d $YB_SRC_ROOT/.git ]]; then
  ( cd "$YB_SRC_ROOT"; git submodule update --init --recursive )
fi

# shellcheck source=submodules/yugabyte-bash-common/src/yugabyte-bash-common.sh
. "$YB_SRC_ROOT/submodules/yugabyte-bash-common/src/yugabyte-bash-common.sh"

# -------------------------------------------------------------------------------------------------
# Constants
# -------------------------------------------------------------------------------------------------

declare -i MAX_JAVA_BUILD_ATTEMPTS=5

# Reuse the C errno value for this.
declare -r -i YB_EXIT_CODE_NO_SUCH_FILE_OR_DIRECTORY=2

# What matches these expressions will be filtered out of Maven output.
MVN_OUTPUT_FILTER_REGEX='^\[INFO\] (Download(ing|ed)( from [-a-z0-9.]+)?): '
MVN_OUTPUT_FILTER_REGEX+='|^\[INFO\] [^ ]+ already added, skipping$'
MVN_OUTPUT_FILTER_REGEX+='|^\[INFO\] Copying .*[.]jar to .*[.]jar$'
MVN_OUTPUT_FILTER_REGEX+='|^Generating .*[.]html[.][.][.]$'

readonly YB_JENKINS_NFS_HOME_DIR=/n/jenkins

# In our NFS environment, we keep Linuxbrew builds in this directory.
readonly SHARED_LINUXBREW_BUILDS_DIR="$YB_JENKINS_NFS_HOME_DIR/linuxbrew"
# Locally cached copies
readonly YB_BUILD_DIR="/opt/yb-build"
readonly LOCAL_THIRDPARTY_DIRS="$YB_BUILD_DIR/thirdparty"
readonly LOCAL_LINUXBREW_DIRS="$YB_BUILD_DIR/brew"
readonly LOCAL_DOWNLOAD_DIR="${LOCAL_DOWNLOAD_DIR:-$YB_BUILD_DIR/download_cache}"

# The assumed number of cores per build worker. This is used in the default make parallelism level
# calculation in yb_build.sh. This does not have to be the exact number of cores per worker, but
# will affect whether or not we force the auto-scaling group of workers to expand.
readonly YB_NUM_CORES_PER_BUILD_WORKER=8

# The "number of build workers" that we'll end up using to compute the parallelism (by multiplying
# it by YB_NUM_CORES_PER_BUILD_WORKER) will be first brought into this range.
readonly MIN_EFFECTIVE_NUM_BUILD_WORKERS=5
readonly MAX_EFFECTIVE_NUM_BUILD_WORKERS=10

readonly MVN_OUTPUT_FILTER_REGEX

readonly YB_LINUXBREW_LOCAL_ROOT=$HOME/.linuxbrew-yb-build

readonly YB_SHARED_MVN_LOCAL_REPO="$YB_JENKINS_NFS_HOME_DIR/m2_repository"
readonly YB_NON_SHARED_MVN_LOCAL_REPO=$HOME/.m2/repository
readonly YB_SHARED_MVN_SETTINGS="$YB_JENKINS_NFS_HOME_DIR/m2_settings.xml"

if [[ -z ${is_run_test_script:-} ]]; then
  is_run_test_script=false
fi
readonly is_run_test_script

# Setting this to "true" will prevent any changes to the virtualenv (creating it or installing
# modules into it) as part of activate_virtualenv.
yb_readonly_virtualenv=false

YB_NFS_DOWNLOAD_CACHE_DIR=${YB_NFS_DOWNLOAD_CACHE_DIR:-$YB_JENKINS_NFS_HOME_DIR/download_cache}

readonly -a VALID_BUILD_TYPES=(
  asan
  compilecmds
  debug
  fastdebug
  release
  tsan
  tsan_slow
  pvs
)
make_regex_from_list VALID_BUILD_TYPES "${VALID_BUILD_TYPES[@]}"

# Valid values of CMAKE_BUILD_TYPE passed to the top-level CMake build. This is the same as the
# above with the exclusion of ASAN/TSAN.
readonly -a VALID_CMAKE_BUILD_TYPES=(
  debug
  fastdebug
  release
)
make_regex_from_list VALID_CMAKE_BUILD_TYPES "${VALID_CMAKE_BUILD_TYPES[@]}"

readonly -a VALID_COMPILER_TYPES=(
  clang
  gcc
  gcc8
  gcc9
  clang10
  clang11
  zapcc
)
make_regex_from_list VALID_COMPILER_TYPES "${VALID_COMPILER_TYPES[@]}"

readonly -a VALID_LINKING_TYPES=(
  dynamic
  static
)
make_regex_from_list VALID_LINKING_TYPES "${VALID_LINKING_TYPES[@]}"

readonly BUILD_ROOT_BASENAME_RE=\
"^($VALID_BUILD_TYPES_RAW_RE)-\
($VALID_COMPILER_TYPES_RAW_RE)-\
($VALID_LINKING_TYPES_RAW_RE)\
(-ninja)?\
(-clion)?$"

declare -i -r DIRECTORY_EXISTENCE_WAIT_TIMEOUT_SEC=100

declare -i -r YB_DOWNLOAD_LOCK_TIMEOUT_SEC=120

readonly YB_DOWNLOAD_LOCKS_DIR=/tmp/yb_download_locks

readonly YB_NFS_PATH_RE="^/(n|z|u|net|Volumes/net|servers|nfusr)/"

# This is used in yb_build.sh and build-and-test.sh.
# shellcheck disable=SC2034
readonly -a MVN_OPTS_TO_DOWNLOAD_ALL_DEPS=(
  dependency:go-offline
  dependency:resolve
  dependency:resolve-plugins
  -DoutputFile=/dev/null
)

if is_mac; then
  if [[ -x /usr/local/bin/flock ]]; then
    readonly FLOCK="/usr/local/bin/flock"
    readonly FLOCK_MSG="File locked"
  else
    readonly FLOCK="/usr/bin/true"
    readonly FLOCK_MSG="Skipped file lock"
  fi
else
  readonly FLOCK="/usr/bin/flock"
  readonly FLOCK_MSG="File locked"
fi

readonly DELAY_ON_BUILD_WORKERS_LIST_HTTP_ERROR_SEC=0.5
declare -i -r MAX_ATTEMPTS_TO_GET_BUILD_WORKER=10

readonly YB_VIRTUALENV_BASENAME=venv

# -------------------------------------------------------------------------------------------------
# Global variables
# -------------------------------------------------------------------------------------------------

# This is needed so we can ignore thirdparty_path.txt and linuxbrew_path.txt
# in the build directory and not pick up old paths from those files in
# a clean build.
is_clean_build=false

# A human-readable description of how we set the respective variables.
yb_thirdparty_dir_where_from=""
if [[ -n ${YB_THIRDPARTY_DIR:-} ]]; then
  yb_thirdparty_dir_where_from=" (from environment)"
fi

yb_thirdparty_url_where_from=""
if [[ -n ${YB_THIRDPARTY_URL:-} ]]; then
  yb_thirdparty_url_where_from=" (from environment)"
fi

yb_linuxbrew_dir_where_from=""
if [[ -n ${YB_LINUXBREW_DIR:-} ]]; then
  yb_linuxbrew_dir_where_from=" (from environment)"
fi

# To deduplicate Maven arguments
yb_mvn_parameters_already_set=false

# -------------------------------------------------------------------------------------------------
# Functions
# -------------------------------------------------------------------------------------------------

is_thirdparty_build() {
  [[ ${YB_IS_THIRDPARTY_BUILD:-0} == "1" ]]
}

normalize_build_type() {
  if [[ -z ${build_type:-} ]]; then
    if [[ -n ${BUILD_TYPE:-} ]]; then
      build_type=$BUILD_TYPE
    else
      fatal "Neither build_type or BUILD_TYPE are set"
    fi
  fi
  validate_build_type "$build_type"
  local lowercase_build_type
  lowercase_build_type=$( echo "$build_type" | to_lowercase )
  if [[ "$build_type" != "$lowercase_build_type" ]]; then
    # Only assign if we actually need to, because the build_type variable may already be read-only.
    build_type=$lowercase_build_type
  fi
}

# Sets the build directory based on the given build type (the build_type variable) and the value of
# the YB_COMPILER_TYPE environment variable.
set_build_root() {
  if [[ ${1:-} == "--no-readonly" ]]; then
    local -r make_build_root_readonly=false
    shift
  else
    local -r make_build_root_readonly=true
  fi

  expect_num_args 0 "$@"
  normalize_build_type
  readonly build_type

  if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    fatal "YB_COMPILER_TYPE is not set"
  fi
  validate_compiler_type "$YB_COMPILER_TYPE"

  # TODO: remove the "-dynamic" suffix. We only use dynamic linking and it would be very hard to
  # produce any kind of a build that does not involve creation of shared libraries, although we
  # might at some point want to produce a static build of yb-tserver with LTO enabled.
  BUILD_ROOT=$YB_BUILD_PARENT_DIR/$build_type-$YB_COMPILER_TYPE-dynamic

  if using_ninja; then
    BUILD_ROOT+="-ninja"
  fi

  normalize_build_root

  if "$make_build_root_readonly"; then
    readonly BUILD_ROOT
  fi

  if [[ -n ${predefined_build_root:-} &&
        $predefined_build_root != "$BUILD_ROOT" ]] &&
     ! "$YB_BUILD_SUPPORT_DIR/is_same_path.py" "$predefined_build_root" "$BUILD_ROOT"; then
    fatal "An inconsistency between predefined BUILD_ROOT ('$predefined_build_root') and" \
          "computed BUILD_ROOT ('$BUILD_ROOT')."
  fi

  export BUILD_ROOT
  export YB_BUILD_ROOT=$BUILD_ROOT
  decide_whether_to_use_ninja
}

# Resolve the BUILD_ROOT symlink and save the result to the real_build_root_path variable.
set_real_build_root_path() {
  if [[ -h $BUILD_ROOT ]]; then
    real_build_root_path=$( readlink "$BUILD_ROOT" )
  else
    real_build_root_path="$BUILD_ROOT"
  fi

  readonly real_build_root_path=$( cd "$real_build_root_path" && pwd )
}

ensure_build_root_is_set() {
  if [[ -z ${BUILD_ROOT:-} ]]; then
    fatal "The BUILD_ROOT environment variable is not set. This must point to the absolute path" \
          "of the build root directory, e.g. '<yugabyte_src_dir>/build/debug'."
  fi
}

ensure_build_root_exists() {
  ensure_build_root_is_set
  if [[ ! -d $BUILD_ROOT ]]; then
    fatal "The directory BUILD_ROOT ('$BUILD_ROOT') does not exist"
  fi
}

normalize_build_root() {
  ensure_build_root_is_set
  if [[ -d $BUILD_ROOT ]]; then
    BUILD_ROOT=$( cd "$BUILD_ROOT" && pwd )
  fi
}

validate_build_type() {
  expect_num_args 1 "$@"
  # Local variable named _build_type to avoid a collision with the global build_type variable.
  local _build_type=$1
  if ! is_valid_build_type "$_build_type"; then
    fatal "Invalid build type: '$_build_type'. Valid build types are: ${VALID_BUILD_TYPES[*]}" \
          "(case-insensitive)."
  fi
}

is_valid_build_type() {
  expect_num_args 1 "$@"
  local -r _build_type=$( echo "$1" | to_lowercase )
  [[ "$_build_type" =~ $VALID_BUILD_TYPES_RE ]]
}

set_build_type_based_on_jenkins_job_name() {
  if [[ -n "${build_type:-}" ]]; then
    if [[ -n "${JOB_NAME:-}" ]]; then
      # This message only makes sense if JOB_NAME is set.
      log "Build type is already set to '$build_type', not setting it based on Jenkins job name."
    fi
    normalize_build_type
    readonly build_type
    return
  fi

  build_type=debug
  if [[ -z "${JOB_NAME:-}" ]]; then
    log "Using build type '$build_type' by default because JOB_NAME is not set."
    readonly build_type
    return
  fi
  local _build_type  # to avoid collision with the global build_type variable
  local jenkins_job_name
  jenkins_job_name=$( echo "$JOB_NAME" | to_lowercase )
  for _build_type in "${VALID_BUILD_TYPES[@]}"; do
    if [[ "-$jenkins_job_name-" =~ [-_]${_build_type}[-_] ]]; then
      log "Using build type '$_build_type' based on Jenkins job name '$JOB_NAME'."
      readonly build_type=$_build_type
      return
    fi
  done
  readonly build_type
  log "Using build type '$build_type' by default: could not determine from Jenkins job name" \
      "'$JOB_NAME'."
}

set_default_compiler_type() {
  if [[ -z "${YB_COMPILER_TYPE:-}" ]]; then
    if [[ "$OSTYPE" =~ ^darwin ]]; then
      YB_COMPILER_TYPE=clang
    else
      YB_COMPILER_TYPE=gcc
    fi
    export YB_COMPILER_TYPE
    readonly YB_COMPILER_TYPE
  fi
}

is_clang() {
  if [[ $YB_COMPILER_TYPE == "clang" ]]; then
    return 0
  else
    return 1
  fi
}

is_gcc() {
  if [[ $YB_COMPILER_TYPE == "gcc" ]]; then
    return 0
  else
    return 1
  fi
}

is_ubuntu() {
  [[ -f /etc/issue ]] && grep -q Ubuntu /etc/issue
}

build_compiler_if_necessary() {
  # Sometimes we have to build the compiler before we can run CMake.
  if is_clang && is_linux; then
    log "Building clang before we can run CMake with compiler pointing to clang"
    "$YB_THIRDPARTY_DIR/build_thirdparty.sh" llvm
  fi
}

set_compiler_type_based_on_jenkins_job_name() {
  if [[ -n "${YB_COMPILER_TYPE:-}" ]]; then
    if [[ -n "${JOB_NAME:-}" ]]; then
      log "The YB_COMPILER_TYPE variable is already set to '${YB_COMPILER_TYPE}', not setting it" \
          "based on the Jenkins job name."
    fi
  else
    local compiler_type
    local jenkins_job_name
    jenkins_job_name=$( echo "$JOB_NAME" | to_lowercase )
    YB_COMPILER_TYPE=""
    for compiler_type in "${VALID_COMPILER_TYPES[@]}"; do
      if [[ "-$jenkins_job_name-" =~ [-_]${compiler_type}[-_] ]]; then
        log "Setting YB_COMPILER_TYPE='$compiler_type' based on Jenkins job name '$JOB_NAME'."
        YB_COMPILER_TYPE=$compiler_type
        break
      fi
    done
    if [[ -z "$YB_COMPILER_TYPE" ]]; then
      log "Could not determine compiler type from Jenkins job name '$JOB_NAME'," \
          "will use the default."
      return
    fi
  fi
  validate_compiler_type
  readonly YB_COMPILER_TYPE
  export YB_COMPILER_TYPE
}

validate_compiler_type() {
  local compiler_type
  if [[ $# -eq 0 ]]; then
    if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
      fatal "${FUNCNAME[0]} is called with no arguments but YB_COMPILER_TYPE is not set or is empty"
    fi
    compiler_type=$YB_COMPILER_TYPE
  elif [[ $# -eq 1 ]]; then
    compiler_type=$1
  else
    fatal "${FUNCNAME[0]} can only be called with 0 or 1 argument, got $# arguments: $*"
  fi

  if [[ ! $compiler_type =~ $VALID_COMPILER_TYPES_RE ]]; then
    fatal "Invalid compiler type: YB_COMPILER_TYPE='$compiler_type'" \
          "(expected one of: ${VALID_COMPILER_TYPES[*]})."
  fi
}

validate_cmake_build_type() {
  expect_num_args 1 "$@"
  local _cmake_build_type=$1
  _cmake_build_type=$( echo "$_cmake_build_type" | to_lowercase )
  if [[ ! "$_cmake_build_type" =~ $VALID_CMAKE_BUILD_TYPES_RE ]]; then
    fatal "Invalid CMake build type (what we're about to pass to our CMake build as" \
          "_cmake_build_type): '$_cmake_build_type'." \
          "Valid CMake build types are: ${VALID_CMAKE_BUILD_TYPES[*]}."
  fi
}

# This performs two configuration actions:
# - Sets cmake_build_type based on build_type. cmake_build_type is what's being passed to CMake
#   using the CMAKE_BUILD_TYPE variable. CMAKE_BUILD_TYPE can't be "asan" or "tsan".
# - Ensure the YB_COMPILER_TYPE environment variable is set. It is used by our compiler-wrapper.sh
#   script to invoke the appropriate C/C++ compiler.
set_cmake_build_type_and_compiler_type() {
  if [[ -z "${cmake_opts:-}" ]]; then
    cmake_opts=()
  fi

  if [[ -z ${build_type:-} ]]; then
    log "Setting build type to 'debug' by default"
    build_type=debug
  fi

  normalize_build_type
  # We're relying on build_type to set more variables, so make sure it does not change later.
  readonly build_type

  case "$build_type" in
    asan)
      cmake_build_type=fastdebug
    ;;
    compilecmds)
      cmake_build_type=debug
      export YB_EXPORT_COMPILE_COMMANDS=1
    ;;
    pvs)
      cmake_build_type=debug
      export YB_EXPORT_COMPILE_COMMANDS=1
      export YB_DO_NOT_BUILD_TESTS=1
      export YB_SKIP_INITIAL_SYS_CATALOG_SNAPSHOT=1
      export YB_REMOTE_COMPILATION=0
    ;;
    tsan)
      cmake_build_type=fastdebug
    ;;
    tsan_slow)
      cmake_build_type=debug
    ;;
    *)
      cmake_build_type=$build_type
  esac
  validate_cmake_build_type "$cmake_build_type"
  readonly cmake_build_type

  if is_mac; then
    if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
      YB_COMPILER_TYPE=clang
    elif [[ $YB_COMPILER_TYPE != "clang" ]]; then
      fatal "YB_COMPILER_TYPE can only be 'clang' on Mac OS X," \
            "found YB_COMPILER_TYPE=$YB_COMPILER_TYPE."
    fi
  elif [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    if [[ $build_type =~ ^(asan|tsan)$ ]]; then
      # Use Clang by default for ASAN/TSAN builds.
      YB_COMPILER_TYPE=clang
    else
      # The default on Linux.
      YB_COMPILER_TYPE=gcc
    fi
  elif [[ $build_type =~ ^(asan|tsan)$ && $YB_COMPILER_TYPE == "gcc" ]]; then
    fatal "Cannot use ASAN/TSAN with the 'gcc' compiler type. Has to be a specific version of " \
          "GCC such as gcc8 or gcc9."
  fi

  validate_compiler_type
  readonly YB_COMPILER_TYPE
  export YB_COMPILER_TYPE

  # We need to set CMAKE_C_COMPILER and CMAKE_CXX_COMPILER outside of CMake. We used to do that from
  # CMakeLists.txt, and got into an infinite loop where CMake kept saying:
  #
  #   You have changed variables that require your cache to be deleted.
  #   Configure will be re-run and you may have to reset some variables.
  #   The following variables have changed:
  #   CMAKE_CXX_COMPILER= /usr/bin/c++
  #
  # Not sure why it printed the old value there, since we tried to assign it the new value, the
  # same as what's given below.
  #
  # So our new approach is to pass the correct command-line options to CMake, and still let CMake
  # use the default compiler in CLion-triggered builds.

  cmake_opts+=( "-DCMAKE_BUILD_TYPE=$cmake_build_type" )
  cmake_opts+=( "${YB_DEFAULT_CMAKE_OPTS[@]}" )
}

find_make_or_ninja_and_update_cmake_opts() {
  if using_ninja; then
    cmake_opts+=( -G Ninja )
    find_ninja_executable
    make_program=$YB_NINJA_PATH
    # make_file is used in yb_build.sh
    # shellcheck disable=SC2034
    make_file="build.ninja"
  else
    make_program="make"
    # shellcheck disable=SC2034
    make_file="Makefile"
  fi

  cmake_opts+=( "-DCMAKE_MAKE_PROGRAM=$make_program" )
}

create_mvn_repo_path_file() {
  if [[ -n ${YB_MVN_LOCAL_REPO:-} ]]; then
    local mvn_repo_path=$BUILD_ROOT/mvn_repo
    echo "Saving YB_MVN_LOCAL_REPO to $mvn_repo_path"
    echo "$YB_MVN_LOCAL_REPO" > "$mvn_repo_path"
  fi
}

set_mvn_parameters() {
  if "$yb_mvn_parameters_already_set"; then
    return
  fi
  local should_use_shared_dirs=false
  if is_jenkins; then
    if "$is_run_test_script" && [[ -d $BUILD_ROOT/m2_repository ]]; then
      should_use_shared_dirs=false
      YB_MVN_LOCAL_REPO=$BUILD_ROOT/m2_repository
      log "Will use Maven repository from build root ($YB_MVN_LOCAL_REPO)"
    else
      should_use_shared_dirs=true
      if [[ -z ${YB_MVN_LOCAL_REPO:-} ]]; then
        log "Will use shared Maven repository ($YB_SHARED_MVN_LOCAL_REPO)."
      fi
      if [[ -z ${YB_MVN_SETTINGS_PATH:-} ]]; then
        log "Will use shared Maven settings file ($YB_SHARED_MVN_SETTINGS)."
      fi
      log "The above choices are based on:" \
          "is_run_test_script=$is_run_test_script," \
          "OSTYPE=$OSTYPE"
    fi
  fi
  if [[ -z ${YB_MVN_LOCAL_REPO:-} ]]; then
    if "$should_use_shared_dirs"; then
      YB_MVN_LOCAL_REPO=$YB_SHARED_MVN_LOCAL_REPO
    else
      YB_MVN_LOCAL_REPO=$YB_NON_SHARED_MVN_LOCAL_REPO
    fi
  fi
  export YB_MVN_LOCAL_REPO

  if [[ -z ${YB_MVN_SETTINGS_PATH:-} ]]; then
    if "$should_use_shared_dirs"; then
      YB_MVN_SETTINGS_PATH=$YB_SHARED_MVN_SETTINGS
    else
      YB_MVN_SETTINGS_PATH=$HOME/.m2/settings.xml
    fi
  fi
  export MVN_SETTINGS_PATH

  mvn_common_options=(
    "--batch-mode"
    "-DbinDir=$BUILD_ROOT/bin"
    "-Dmaven.repo.local=$YB_MVN_LOCAL_REPO"
    "-Dyb.thirdparty.dir=$YB_THIRDPARTY_DIR"
  )
  log "The result of set_mvn_parameters:" \
      "YB_MVN_LOCAL_REPO=$YB_MVN_LOCAL_REPO," \
      "YB_MVN_SETTINGS_PATH=$YB_MVN_SETTINGS_PATH"
  create_mvn_repo_path_file
  yb_mvn_parameters_already_set=true
}

# Put a retry loop here since it is possible that multiple concurrent builds will try to do this
# at the same time. However, this should converge quickly.
rsync_with_retries() {
  declare -i attempt=1
  declare -i -r max_attempts=5
  while true; do
    if ( set -x; rsync "$@" ); then
      return
    fi
    if [[ $attempt -eq $max_attempts ]]; then
      log "rsync failed after $max_attempts attempts, giving up"
      return 1
    fi
    log "This was rsync attempt $attempt out of $max_attempts. Re-trying after a delay."
    sleep 1
    (( attempt+=1 ))
  done
}

# Appends the settings path specified by $YB_MVN_SETTINGS_PATH (in case that path exists), as well
# as other common Maven options used across all invocations of Maven, to the mvn_opts array. The
# caller of this function usually declares mvn_opts as a local array.
append_common_mvn_opts() {
  mvn_opts+=(
    "${mvn_common_options[@]}"
  )
  if [[ -f $YB_MVN_SETTINGS_PATH ]]; then
    mvn_opts+=(
      --settings "$YB_MVN_SETTINGS_PATH"
    )
  elif [[ $YB_MVN_SETTINGS_PATH != $HOME/.m2/settings.xml ]]; then
    log "Non-default maven user settings file specified by YB_MVN_SETTINGS_PATH does not exist:" \
        "'$YB_MVN_SETTINGS_PATH'"
  fi
}

# A utility function called by both 'build_yb_java_code' and 'build_yb_java_code_with_retries'.
build_yb_java_code_filter_save_output() {
  set_mvn_parameters
  log "Building Java code in $PWD"

  # --batch-mode hides download progress.
  # We are filtering out some patterns from Maven output, e.g.:
  # [INFO] META-INF/NOTICE already added, skipping
  # [INFO] Downloaded: https://repo.maven.apache.org/maven2/org/codehaus/plexus/plexus-classworlds/2.4/plexus-classworlds-2.4.jar (46 KB at 148.2 KB/sec)
  # [INFO] Downloading: https://repo.maven.apache.org/maven2/org/apache/maven/doxia/doxia-logging-api/1.1.2/doxia-logging-api-1.1.2.jar
  local has_local_output=false # default is output path variable is set by calling function
  if [[ -z ${java_build_output_path:-} ]]; then
    local java_build_output_path
    java_build_output_path=/tmp/yb-java-build-$( get_timestamp ).$$.tmp
    has_local_output=true
  fi
  local -a mvn_opts=()
  append_common_mvn_opts
  if ! is_jenkins; then
    mvn_opts+=( -Dmaven.javadoc.skip )
  fi
  set +e -x  # +e: do not fail on grep failure, -x: print the command to stderr.
  if mvn "${mvn_opts[@]}" "$@" 2>&1 | \
      grep -Ev --line-buffered "$MVN_OUTPUT_FILTER_REGEX" | \
      tee "$java_build_output_path"; then
    set +x # stop printing commands
    # We are testing for mvn build failure with grep, since we run mvn with '--fail-never' which
    # always returns success. '--fail-at-end' could have been another possibility, but that mode
    # skips dependent modules so most tests are often not run. Therefore, we resort to grep.
    grep -Eq "BUILD SUCCESS" "$java_build_output_path"
    local mvn_exit_code=$?
    set -e
    if [[ $has_local_output == "true" ]]; then
      rm -f "$java_build_output_path" # cleaning up
    fi
    if [[ $mvn_exit_code -eq 0 ]]; then
      log "Java build SUCCEEDED"
    else
      # Useful for searching in console output.
      log "Java build FAILED: could not find 'BUILD SUCCESS' in Maven output"
    fi
    return $mvn_exit_code
  fi
  set -e +x
  log "Java build or one of its output filters failed"
  if [[ -f $java_build_output_path ]]; then
    log "Java build output (from '$java_build_output_path'):"
    cat "$java_build_output_path"
    log "(End of Java build output)"
    rm -f "$java_build_output_path"
  else
    log "Java build output path file not found at '$java_build_output_path'"
  fi
  return 1
}

build_yb_java_code() {
  local java_build_output_path
  java_build_output_path=/tmp/yb-java-build-$( get_timestamp ).$$.tmp
  build_yb_java_code_filter_save_output "$@"
  local mvn_exit_code=$?
  rm -f "$java_build_output_path"
  return $mvn_exit_code
}

build_yb_java_code_with_retries() {
  local java_build_output_path
  java_build_output_path=/tmp/yb-java-build-$( get_timestamp ).$$.tmp
  declare -i attempt=1

  while [[ $attempt -le $MAX_JAVA_BUILD_ATTEMPTS ]]; do
    if build_yb_java_code_filter_save_output "$@"; then
      rm -f "$java_build_output_path"
      return 0
    fi

    if grep "Could not transfer artifact" "$java_build_output_path" >/dev/null; then
      log "Java build attempt $attempt failed due to temporary connectivity issues, re-trying."
    else
      return 1
    fi

    rm -f "$java_build_output_path"

    (( attempt+=1 ))
  done
  return 1
}

build_yb_java_code_in_all_dirs() {
  local java_project_dir
  for java_project_dir in "${yb_java_project_dirs[@]}"; do
    pushd "$java_project_dir"
    if ! time build_yb_java_code_with_retries "$@"; then
      log "Failed to build Java code in directory '$java_project_dir'" \
          "with these Maven arguments: $*"
      return 1
    fi
    # shellcheck disable=SC2119
    popd
  done
}

# Skip the most part of the normal C++ build output. Still keep the "100%" lines so we can see
# if the build runs to completion. This only filters stdin, so it is expected that stderr is
# redirected to stdout when invoking the C++ build.
filter_boring_cpp_build_output() {
  # For Ninja, keep every 10th successful C/C++ compilation message.
  grep -Ev --line-buffered "\
^(\[ *[0-9]{1,2}%\] +)*(\
Building C(XX)? object |\
Running C[+][+] protocol buffer compiler (with YRPC plugin )?on |\
Linking CXX ((static|shared )?library|executable) |\
Built target \
)|\
Scanning dependencies of target |\
^ssh: connect to host .* port [0-9]+: Connection (timed out|refused)|\
Host .* seems to be down, retrying on a different host|\
Connection to .* closed by remote host.|\
ssh: Could not resolve hostname build-workers-.*: Name or service not known|\
^\[[0-9]+?[1-9]/[0-9]+\] (\
Building CXX object|\
Running C[+][+] protocol buffer compiler|\
Linking CXX shared library|\
Linking CXX executable)"
}

put_path_entry_first() {
  expect_num_args 1 "$@"
  local path_entry=$1
  remove_path_entry "$path_entry"
  export PATH=$path_entry:$PATH
}

add_path_entry() {
  expect_num_args 1 "$@"
  local path_entry=$1
  if [[ $PATH != *:$path_entry && $PATH != $path_entry:* && $PATH != *:$path_entry:* ]]; then
    export PATH+=:$path_entry
  fi
}

# Removes the ccache wrapper directory from PATH so we can find the real path to a compiler, e.g.
# /usr/bin/gcc instead of /usr/lib64/ccache/gcc.  This is expected to run in a subshell so that we
# don't make any unexpected changes to the script's PATH.
# TODO: how to do this properly on Mac OS X?
remove_ccache_dir_from_path() {
  remove_path_entry /usr/lib64/ccache
}

log_diagnostics_about_local_thirdparty() {
  if [[ $YB_THIRDPARTY_DIR == /opt/yb-build/* ]]; then
    log "[Host $(hostname)]" \
        "See diagnostic information below about subdirectories of /opt/yb-build:"
    (
      set -x +e
      ls -l /opt/yb-build/brew >&2
      ls -l /opt/yb-build/thirdparty >&2
    )
  fi
}

# Given a compiler type, e.g. gcc or clang, find the actual compiler executable (not a wrapper
# provided by ccache).  Takes into account YB_GCC_PREFIX and YB_CLANG_PREFIX variables that allow to
# use custom gcc and clang installations. Sets cc_executable and cxx_executable variables. This is
# used in compiler-wrapper.sh.
find_compiler_by_type() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set YB_COMPILER_TYPE
  if [[ -n ${YB_RESOLVED_CC_COMPILER:-} && -n ${YB_RESOLVED_CXX_COMPILER:-} ]]; then
    cc_executable=$YB_RESOLVED_CC_COMPILER
    cxx_executable=$YB_RESOLVED_CXX_COMPILER
    return
  fi

  validate_compiler_type "$YB_COMPILER_TYPE"

  unset cc_executable
  unset cxx_executable
  case "$YB_COMPILER_TYPE" in
    gcc)
      if [[ -n ${YB_GCC_PREFIX:-} ]]; then
        if [[ ! -d $YB_GCC_PREFIX/bin ]]; then
          fatal "Directory YB_GCC_PREFIX/bin ($YB_GCC_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_GCC_PREFIX/bin/gcc
        cxx_executable=$YB_GCC_PREFIX/bin/g++
      elif using_linuxbrew; then
        cc_executable=$YB_LINUXBREW_DIR/bin/gcc
        cxx_executable=$YB_LINUXBREW_DIR/bin/g++
      else
        cc_executable=gcc
        cxx_executable=g++
      fi
      cc_executable+=${YB_GCC_SUFFIX:-}
      cxx_executable+=${YB_GCC_SUFFIX:-}
    ;;
    # TODO mbautin: remove repetition in handling of various versions of GCC.
    # TODO mbautin: do we actually need separate YB_GCC<n>_PREFIX environment variables?
    gcc8)
      if is_centos; then
        cc_executable=/opt/rh/devtoolset-8/root/usr/bin/gcc
        cxx_executable=/opt/rh/devtoolset-8/root/usr/bin/g++
      elif [[ -n ${YB_GCC8_PREFIX:-} ]]; then
        if [[ ! -d $YB_GCC8_PREFIX/bin ]]; then
          fatal "Directory YB_GCC_PREFIX/bin ($YB_GCC_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_GCC8_PREFIX/bin/gcc-8
        cxx_executable=$YB_GCC8_PREFIX/bin/g++-8
      else
        # shellcheck disable=SC2230
        cc_executable=$(which gcc-8)
        # shellcheck disable=SC2230
        cxx_executable=$(which g++-8)
      fi
    ;;
    gcc9)
      if is_centos; then
        cc_executable=/opt/rh/devtoolset-9/root/usr/bin/gcc
        cxx_executable=/opt/rh/devtoolset-9/root/usr/bin/g++
      elif [[ -n ${YB_GCC9_PREFIX:-} ]]; then
        if [[ ! -d $YB_GCC9_PREFIX/bin ]]; then
          fatal "Directory YB_GCC_PREFIX/bin ($YB_GCC_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_GCC9_PREFIX/bin/gcc-9
        cxx_executable=$YB_GCC9_PREFIX/bin/g++-9
      else
        # shellcheck disable=SC2230
        cc_executable=$(which gcc-9)
        # shellcheck disable=SC2230
        cxx_executable=$(which g++-9)
      fi
    ;;
    clang)
      if [[ -n ${YB_CLANG_PREFIX:-} ]]; then
        if [[ ! -d $YB_CLANG_PREFIX/bin ]]; then
          fatal "Directory \$YB_CLANG_PREFIX/bin ($YB_CLANG_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_CLANG_PREFIX/bin/clang
      elif [[ $OSTYPE =~ ^darwin ]]; then
        cc_executable=/usr/bin/clang
      else
        local clang_path
        local clang_found=false
        local clang_paths_to_try=(
          "$YB_THIRDPARTY_DIR/clang-toolchain/bin/clang"
          # clang is present in this location in pre-built third-party archives built before
          # the transition to Linuxbrew (https://phabricator.dev.yugabyte.com/D982). This can be
          # removed when the transition is complete.
          "$YB_THIRDPARTY_DIR/installed/common/bin/clang"
        )
        for clang_path in "${clang_paths_to_try[@]}"; do
          if [[ -f $clang_path ]]; then
            cc_executable=$clang_path
            clang_found=true
            break
          fi
        done
        if ! "$clang_found"; then
          fatal "Failed to find clang at the following locations: ${clang_paths_to_try[*]}"
        fi
      fi
      if [[ -z ${cxx_executable:-} ]]; then
        cxx_executable=$cc_executable++  # clang -> clang++
      fi
      cc_executable+=${YB_CLANG_SUFFIX:-}
      cxx_executable+=${YB_CLANG_SUFFIX:-}
    ;;
    clang10|clang11)
      local clang_prefix_candidate
      local clang_cc_compiler_basename=${YB_COMPILER_TYPE//clang/clang-}
      local clang_cxx_compiler_basename=${YB_COMPILER_TYPE//clang/clang++-}
      for clang_prefix_candidate in /usr/local/bin /usr/bin; do
        if [[ -L $clang_prefix_candidate/$clang_cc_compiler_basename &&
              -L $clang_prefix_candidate/$clang_cxx_compiler_basename ]]; then
          cc_executable=$( readlink "$clang_prefix_candidate/$clang_cc_compiler_basename" )
          cxx_executable=$( readlink "$clang_prefix_candidate/$clang_cxx_compiler_basename" )
          break
        fi
      done
    ;;
    zapcc)
      if [[ -n ${YB_ZAPCC_INSTALL_PATH:-} ]]; then
        cc_executable=$YB_ZAPCC_INSTALL_PATH/bin/zapcc
        cxx_executable=$YB_ZAPCC_INSTALL_PATH/bin/zapcc++
      else
        cc_executable=zapcc
        cxx_executable=zapcc++
      fi
    ;;
    *)
      fatal "Unknown compiler type '$YB_COMPILER_TYPE'"
  esac

  # -----------------------------------------------------------------------------------------------
  # Validate existence of compiler executables.
  # -----------------------------------------------------------------------------------------------

  local compiler_var_name
  for compiler_var_name in cc_executable cxx_executable; do
    if [[ -n ${!compiler_var_name:-} ]]; then
      local compiler_path=${!compiler_var_name}
      if [[ ! -x $compiler_path && $compiler_path =~ ^[a-z+]+$ ]]; then
        # This is a plain "gcc/g++/clang/clang++" compiler command name. Try to find the exact
        # compiler path using the "which" command.
        set +e
        # shellcheck disable=SC2230
        compiler_path=$( remove_ccache_dir_from_path && which "${!compiler_var_name}" )
        # shellcheck disable=SC2181
        if [[ $? -ne 0 ]]; then
          # "which" did not work, revert to the old value.
          compiler_path=${!compiler_var_name}
        fi
        set -e
      fi

      if [[ ! -x $compiler_path ]]; then
        log_diagnostics_about_local_thirdparty
        fatal "[Host $(hostname)]" \
              "Compiler does not exist or is not executable at the path we set" \
              "$compiler_var_name to" \
              "(possibly applying 'which' expansion): $compiler_path" \
              "(trying to use compiler type '$YB_COMPILER_TYPE')."
      fi
      eval $compiler_var_name=\"$compiler_path\"
    fi
  done

  export YB_RESOLVED_CC_COMPILER=$cc_executable
  export YB_RESOLVED_CXX_COMPILER=$cxx_executable
}

# Make pushd and popd quiet.
# http://stackoverflow.com/questions/25288194/dont-display-pushd-popd-stack-accross-several-bash-scripts-quiet-pushd-popd
pushd() {
  local dir_name=$1
  if [[ ! -d $dir_name ]]; then
    fatal "Directory '$dir_name' does not exist"
  fi
  command pushd "$@" > /dev/null
}

# shellcheck disable=SC2120
popd() {
  command popd "$@" > /dev/null
}

# Creates files such as thirdparty_url.txt, thirdparty_path.txt, linuxbrew_path.txt in the build
# directory. This is only being done if the file does not exist.
save_var_to_file_in_build_dir() {
  expect_num_args 2 "$@"
  local value=$1
  if [[ -z ${value:-} ]]; then
    return
  fi

  local file_name=$2
  if [[ -z ${BUILD_ROOT:-} ]]; then
    fatal "BUILD_ROOT is not set"
  fi
  if [[ ! -f $BUILD_ROOT/$file_name ]]; then
    if [[ ! -d $BUILD_ROOT ]]; then
      mkdir -p "$BUILD_ROOT"
    fi
    echo "$value" >"$BUILD_ROOT/$file_name"
  fi
}

# -------------------------------------------------------------------------------------------------
# Downloading third-party dependencies from GitHub releases
# -------------------------------------------------------------------------------------------------

download_and_extract_archive() {
  expect_num_args 2 "$@"
  extracted_dir=""

  local url=$1
  local dest_dir_parent=$2
  local tar_gz_name=${url##*/}
  local install_dir_name=${tar_gz_name%.tar.gz}
  local dest_dir=$dest_dir_parent/$install_dir_name
  if [[ ! -d $dest_dir && ! -L $dest_dir ]]; then
    if [[ ! -d $YB_DOWNLOAD_LOCKS_DIR ]]; then
      ( umask 0; mkdir -p "$YB_DOWNLOAD_LOCKS_DIR" )
    fi
    (
      umask 0
      lock_path=$YB_DOWNLOAD_LOCKS_DIR/$install_dir_name
      (
        "$FLOCK" -w "$YB_DOWNLOAD_LOCK_TIMEOUT_SEC" 200
        if [[ ! -d $dest_dir && ! -L $dest_dir ]]; then
          log "[Host $(hostname)] $FLOCK_MSG $lock_path, proceeding with archive installation."
          (
            set -x
            "$YB_SRC_ROOT/python/yb/download_and_extract_archive.py" \
              --url "$url" \
              --dest-dir-parent "$dest_dir_parent" \
              --local-cache-dir "$LOCAL_DOWNLOAD_DIR"
          )
        else
          log "[Host $(hostname)] $FLOCK_MSG $lock_path but directory $dest_dir already exists."
        fi
      ) 200>"$lock_path"
    )
  fi
  extracted_dir=$dest_dir
}

download_thirdparty() {
  if [[ ! -w $YB_BUILD_DIR ]]; then
    echo >&2 "
  ERROR:  Cannot download pre-built thirdparty dependencies.
          Due to embedded paths, they must be installed under: $YB_BUILD_DIR

          Option 1) To enable downloading: sudo mkdir -m 777 $YB_BUILD_DIR

          Option 2) To build dependencies from source, use build option --ndltp
    "
    fatal "Cannot download pre-built thirdparty dependencies."
  fi
  download_and_extract_archive "$YB_THIRDPARTY_URL" "$LOCAL_THIRDPARTY_DIRS"
  if [[ -n ${YB_THIRDPARTY_DIR:-} &&
        $YB_THIRDPARTY_DIR != "$extracted_dir" ]]; then
    log_thirdparty_and_toolchain_details
    fatal "YB_THIRDPARTY_DIR is already set to '$YB_THIRDPARTY_DIR', cannot set it to" \
          "'$extracted_dir'"
  fi
  export YB_THIRDPARTY_DIR=$extracted_dir
  yb_thirdparty_dir_where_from=" (downloaded from $YB_THIRDPARTY_URL)"
  save_thirdparty_info_to_build_dir

  if ! is_centos; then
    return
  fi

  # Only attempt to download Linuxbrew if the third-party tarball name explicitly mentions it.
  if [[ ${YB_THIRDPARTY_URL##*/} == *linuxbrew* ]]; then
    # Read a linuxbrew_url.txt file in the third-party directory that we downloaded, and follow that
    # link to download and install the appropriate Linuxbrew package.
    local linuxbrew_url_path=$YB_THIRDPARTY_DIR/linuxbrew_url.txt
    if [[ -f $linuxbrew_url_path ]]; then
      local linuxbrew_url
      linuxbrew_url=$(<"$linuxbrew_url_path")
      download_and_extract_archive "$linuxbrew_url" "$LOCAL_LINUXBREW_DIRS"
      if [[ -n ${YB_LINUXBREW_DIR:-} &&
            $YB_LINUXBREW_DIR != "$extracted_dir" ]]; then
        log_thirdparty_and_toolchain_details
        fatal "YB_LINUXBREW_DIR is already set to '$YB_LINUXBREW_DIR', cannot set it to" \
              "'$extracted_dir'"
      fi
      export YB_LINUXBREW_DIR=$extracted_dir
      yb_linuxbrew_dir_where_from=" (downloaded from $linuxbrew_url)"
      save_brew_path_to_build_dir
    else
      fatal "Cannot download Linuxbrew: file $linuxbrew_url_path does not exist"
    fi
  fi
}

# -------------------------------------------------------------------------------------------------
# Detecting Homebrew/Linuxbrew
# -------------------------------------------------------------------------------------------------

disable_linuxbrew() {
  export YB_DISABLE_LINUXBREW=1
  unset YB_LINUXBREW_DIR
}

detect_brew() {
  if [[ ${YB_DISABLE_LINUXBREW:-0} == "1" ]]; then
    disable_linuxbrew
    return
  fi
  if [[ -n ${YB_COMPILER_TYPE:-} &&
        # YB_COMPILER_TYPE could be set to a specific compiler version, like clang10 or gcc8, and
        # in those cases we know we don't use Linuxbrew.
        $YB_COMPILER_TYPE != "gcc" &&
        $YB_COMPILER_TYPE != "clang" ]]; then
    disable_linuxbrew
    return
  fi
  if is_ubuntu; then
    disable_linuxbrew
    return
  fi
  if is_linux; then
    detect_linuxbrew
  fi
}

install_linuxbrew() {
  if ! is_linux; then
    fatal "Expected this function to only be called on Linux"
  fi
  if is_ubuntu; then
    return
  fi
  local version=$1
  local linuxbrew_dirname=linuxbrew-$version
  local linuxbrew_dir=$YB_LINUXBREW_LOCAL_ROOT/$linuxbrew_dirname
  local linuxbrew_archive="${linuxbrew_dir}.tar.gz"
  local linuxbrew_archive_checksum="${linuxbrew_archive}.sha256"
  local url="https://github.com/YugaByte/brew-build/releases/download/$version/\
linuxbrew-$version.tar.gz"
  mkdir -p "$YB_LINUXBREW_LOCAL_ROOT"
  if [[ ! -f $linuxbrew_archive ]]; then
    echo "Downloading Linuxbrew from $url..."
    rm -f "$linuxbrew_archive_checksum"
    curl -L "$url" -o "$linuxbrew_archive"
  fi
  if [[ ! -f $linuxbrew_archive_checksum ]]; then
    echo "Downloading Linuxbrew archive checksum file for $url..."
    curl -L "$url.sha256" -o "$linuxbrew_archive_checksum"
  fi
  echo "Verifying Linuxbrew archive checksum ..."
  pushd "$YB_LINUXBREW_LOCAL_ROOT"
  sha256sum -c --strict "$linuxbrew_archive_checksum"
  popd
  echo "Installing Linuxbrew into $linuxbrew_dir..."
  local tmp=$YB_LINUXBREW_LOCAL_ROOT/tmp/$$_$RANDOM$RANDOM
  mkdir -p "$tmp"
  tar zxf "$linuxbrew_archive" -C "$tmp"
  if mv "$tmp/$linuxbrew_dirname" "$YB_LINUXBREW_LOCAL_ROOT/"; then
    pushd "$linuxbrew_dir"
    ./post_install.sh
    popd
  fi
}

try_set_linuxbrew_dir() {
  local linuxbrew_dir=$1
  if [[ -d "$linuxbrew_dir" &&
        -d "$linuxbrew_dir/bin" &&
        -d "$linuxbrew_dir/lib" &&
        -d "$linuxbrew_dir/include" ]]; then
    YB_LINUXBREW_DIR=$(realpath "$linuxbrew_dir")
    export YB_LINUXBREW_DIR
    save_brew_path_to_build_dir
    return 0
  else
    return 1
  fi
}

wait_for_directory_existence() {
  expect_num_args 1 "$@"
  local dir_path=$1
  declare -i attempt=0
  while [[ ! -d $dir_path ]]; do
    if [[ $attempt -ge $DIRECTORY_EXISTENCE_WAIT_TIMEOUT_SEC ]]; then
      fatal "Gave up waiting for directory '$dir_path' to appear after $attempt seconds"
    fi
    log "Directory '$dir_path' not found, waiting for it to mount"
    (( attempt+=1 ))
    sleep 1
  done
}

save_brew_path_to_build_dir() {
  if is_linux; then
    save_var_to_file_in_build_dir "${YB_LINUXBREW_DIR:-}" "linuxbrew_path.txt"
  fi
}

save_thirdparty_info_to_build_dir() {
  save_var_to_file_in_build_dir "${YB_THIRDPARTY_DIR:-}" "thirdparty_path.txt"
  save_var_to_file_in_build_dir "${YB_THIRDPARTY_URL:-}" "thirdparty_url.txt"
}

save_paths_to_build_dir() {
  save_brew_path_to_build_dir
  save_thirdparty_info_to_build_dir
}

detect_linuxbrew() {
  expect_vars_to_be_set YB_COMPILER_TYPE
  if [[ -z ${BUILD_ROOT:-} ]]; then
    fatal "BUILD_ROOT is not set, not trying to use the default version of Linuxbrew."
  fi
  if ! is_linux; then
    fatal "Expected this function to only be called on Linux"
  fi
  if is_ubuntu; then
    # Not using Linuxbrew on Ubuntu.
    unset YB_LINUXBREW_DIR
    return
  fi
  if [[ $YB_COMPILER_TYPE =~ ^.*[0-9]+$ ]]; then
    # Not allowing to use Linuxbrew if the compiler type mentions a specific compiler version, e.g.
    # gcc9 or clang11.
    unset YB_LINUXBREW_DIR
    return
  fi
  if [[ -n ${YB_LINUXBREW_DIR:-} ]]; then
    export YB_LINUXBREW_DIR
    return
  fi

  if ! "$is_clean_build" && [[ -n ${BUILD_ROOT:-} && -f $BUILD_ROOT/linuxbrew_path.txt ]]; then
    YB_LINUXBREW_DIR=$(<"$BUILD_ROOT/linuxbrew_path.txt")
    export YB_LINUXBREW_DIR
    yb_linuxbrew_dir_where_from=" (from file '$BUILD_ROOT/linuxbrew_path.txt')"
    return
  fi

  local version_file=$YB_SRC_ROOT/build-support/linuxbrew_version.txt
  if [[ ! -f $version_file ]]; then
    fatal "'$version_file' does not exist"
  fi
  local linuxbrew_version
  linuxbrew_version=$( read_file_and_trim "$version_file" )
  local linuxbrew_dirname
  linuxbrew_dirname="linuxbrew-$linuxbrew_version"

  local candidates=()
  local jenkins_linuxbrew_dir="$SHARED_LINUXBREW_BUILDS_DIR/$linuxbrew_dirname"
  if [[ -d $jenkins_linuxbrew_dir ]]; then
    candidates=( "$jenkins_linuxbrew_dir" )
  elif is_jenkins; then
    if is_src_root_on_nfs; then
      wait_for_directory_existence "$jenkins_linuxbrew_dir"
      candidates=( "$jenkins_linuxbrew_dir" )
    else
      yb_fatal_exit_code=$YB_EXIT_CODE_NO_SUCH_FILE_OR_DIRECTORY
      fatal "Warning: Linuxbrew directory referenced by '$version_file' does not" \
            "exist: '$jenkins_linuxbrew_dir', refusing to proceed to prevent " \
            "non-deterministic builds."
    fi
  fi

  if [[ ${#candidates[@]} -gt 0 ]]; then
    local linuxbrew_dir
    for linuxbrew_dir in "${candidates[@]}"; do
      if try_set_linuxbrew_dir "$linuxbrew_dir"; then
        yb_linuxbrew_dir_where_from=" (from '$version_file')"
        return
      fi
    done
  fi

  local linuxbrew_local_dir="$YB_LINUXBREW_LOCAL_ROOT/$linuxbrew_dirname"
  if ! is_jenkins && [[ ! -d $linuxbrew_local_dir ]]; then
    install_linuxbrew "$linuxbrew_version"
  fi
  if try_set_linuxbrew_dir "$linuxbrew_local_dir"; then
    yb_linuxbrew_dir_where_from=" (local installation)"
  else
    if [[ ${#candidates[@]} -gt 0 ]]; then
      log "Could not find Linuxbrew in any of these directories: ${candidates[*]}."
    else
      log "Could not find Linuxbrew candidate directories."
    fi
    log "Failed to install Linuxbrew $linuxbrew_version into $linuxbrew_local_dir."
  fi
}

using_linuxbrew() {
  if is_linux && [[ -n ${YB_LINUXBREW_DIR:-} ]]; then
    return 0  # true in bash
  fi
  return 1
}

decide_whether_to_use_ninja() {
  if [[ -z ${YB_USE_NINJA:-} ]]; then
    # Autodetect whether we need to use Ninja at all, based on whether it is available.
    export YB_USE_NINJA=0
    if [[ -n ${BUILD_ROOT:-} ]]; then
      if [[ $BUILD_ROOT == *-ninja ]]; then
        export YB_USE_NINJA=1
      fi
    elif command -v ninja || [[ -x /usr/local/bin/ninja ]]; then
      export YB_USE_NINJA=1
    elif using_linuxbrew; then
      local yb_ninja_path_candidate=$YB_LINUXBREW_DIR/bin/ninja
      if [[ -x $yb_ninja_path_candidate ]]; then
        export YB_USE_NINJA=1
      fi
    fi
  fi

  find_ninja_executable
}

find_ninja_executable() {
  if ! using_ninja || [[ -n ${YB_NINJA_PATH:-} ]] ||
     [[ ${yb_ninja_executable_not_needed:-} == "true" ]]; then
    return
  fi

  if [[ -x /usr/local/bin/ninja ]]; then
    # This will eventually be the preferred Ninja executable on all platforms in our build
    # environment.
    export YB_NINJA_PATH=/usr/local/bin/ninja
    return
  fi

  set +e
  local which_ninja
  which_ninja=$( command -v ninja )
  set -e
  if [[ -x $which_ninja ]]; then
    export YB_NINJA_PATH=$which_ninja
    return
  fi

  if using_linuxbrew; then
    # On Linux, try to use Linux from Linuxbrew as a last resort.
    local yb_ninja_path_candidate=$YB_LINUXBREW_DIR/bin/ninja
    if [[ -x $yb_ninja_path_candidate ]]; then
      export YB_NINJA_PATH=$yb_ninja_path_candidate
      return
    fi
  fi

  # -----------------------------------------------------------------------------------------------
  # Ninja not found
  # -----------------------------------------------------------------------------------------------

  log "PATH: $PATH"
  if is_linux; then
    log "YB_LINUXBREW_DIR: ${YB_LINUXBREW_DIR:-undefined}"
  fi

  fatal "ninja executable not found"
}

using_ninja() {
  if [[ ${YB_USE_NINJA:-} == "1" ]]; then
    return 0
  else
    return 1
  fi
}

add_brew_bin_to_path() {
  if using_linuxbrew; then
    # We need to add Linuxbrew's bin directory to PATH so that we can find the right compiler and
    # linker.
    put_path_entry_first "$YB_LINUXBREW_DIR/bin"
  fi
}

detect_num_cpus() {
  if [[ ! ${YB_NUM_CPUS:-} =~ ^[0-9]+$ ]]; then
    if is_linux; then
      YB_NUM_CPUS=$(grep -c processor /proc/cpuinfo)
    elif is_mac; then
      YB_NUM_CPUS=$(sysctl -n hw.ncpu)
    else
      fatal "Don't know how to detect the number of CPUs on OS $OSTYPE."
    fi

    if [[ ! $YB_NUM_CPUS =~ ^[0-9]+$ ]]; then
      fatal "Invalid number of CPUs detected: '$YB_NUM_CPUS' (expected a number)."
    fi
  fi
}

# Gets a random build worker host name. Output variable: build_workers (array).
# shellcheck disable=SC2120
get_build_worker_list() {
  expect_num_args 0 "$@"
  if [[ -z ${YB_BUILD_WORKERS_LIST_URL:-} ]]; then
    fatal "YB_BUILD_WORKERS_LIST_URL not set"
  fi

  declare -i attempt
  for (( attempt=1; attempt <= MAX_ATTEMPTS_TO_GET_BUILD_WORKER; attempt++ )); do
    # Note: ignoring bad exit codes and HTTP status codes here. We only look at the output and
    # if each build worker name is of the right format ("build-worker-..."), we consider it valid.
    #
    # Typically, when advice at https://github.com/koalaman/shellcheck/wiki/SC2207 is followed,
    # the code ends up being unnecessarily complex for simple word splitting where all whitespace
    # (new lines, spaces, etc.) has to be treated the same way.
    #
    # shellcheck disable=SC2207
    build_workers=( $( curl -s "$YB_BUILD_WORKERS_LIST_URL" ) )
    if [[ ${#build_workers[@]} -eq 0 ]]; then
      log "Got an empty list of build workers from $YB_BUILD_WORKERS_LIST_URL," \
          "waiting for $DELAY_ON_BUILD_WORKERS_LIST_HTTP_ERROR_SEC sec."
    else
      local build_worker_name
      local all_worker_names_valid=true
      for build_worker_name in "${build_workers[@]}"; do
        if [[ $build_worker_name != build-worker* ]]; then
          log "Got an invalid build worker name from $YB_BUILD_WORKERS_LIST_URL:" \
              "'$build_worker_name', expected all build worker names to start with" \
              "'build-worker', waiting for $DELAY_ON_BUILD_WORKERS_LIST_HTTP_ERROR_SEC sec."
          all_worker_names_valid=false
          break
        fi
      done
      if "$all_worker_names_valid"; then
        return
      fi
    fi
    sleep "$DELAY_ON_BUILD_WORKERS_LIST_HTTP_ERROR_SEC"
  done

  fatal "Could not get a build worker name from $YB_BUILD_WORKERS_LIST_URL in" \
        "$MAX_ATTEMPTS_TO_GET_BUILD_WORKER attempts. Last output from curl: '${build_workers[*]}'"
}

detect_num_cpus_and_set_make_parallelism() {
  detect_num_cpus
  if [[ -z ${YB_MAKE_PARALLELISM:-} ]]; then
    if [[ ${YB_REMOTE_COMPILATION:-} == "1" ]]; then
      declare -i num_build_workers
      declare -a build_workers  # This is set by the get_build_worker_list function.
      get_build_worker_list
      num_build_workers=${#build_workers[@]}
      # Add one to the number of workers so that we cause the auto-scaling group to scale up a bit
      # by stressing the CPU on each worker a bit more.
      declare -i effective_num_build_workers
      effective_num_build_workers=$(( num_build_workers + 1 ))

      # However, make sure this number is within a reasonable range.
      if [[ $effective_num_build_workers -lt $MIN_EFFECTIVE_NUM_BUILD_WORKERS ]]; then
        effective_num_build_workers=$MIN_EFFECTIVE_NUM_BUILD_WORKERS
      fi
      if [[ $effective_num_build_workers -gt $MAX_EFFECTIVE_NUM_BUILD_WORKERS ]]; then
        effective_num_build_workers=$MAX_EFFECTIVE_NUM_BUILD_WORKERS
      fi

      YB_MAKE_PARALLELISM=$(( effective_num_build_workers * YB_NUM_CORES_PER_BUILD_WORKER ))
    else
      YB_MAKE_PARALLELISM=$YB_NUM_CPUS
    fi
  fi
  export YB_MAKE_PARALLELISM
}

validate_thirdparty_dir() {
  ensure_file_exists "$YB_THIRDPARTY_DIR/build_thirdparty.sh"
  ensure_directory_exists "$YB_THIRDPARTY_DIR/installed"
}

# Detect if we're running on Google Compute Platform. We perform this check lazily as there might be
# a bit of a delay resolving the domain name.
detect_gcp() {
  # How to detect if we're running on Google Compute Engine:
  # https://cloud.google.com/compute/docs/instances/managing-instances#dmi
  if [[ -n ${YB_PRETEND_WE_ARE_ON_GCP:-} ]] || \
     curl metadata.google.internal --silent --output /dev/null --connect-timeout 1; then
    readonly is_running_on_gcp_exit_code=0  # "true" exit code
  else
    readonly is_running_on_gcp_exit_code=1  # "false" exit code
  fi
}

is_running_on_gcp() {
  if [[ -z ${is_running_on_gcp_exit_code:-} ]]; then
    detect_gcp
  fi
  return "$is_running_on_gcp_exit_code"
}

is_jenkins_user() {
  [[ $USER == "jenkins" ]]
}

is_jenkins() {
  if [[ -n ${JOB_NAME:-} ]] && is_jenkins_user; then
    return 0  # Yes, we're running on Jenkins.
  fi
  return 1  # Probably running locally.
}

should_gzip_test_logs() {
  is_jenkins || [[ ${YB_GZIP_TEST_LOGS:-0} == "1" ]]
}

# For each file provided as an argument, gzip the given file if it exists and is not already
# compressed.
gzip_if_exists() {
  local f
  for f in "$@"; do
    if [[ -f $f && $f != *.gz && $f != *.bz2 ]]; then
      gzip "$f"
    fi
  done
}

# Check if we're in a Jenkins master build.
is_jenkins_master_build() {
  if [[ -n ${JOB_NAME:-} && $JOB_NAME = *-master-* ]]; then
    return 0
  fi
  return 1
}

# Check if we're in a Jenkins Phabricator build (a pre-commit build).
is_jenkins_phabricator_build() {
  if [[ -z ${JOB_NAME:-} ]]; then
    return 1  # No, not running on Jenkins.
  fi

  if [[ $JOB_NAME == *-phabricator-* || $JOB_NAME == *-phabricator ]]; then
    return 0  # Yes, this is a Phabricator build.
  fi

  return 1  # No, some other kind of Jenkins job.
}

# Check if we're using an NFS partition in YugaByte's build environment.
is_src_root_on_nfs() {
  if [[ $YB_SRC_ROOT =~ $YB_NFS_PATH_RE ]]; then
    return 0
  fi
  return 1
}

using_remote_compilation() {
  if [[ ${YB_REMOTE_COMPILATION:-} == "1" ]]; then
    return 0  # "true" return value
  fi
  return 1  # "false" return value
}

debugging_remote_compilation() {
  [[ ${YB_DEBUG_REMOTE_COMPILATION:-undefined} == "1" ]]
}

cmd_line_to_env_vars_for_remote_cmd() {
  declare -i i=1
  YB_ENCODED_REMOTE_CMD_LINE=""
  # This must match the separator in remote_cmd.sh.
  declare -r ARG_SEPARATOR=$'=:\t:='
  for arg in "$@"; do
    # The separator used here must match the separator used in remote_cmd.sh.
    YB_ENCODED_REMOTE_CMD_LINE+=$arg$ARG_SEPARATOR
  done
  # This variable must be accessible to remote_cmd.sh on the other side of ssh.
  export YB_ENCODED_REMOTE_CMD_LINE
}

run_remote_cmd() {
  local build_host=$1
  local executable=$2
  shift 2
  cmd_line_to_env_vars_for_remote_cmd "$@"
  local ssh_args=(
    "$build_host"
    "$YB_BUILD_SUPPORT_DIR/remote_cmd.sh"
    "$PWD"
    "$PATH"
    "$executable"
  )
  if debugging_remote_compilation; then
    ( set -x; ssh "${ssh_args[@]}" ) 2>&1
  else
    ssh "${ssh_args[@]}"
  fi
}

configure_remote_compilation() {
  if [[ ! ${YB_REMOTE_COMPILATION:-auto} =~ ^(0|1|auto)$ ]]; then
    fatal "Invalid value of the YB_REMOTE_COMPILATION environment variable: can be '0', '1', or" \
          "'auto'. Actual value: ${YB_REMOTE_COMPILATION:-undefined}."
  fi
  # Automatically set YB_REMOTE_COMPILATION in an NFS GCP environment.
  if [[ ${YB_REMOTE_COMPILATION:-auto} == "auto" ]]; then
    if is_running_on_gcp && is_src_root_on_nfs; then
      log "Automatically enabling remote compilation (running in an NFS GCP environment). " \
          "Use YB_REMOTE_COMPILATION=0 (or the --no-remote ybd option) to disable this behavior."
      YB_REMOTE_COMPILATION=1
    else
      YB_REMOTE_COMPILATION=0
      if is_jenkins; then
        # Make it easier to diagnose why we're not using the distributed build. Only enable this on
        # Jenkins to avoid confusing output during development.
        log "Not using remote compilation: " \
            "YB_REMOTE_COMPILATION=${YB_REMOTE_COMPILATION:-undefined}. " \
            "See additional diagnostics below."
        if is_running_on_gcp; then
          log "Running on GCP."
        else
          log "This is not GCP."
        fi
        if is_src_root_on_nfs; then
          log "YB_SRC_ROOT ($YB_SRC_ROOT) appears to be on NFS in YugaByte's distributed" \
              "build setup."
        fi
      fi
    fi
  fi
  export YB_REMOTE_COMPILATION
}

read_file_and_trim() {
  expect_num_args 1 "$@"
  local file_name=$1
  if [[ -f $file_name ]]; then
    sed 's/^[[:space:]]*//; s/[[:space:]]*$//' "$file_name"
  else
    log "File '$file_name' does not exist"
    return 1
  fi
}

# -------------------------------------------------------------------------------------------------
# Finding the third-party directory
# -------------------------------------------------------------------------------------------------

using_default_thirdparty_dir() {
  if [[ -n ${YB_THIRDPARTY_DIR:-} &&
        $YB_THIRDPARTY_DIR != "$YB_SRC_ROOT/thirdparty" ]]; then
    # YB_THIRDPARTY_DIR is specified and is not the default location
    return 1
  fi
  return 0
}

find_or_download_thirdparty() {
  if [[ ${YB_IS_THIRDPARTY_BUILD:-} == "1" ]]; then
    return
  fi
  if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    fatal "YB_COMPILER_TYPE is not set"
  fi
  if ! "$is_clean_build"; then
    if [[ -f $BUILD_ROOT/thirdparty_url.txt ]]; then
      local thirdparty_url_from_file
      thirdparty_url_from_file=$(<"$BUILD_ROOT/thirdparty_url.txt")
      if [[ -n ${YB_THIRDPARTY_URL:-} &&
            "$YB_THIRDPARTY_URL" != "$thirdparty_url_from_file" ]]; then
        fatal "YB_THIRDPARTY_URL is explicitly set to '$YB_THIRDPARTY_URL' but file" \
              "'$BUILD_ROOT/thirdparty_url.txt' contains '$thirdparty_url_from_file'"
      fi
      export YB_THIRDPARTY_URL=$thirdparty_url_from_file
      yb_thirdparty_url_where_from=" (from file '$BUILD_ROOT/thirdparty_url.txt')"
      if [[ ${YB_DOWNLOAD_THIRDPARTY:-} == "0" ]]; then
        fatal "YB_DOWNLOAD_THIRDPARTY is explicitly set to 0 but file" \
              "$BUILD_ROOT/thirdparty_url.txt exists"
      fi
      export YB_DOWNLOAD_THIRDPARTY=1
    fi

    if [[ -f $BUILD_ROOT/thirdparty_path.txt ]]; then
      local thirdparty_dir_from_file
      thirdparty_dir_from_file=$(<"$BUILD_ROOT/thirdparty_path.txt")
      if [[ -n ${YB_THIRDPARTY_DIR:-} &&
            "$YB_THIRDPARTY_DIR" != "$thirdparty_dir_from_file" ]]; then
        fatal "YB_THIRDPARTY_DIR is explicitly set to '$YB_THIRDPARTY_DIR' but file" \
              "'$BUILD_ROOT/thirdparty_path.txt' contains '$thirdparty_dir_from_file'"
      fi
      export YB_THIRDPARTY_DIR=$thirdparty_dir_from_file
      yb_thirdparty_dir_where_from=" (from file '$BUILD_ROOT/thirdparty_path.txt')"
    fi
  fi

  if [[ -n ${YB_THIRDPARTY_DIR:-} && -d $YB_THIRDPARTY_DIR ]]; then
    export YB_THIRDPARTY_DIR
    if ! using_default_thirdparty_dir; then
      export NO_REBUILD_THIRDPARTY=1
    fi
    return
  fi

  # Even if YB_THIRDPARTY_DIR is set but it does not exist, it is possible that we need to download
  # the third-party archive.

  set_prebuilt_thirdparty_url
  if [[ ${YB_DOWNLOAD_THIRDPARTY:-} == "1" ]]; then
    download_thirdparty
    export NO_REBUILD_THIRDPARTY=1
    log "Using downloaded third-party directory: $YB_THIRDPARTY_DIR"
    if using_linuxbrew; then
      log "Using Linuxbrew directory: $YB_LINUXBREW_DIR"
    fi
  fi

  if [[ -z ${YB_THIRDPARTY_DIR:-} ]]; then
    export YB_THIRDPARTY_DIR=$YB_SRC_ROOT/thirdparty
    yb_thirdparty_dir_where_from=" (default)"
  fi
  save_thirdparty_info_to_build_dir
}

log_thirdparty_and_toolchain_details() {
  (
    echo "Details of third-party dependencies:"
    echo "    YB_THIRDPARTY_DIR: ${YB_THIRDPARTY_DIR:-undefined}$yb_thirdparty_dir_where_from"
    if is_linux && [[ -n ${YB_LINUXBREW_DIR:-} ]]; then
      echo "    YB_LINUXBREW_DIR: $YB_LINUXBREW_DIR$yb_linuxbrew_dir_where_from"
    fi
    if [[ -n ${YB_THIRDPARTY_URL:-} ]]; then
      echo "    YB_THIRDPARTY_URL: $YB_THIRDPARTY_URL$yb_thirdparty_url_where_from"
    fi
    if [[ -n ${YB_DOWNLOAD_THIRDPARTY:-} ]]; then
      echo "    YB_DOWNLOAD_THIRDPARTY: $YB_DOWNLOAD_THIRDPARTY"
    fi
    if [[ -n ${NO_REBUILD_THIRDPARTY:-} ]]; then
      echo "    NO_REBUILD_THIRDPARTY: ${NO_REBUILD_THIRDPARTY}"
    fi
  ) >&2
}

handle_predefined_build_root_quietly=false

# shellcheck disable=SC2120
handle_predefined_build_root() {
  expect_num_args 0 "$@"
  if [[ -z ${predefined_build_root:-} ]]; then
    return
  fi

  if [[ -L $predefined_build_root ]]; then
    predefined_build_root=$( readlink "$predefined_build_root" )
  fi

  if [[ -d $predefined_build_root ]]; then
    predefined_build_root=$( cd "$predefined_build_root" && pwd )
  fi

  if [[ $predefined_build_root != $YB_BUILD_INTERNAL_PARENT_DIR/* && \
        $predefined_build_root != $YB_BUILD_EXTERNAL_PARENT_DIR/* ]]; then
    # Sometimes $predefined_build_root contains symlinks on its path.
    "$YB_SRC_ROOT/build-support/validate_build_root.py" \
      "$predefined_build_root" \
      "$YB_BUILD_INTERNAL_PARENT_DIR" \
      "$YB_BUILD_EXTERNAL_PARENT_DIR"
  fi

  local basename=${predefined_build_root##*/}

  if [[ $basename =~ $BUILD_ROOT_BASENAME_RE ]]; then
    local _build_type=${BASH_REMATCH[1]}
    local _compiler_type=${BASH_REMATCH[2]}
    # _linking_type is unused. We always use dynamic linking. TODO: get rid of this parameter.
    # shellcheck disable=SC2034
    local _linking_type=${BASH_REMATCH[3]}
    local _dash_ninja=${BASH_REMATCH[4]}
  else
    fatal "Could not parse build root directory name '$basename'" \
          "(full path: '$predefined_build_root'). Expected to match '$BUILD_ROOT_BASENAME_RE'."
  fi

  if [[ -z ${build_type:-} ]]; then
    build_type=$_build_type
    if ! "$handle_predefined_build_root_quietly"; then
      log "Setting build type to '$build_type' based on predefined build root ('$basename')"
    fi
    validate_build_type "$build_type"
  elif [[ $build_type != "$_build_type" ]]; then
    fatal "Build type from the build root ('$_build_type' from '$predefined_build_root') does " \
          "not match current build type ('$build_type')."
  fi

  if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    export YB_COMPILER_TYPE=$_compiler_type
    if ! "$handle_predefined_build_root_quietly"; then
      log "Automatically setting compiler type to '$YB_COMPILER_TYPE' based on predefined build" \
          "root ('$basename')"
    fi
  elif [[ $YB_COMPILER_TYPE != "$_compiler_type" ]]; then
    fatal "Compiler type from the build root ('$_compiler_type' from '$predefined_build_root') " \
          "does not match YB_COMPILER_TYPE ('$YB_COMPILER_TYPE')."
  fi

  export YB_USE_NINJA=${YB_USE_NINJA:-}
  if [[ $_dash_ninja == "-ninja" && -z ${YB_USE_NINJA:-} ]]; then
    if ! "$handle_predefined_build_root_quietly"; then
      log "Setting YB_USE_NINJA to 1 based on predefined build root ('$basename')"
    fi
    export YB_USE_NINJA=1
  elif [[ $_dash_ninja == "-ninja" && $YB_USE_NINJA != "1" || \
          $_dash_ninja != "-ninja" && $YB_USE_NINJA == "1" ]]; then
    fatal "The use of ninja from build root ('$predefined_build_root') does not match that" \
          "of the YB_USE_NINJA env var ('$YB_USE_NINJA')"
  fi

  decide_whether_to_use_ninja
}

# Remove the build/latest symlink to prevent Jenkins from showing every test twice in test results.
# We call this from a few different places just in case.
remove_latest_symlink() {
  local latest_build_link=$YB_BUILD_PARENT_DIR/latest
  if [[ -h $latest_build_link ]]; then
    log "Removing the latest symlink at '$latest_build_link'"
    ( set -x; unlink "$latest_build_link" )
  fi
}

# Assigns a random "test invocation id" that allows to kill stuck processes corresponding to this
# instance of a particular test or the whole test suite.
set_test_invocation_id() {
  local timestamp
  timestamp=$( get_timestamp_for_filenames )
  export YB_TEST_INVOCATION_ID=test_invocation_${timestamp}_${RANDOM}_${RANDOM}_$$
}

# Kills any processes that have YB_TEST_INVOCATION_ID in their command line. Sets
# killed_stuck_processes=true in case that happens.
kill_stuck_processes() {
  expect_num_args 0 "$@"
  killed_stuck_processes=false
  if [[ -z ${YB_TEST_INVOCATION_ID:-} ]]; then
    return
  fi
  local pid
  for pid in $( pgrep -f "$YB_TEST_INVOCATION_ID" ); do
    log "Found pid $pid from this test suite (YB_TEST_INVOCATION_ID=$YB_TEST_INVOCATION_ID)," \
        "killing it with SIGKILL."
    ps -p "$pid" -f
    if kill -9 "$pid"; then
      # killed_stuck_processes is used in run-test.sh
      # shellcheck disable=SC2034
      killed_stuck_processes=true
      log "Killed process $pid with SIGKILL."
    fi
  done
}

handle_build_root_from_current_dir() {
  predefined_build_root=""
  if [[ ${YB_IS_THIRDPARTY_BUILD:-} == "1" ]]; then
    return
  fi
  local handle_predefined_build_root_quietly=true
  local d=$PWD
  while [[ $d != "/" && $d != "" ]]; do
    basename=${d##*/}
    if [[ ${YB_DEBUG_BUILD_ROOT_BASENAME_VALIDATION:-0} == "1" ]]; then
      log "Trying to match basename $basename to regex: $BUILD_ROOT_BASENAME_RE"
    fi
    if [[ $basename =~ $BUILD_ROOT_BASENAME_RE ]]; then
      predefined_build_root=$d
      handle_predefined_build_root
      return
    fi
    d=${d%/*}
  done

  fatal "Working directory of the compiler '$PWD' is not within a valid Yugabyte build root: " \
        "'$BUILD_ROOT_BASENAME_RE'"
}

validate_numeric_arg_range() {
  expect_num_args 4 "$@"
  local arg_name=$1
  local arg_value=$2
  local -r -i min_value=$3
  local -r -i max_value=$4
  if [[ ! $arg_value =~ ^[0-9]+$ ]]; then
    fatal "Invalid numeric argument value for --$arg_name: '$arg_value'"
  fi
  if [[ $arg_value -lt $min_value || $arg_value -gt $max_value ]]; then
    fatal "Value out of range for --$arg_name: $arg_value, must be between $min_value and" \
          "$max_value."
  fi
}

# -------------------------------------------------------------------------------------------------
# Python support
# -------------------------------------------------------------------------------------------------

# Checks syntax of all Python scripts in the repository.
check_python_script_syntax() {
  if [[ -n ${YB_VERBOSE:-} ]]; then
    log "Checking syntax of Python scripts"
  fi
  pushd "$YB_SRC_ROOT"
  local IFS=$'\n'
  git ls-files '*.py' | xargs -P 8 -n 1 "$YB_BUILD_SUPPORT_DIR/check_python_syntax.py"
  popd
}

activate_virtualenv() {
  local virtualenv_parent_dir=$YB_BUILD_PARENT_DIR
  local virtualenv_dir=$virtualenv_parent_dir/$YB_VIRTUALENV_BASENAME

  if [[ ${YB_RECREATE_VIRTUALENV:-} == "1" && -d $virtualenv_dir ]] && \
     ! "$yb_readonly_virtualenv"; then
    log "YB_RECREATE_VIRTUALENV is set, deleting virtualenv at '$virtualenv_dir'"
    rm -rf "$virtualenv_dir"
    unset YB_RECREATE_VIRTUALENV
  fi

  if [[ ! -d $virtualenv_dir ]]; then
    if "$yb_readonly_virtualenv"; then
      fatal "virtualenv does not exist at '$virtualenv_dir', and we are not allowed to create it"
    fi
    if [[ -n ${VIRTUAL_ENV:-} && -f $VIRTUAL_ENV/bin/activate ]]; then
      local old_virtual_env=$VIRTUAL_ENV
      # Re-activate and deactivate the other virtualenv we're in. Otherwise the deactivate
      # function might not even be present in our current shell. This is necessary because otherwise
      # the --user installation below will fail.
      set +eu
      # shellcheck disable=SC1090
      . "$VIRTUAL_ENV/bin/activate"
      deactivate
      set -eu
      # Not clear why deactivate does not do this.
      remove_path_entry "$old_virtual_env/bin"
    fi
    # We need to be using system python to install the virtualenv module or create a new virtualenv.
    (
      set -x
      mkdir -p "$virtualenv_parent_dir"
      cd "$virtualenv_parent_dir"
      python3 -m venv "$YB_VIRTUALENV_BASENAME"
    )
  fi

  set +u
  # shellcheck disable=SC1090
  . "$virtualenv_dir"/bin/activate
  set -u
  local pip_no_cache=""
  if [[ -n ${YB_PIP_NO_CACHE:-} ]]; then
    pip_no_cache="--no-cache-dir"
  fi

  local pip_executable=pip3
  if ! "$yb_readonly_virtualenv"; then
    local requirements_file_path="$YB_SRC_ROOT/requirements_frozen.txt"
    local installed_requirements_file_path=$virtualenv_dir/${requirements_file_path##*/}
    if ! cmp --silent "$requirements_file_path" "$installed_requirements_file_path"; then
      run_with_retries 10 0.5 "$pip_executable" install -r "$requirements_file_path" \
        $pip_no_cache
    fi
    # To avoid re-running pip install, save the requirements that we've installed in the virtualenv.
    cp "$requirements_file_path" "$installed_requirements_file_path"
  fi

  if [[ ${YB_DEBUG_VIRTUALENV:-0} == "1" ]]; then
    echo >&2 "
VIRTUALENV DEBUGGING
--------------------

    Activated virtualenv in: $virtualenv_dir
    Executable: $0
    PATH: $PATH
    PYTHONPATH: ${PYTHONPATH:-undefined}
    VIRTUAL_ENV: ${VIRTUAL_ENV:-undefined}

"
  fi

  export VIRTUAL_ENV
}

log_file_existence() {
  expect_num_args 1 "$@"
  local file_name=$1
  if [[ -L $file_name && -f $file_name ]]; then
    log "Symlink exists and points to a file: $file_name"
  elif [[ -L $file_name && -d $file_name ]]; then
    log "Symlink exists and points to a directory: $file_name"
  elif [[ -L $file_name ]]; then
    log "Symlink exists but it might be broken: $file_name"
  elif [[ -f $file_name ]]; then
    log "File exists: $file_name"
  elif [[ -d $file_name ]]; then
    log "Directory exists: $file_name"
  elif [[ ! -e $file_name ]]; then
    log "File does not exist: $file_name"
  else
    log "File exists but we could not determine its type: $file_name"
  fi
}

is_valid_git_sha1() {
  [[ $1 =~ ^[0-9a-f]{40} ]]
}

# Returns current git SHA1 in the variable current_git_sha1.
get_current_git_sha1() {
  current_git_sha1=$( git rev-parse HEAD )
  if ! is_valid_git_sha1 "$current_git_sha1"; then
    fatal "Could not get current git SHA1 in $PWD, got: $current_git_sha1"
  fi
}

# sed -i works differently on Linux vs macOS.
sed_i() {
  if is_mac; then
    sed -i "" "$@"
  else
    sed -i "$@"
  fi
}

lint_java_code() {
  local java_project_dir
  declare -i num_errors=0

  for java_project_dir in "${yb_java_project_dirs[@]}"; do
    local IFS=$'\n'
    local java_test_files
    # The proposed syntax in SC2207 is too confusing.
    # shellcheck disable=SC2207
    java_test_files=( $(
      find "$java_project_dir" \( -name "Test*.java" -or -name "*Test.java" \) -and \
          -not -name "TestUtils.java" -and \
          -not -name "*Base.java" -and \
          -not -name "Base*Test.java"
    ) )
    local java_test_file
    for java_test_file in "${java_test_files[@]}"; do
      local log_prefix="YB JAVA LINT: $java_test_file"
      if ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBParameterizedTestRunner\.class\)' \
             "$java_test_file" &&
         ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBTestRunner\.class\)' \
             "$java_test_file" &&
         ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBTestRunnerNonTsanOnly\.class\)' \
             "$java_test_file" &&
         ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBTestRunnerNonTsanAsan\.class\)' \
             "$java_test_file" &&
         ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBTestRunnerNonSanitizersOrMac\.class\)' \
             "$java_test_file"
      then
        log "$log_prefix: neither YBTestRunner, YBParameterizedTestRunner, " \
            "YBTestRunnerNonTsanOnly, YBTestRunnerNonTsanAsan " \
            "nor YBTestRunnerNonSanitizersOrMac are being used in test"
        num_errors+=1
      fi
      if grep -Fq 'import static org.junit.Assert' "$java_test_file" ||
         grep -Fq 'import org.junit.Assert' "$java_test_file"; then
        log "$log_prefix: directly importing org.junit.Assert. Should use org.yb.AssertionWrappers."
        num_errors+=1
      fi
    done
  done
  if [[ $num_errors -eq 0 ]]; then
    log "Light-weight lint of YB Java code: SUCCESS"
  else
    log "Light-weight lint of YB Java code: FAILURE ($num_errors errors found)"
    return 1
  fi
}

run_with_retries() {
  if [[ $# -lt 2 ]]; then
    fatal "run_with_retries requires at least three arguments: max_attempts, delay_sec, and " \
          "the command to run (at least one additional argument)."
  fi
  declare -i -r max_attempts=$1
  declare -r delay_sec=$2
  shift 2

  declare -i attempt_index=1
  while [[ $attempt_index -le $max_attempts ]]; do
    set +e
    "$@"
    declare exit_code=$?
    set -e
    if [[ $exit_code -eq 0 ]]; then
      return
    fi
    log "Warning: command failed with exit code $exit_code at attempt $attempt_index: $*." \
        "Waiting for $delay_sec sec, will then re-try for up to $max_attempts attempts."
    (( attempt_index+=1 ))
    sleep "$delay_sec"
  done
  fatal "Failed to execute command after $max_attempts attempts: $*"
}

debug_log_boolean_function_result() {
  expect_num_args 1 "$@"
  local fn_name=$1
  if "$fn_name"; then
    log "$fn_name is true"
  else
    log "$fn_name is false"
  fi
}

set_java_home() {
  if ! is_mac; then
    return
  fi
  # macOS has a peculiar way of setting JAVA_HOME
  local cmd_to_get_java_home="/usr/libexec/java_home --version 1.8"
  local new_java_home
  new_java_home=$( $cmd_to_get_java_home )
  if [[ ! -d $new_java_home ]]; then
    fatal "Directory returned by '$cmd_to_get_java_home' does not exist: $new_java_home"
  fi
  if [[ -n ${JAVA_HOME:-} && $JAVA_HOME != "$new_java_home" ]]; then
    log "Warning: updating JAVA_HOME from $JAVA_HOME to $new_java_home"
  else
    log "Setting JAVA_HOME: $new_java_home"
  fi
  export JAVA_HOME=$new_java_home
  put_path_entry_first "$JAVA_HOME/bin"
}

update_submodules() {
  if [[ -d $YB_SRC_ROOT/.git ]]; then
    # This does NOT create any new commits in the top-level repository (the "superproject").
    #
    # From documentation on "update" from https://git-scm.com/docs/git-submodule:
    # Update the registered submodules to match what the superproject expects by cloning missing
    # submodules and updating the working tree of the submodules
    ( cd "$YB_SRC_ROOT"; git submodule update --init --recursive )
  fi
}

set_prebuilt_thirdparty_url() {
  expect_vars_to_be_set YB_COMPILER_TYPE
  if [[ ${YB_DOWNLOAD_THIRDPARTY:-} == "1" ]]; then
    local auto_thirdparty_url=""
    local thirdparty_url_file=$YB_BUILD_SUPPORT_DIR/thirdparty_url_${short_os_name}
    if [[ ${YB_COMPILER_TYPE} =~ ^.*[0-9]+$ ]]; then
      # For compiler types like gcc9 or clang11, append the compiler type to the file path.
      thirdparty_url_file+="_${YB_COMPILER_TYPE}"
    fi
    thirdparty_url_file+=.txt
    if [[ -f $thirdparty_url_file ]]; then
      auto_thirdparty_url=$( read_file_and_trim "$thirdparty_url_file" )
      if [[ $auto_thirdparty_url != http://* && $auto_thirdparty_url != https://* ]]; then
        fatal "Invalid third-party URL: '$auto_thirdparty_url' (expected http:// or https://)." \
              "From file: $thirdparty_url_file."
      fi
    elif [[ -z ${YB_THIRDPARTY_URL:-} ]]; then
      log "$thirdparty_url_file file not found, and YB_THIRDPARTY_URL not set."
      log "Cannot download pre-built third-party dependencies. Continuing without downloading."
      export YB_THIRDPARTY_URL=""
      export YB_DOWNLOAD_THIRDPARTY=0
      return
    fi

    if [[ -z ${YB_THIRDPARTY_URL:-} ]]; then
      export YB_THIRDPARTY_URL=$auto_thirdparty_url
      log "Setting third-party URL to $auto_thirdparty_url"
      save_var_to_file_in_build_dir "$YB_THIRDPARTY_URL" thirdparty_url.txt
    elif [[ -n $auto_thirdparty_url ]]; then
      if [[ $auto_thirdparty_url != "$YB_THIRDPARTY_URL" ]]; then
        log "YB_THIRDPARTY_URL is already set to $YB_THIRDPARTY_URL, not trying to set it to" \
            "the default value of $auto_thirdparty_url"
      fi
    fi
  fi
}

# -------------------------------------------------------------------------------------------------
# Initialization
# -------------------------------------------------------------------------------------------------

detect_os

# http://man7.org/linux/man-pages/man7/signal.7.html
if is_mac; then
  declare -i -r SIGUSR1_EXIT_CODE=158  # 128 + 30
else
  # Linux
  # SIGUSR1_EXIT_CODE is used in common-test-env.sh
  # shellcheck disable=SC2034
  declare -i -r SIGUSR1_EXIT_CODE=138  # 128 + 10
fi

# Parent directory for build directories of all build types.
YB_BUILD_INTERNAL_PARENT_DIR=$YB_SRC_ROOT/build
YB_BUILD_EXTERNAL_PARENT_DIR=${YB_SRC_ROOT}__build
if [[ ${YB_USE_EXTERNAL_BUILD_ROOT:-} == "1" ]]; then
  YB_BUILD_PARENT_DIR=$YB_BUILD_EXTERNAL_PARENT_DIR
else
  YB_BUILD_PARENT_DIR=$YB_BUILD_INTERNAL_PARENT_DIR
fi

if [[ ! -d $YB_BUILD_SUPPORT_DIR ]]; then
  fatal "Could not determine YB source directory from '${BASH_SOURCE[0]}':" \
        "$YB_BUILD_SUPPORT_DIR does not exist."
fi

readonly -a YB_DEFAULT_CMAKE_OPTS=(
  "-DCMAKE_C_COMPILER=$YB_COMPILER_WRAPPER_CC"
  "-DCMAKE_CXX_COMPILER=$YB_COMPILER_WRAPPER_CXX"
)
