#!/bin/bash
#
# Copyright 2020 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

check_type="provision"
airgap=false
install_node_exporter=false
mount_points=""
yb_home_dir="/home/yugabyte"
# This should be a comma separated key-value list. Associative arrays were add in bash 4.0 so
# they might not exist in the provided instance depending on how old it is.
result_kvs=""

preflight_provision_check() {
  # Check python is installed.
  sudo /bin/sh -c "/usr/bin/env python --version"
  update_result_json_with_rc "Sudo Access to Python" "$?"

  # Check for internet access.
  if [[ "$airgap" = false ]]; then
    # Send 3 packets with 3 second timeout and return success if any succeed. Do not send
    # multiple packets at once since ping will return an error if any packet fails.
    for i in 1 2 3; do
      ping -c 1 -W 3 www.yugabyte.com && break
    done
    update_result_json_with_rc "Internet Connection" "$?"
  fi

  if [[ $install_node_exporter = true ]]; then
    # Check node exporter isn't already installed.
    no_node_exporter=false
    if [[ "$(ps -ef | grep "node_exporter" | grep -v "grep" | grep -v "preflight" |
             wc -l | tr -d ' ')" = '0' ]]; then
      no_node_exporter=true
    fi
    update_result_json "(Prometheus) No Pre-existing Node Exporter Running" "$no_node_exporter"

    # Check prometheus files are writable.
    filepaths="/opt/prometheus /etc/prometheus /var/log/prometheus /var/run/prometheus \
      /var/lib/prometheus /lib/systemd/system/node_exporter.service"
    for path in $filepaths; do
      check_filepath "Prometheus" "$path" true
    done
  fi

  # Check ulimit settings.
  ulimit_filepath="/etc/security/limits.conf"
  check_filepath "PAM Limits" $ulimit_filepath true

  # Check mount points are writeable.
  IFS="," read -ra mount_points_arr <<< "$mount_points"
  for path in "${mount_points_arr[@]}"; do
    check_filepath "Mount Point" "$path" false
  done

  # Check yugabyte user belongs to yugabyte group if it exists.
  if id -u "yugabyte"; then
    yb_group=$(id -gn "yugabyte")
    user_status=false
    if [[ $yb_group == "yugabyte" ]]; then
      user_status=true
    fi
    update_result_json "Yugabyte User in Yugabyte Group" "$user_status"
  fi
}

preflight_configure_check() {
  # Check yugabyte user exists.
  id -u yugabyte
  update_result_json_with_rc "Yugabyte User" "$?"

  # Check yugabyte user belongs to group yugabyte.
  yb_group=$(id -gn "yugabyte")
  user_status=false
  if [[ $yb_group == "yugabyte" ]]; then
    user_status=true
  fi
  update_result_json "Yugabyte Group" "$user_status"

  # Check home directory exists.
  check_filepath "Home Directory" "$yb_home_dir" false
}

# Checks if given filepath is writable.
check_filepath() {
  test_type="$1"
  path="$2"
  check_parent="$3" # If true, will check parent directory is writable if given path doesn't exist.
  file_status="1" # 0 if success, else failed

  # Use sudo command for provision.
  if [[ "$check_type" == "provision" ]]; then
    # To reduce sudo footprint, use a format similar to what ansible would execute
    # (e.g. /bin/sh -c */usr/bin/env python *)
    if $check_parent; then
      sudo /bin/sh -c "/usr/bin/env python -c \"import os; \
        filepath = '$path' if os.path.exists('$path') else os.path.dirname('$path'); \
        exit(1) if not os.access(filepath, os.W_OK) else exit();\""
    else
      sudo /bin/sh -c "/usr/bin/env python -c \"import os; \
        exit(1) if not os.access('$path', os.W_OK) else exit();\""
    fi
  else
    test -w "$path" || ($check_parent && test -w $(dirname "$path"))
  fi

  update_result_json_with_rc "($test_type) $path is writable" "$?"
}

update_result_json() {
  # Input: test_name, check_passed
  if [[ -z "$result_kvs" ]]; then
    result_kvs="\"${1}\":\"${2}\""
  else
    result_kvs="${result_kvs},\"${1}\":\"${2}\""
  fi
}

update_result_json_with_rc() {
  # Input: test_name, returncode
  check_passed=false
  if [[ "$2" == "0" ]]; then
    check_passed=true
  fi
  update_result_json "$1" "$check_passed"
}


show_usage() {
  cat <<-EOT
Usage: ${0##*/} --type {configure,provision} [<options>]

Options:
  -t, --type (REQUIRED)
    Type of preflight check to perform. Must be in ['configure', 'provision'].
  --airgap
    Skip internet access check.
  --install_node_exporter
    Check if node exporter files are accessible.
  --mount_points MOUNT_POINTS
    Commas separated list of mount paths to check permissions of.
  --yb_home_dir HOME_DIR
    Home directory of yugabyte user.
  -h, --help
    Show usage.
EOT
}

if [[ ! $# -gt 0 ]]; then
  show_usage
  exit 1
fi

while [[ $# -gt 0 ]]; do
  case $1 in
    -t|--type)
      options="provision configure"
      if [[ ! $options =~ (^|[[:space:]])"$2"($|[[:space:]]) ]]; then
        echo "Invalid option: $2. Must be one of ['configure', 'provision'].\n"
        show_usage
        exit 1
      fi
      check_type="$2"
      shift
    ;;
    --airgap)
      airgap=true
    ;;
    --install_node_exporter)
      install_node_exporter=true
    ;;
    --mount_points)
      mount_points="$2"
      shift
    ;;
    --yb_home_dir)
      yb_home_dir="$2"
      shift
    ;;
    -h|--help)
      show_usage >&2
      exit 1
    ;;
    *)
      echo "Invalid option: $1\n"
      show_usage
      exit 1
  esac
  shift
done

if [[ "$check_type" == "provision" ]]; then
  preflight_provision_check >/dev/null 2>&1
else
  preflight_configure_check >/dev/null 2>&1
fi

echo "{$result_kvs}"
