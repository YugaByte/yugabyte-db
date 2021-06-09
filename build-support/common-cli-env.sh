#!/usr/bin/env bash
#@IgnoreInspection BashAddShebang

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

# Common bash code for CLIs (e.g. cqlsh, yb-ctl).

set -euo pipefail

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  echo "${BASH_SOURCE[0]} must be sourced, not executed" >&2
  exit 1
fi

# Guard against multiple inclusions.
if [[ -n ${YB_COMMON_CLI_ENV_SOURCED:-} ]]; then
  # Return to the executing script.
  return
fi

readonly YB_COMMON_CLI_ENV_SOURCED=1

# shellcheck source=build-support/common-build-env.sh
. "${BASH_SOURCE%/*}/common-build-env.sh"

# Get variable BUILD_ROOT if it is empty.
# shellcheck disable=SC2120
set_build_root_from_latest_if_unset() {
  expect_num_args 0 "$@"
  if [[ -z ${BUILD_ROOT:-} ]]; then
    pushd "$YB_SRC_ROOT"/build/latest
    cd -P .
    handle_build_root_from_current_dir
    popd
    # shellcheck disable=SC2119
    set_build_root
  else
    predefined_build_root=$BUILD_ROOT
    handle_predefined_build_root
  fi
}

# Get variable YB_THIRDPARTY_DIR if it is empty.
# shellcheck disable=SC2120
set_yb_thirdparty_dir_if_unset() {
  expect_num_args 0 "$@"
  expect_vars_to_be_set BUILD_ROOT
  if [[ -z ${YB_THIRDPARTY_DIR:-} ]]; then
    find_or_download_thirdparty
  fi
}

set_build_root_from_latest_if_unset

set_yb_thirdparty_dir_if_unset
