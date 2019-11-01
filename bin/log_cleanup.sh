#!/bin/sh

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
set -euo pipefail

print_help() {
  cat <<EOT
Usage: ${0##*/} [<options>]
Options:
  -p, --logs_disk_percent_max <logsdiskpercent>
    max percentage of disk to use for logs (default=10).
  -z, --gzip_only
    only gzip files, don't purge.
  -h, --help
    Show usage
EOT
}

gzip_only=false
YB_HOME_DIR=/home/yugabyte/
MAX_LOG_SIZE=256
logs_disk_percent_max=10
while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--logs_disk_percent_max)
      logs_disk_percent_max=$2
      shift
    ;;
    -z|--gzip_only)
      gzip_only=true
    ;;
    -h|--help)
      print_help
      exit 0
    ;;
    *)
      echo "Invalid option: $1" >&2
      print_help
      exit 1
  esac
  shift
done

if [[ "$(id -u)" != "0" && "$USER" != "yugabyte" ]]; then
  echo "This script must be run as root or yugabyte" >&2
  exit 1
fi

if [[ $logs_disk_percent_max -lt 1 || $logs_disk_percent_max -gt 100 ]]; then
  echo "--logs_disk_percent_max needs to be [1, 100]" >&2
  exit 1
fi

# half for tserver and half for master.
logs_disk_percent_max=$(($logs_disk_percent_max / 2))

compute_num_log_files() {
  local log_dir=$1
  local daemon_type=$2
  # df --output=size isn't supported on all UNIX systems.
  # output in the format: [size]    [file]
  local logdirsize_kb=$(du -k $YB_HOME_DIR/$daemon_type/logs/ | awk '{print $1}')
  local per_log_size_mb=$MAX_LOG_SIZE
  if [[ $per_log_size_mb -le 0 ]]; then
    echo "--$MAX_LOG_SIZE_FLAG needs to be greater than 0, found $per_log_size_mb" >&2
    exit 1
  fi

  local num_logs_to_keep=$(($logdirsize_kb / 1000 * $logs_disk_percent_max / 100 / \
    $per_log_size_mb))

  if [[ $num_logs_to_keep -lt 1 ]]; then
    # Should be atleast 1.
    num_logs_to_keep=1
  fi
  echo $num_logs_to_keep
}

log_levels="INFO ERROR WARNING FATAL"
daemon_types=""
if [[ -d "$YB_HOME_DIR/master/" ]]; then
  daemon_types="${daemon_types} master"
fi
if [[ -d "$YB_HOME_DIR/tserver/" ]]; then
  daemon_types="${daemon_types} tserver"
fi
for daemon_type in $daemon_types; do
  YB_LOG_DIR="$YB_HOME_DIR/$daemon_type/logs/"
  num_logs_to_keep=$(compute_num_log_files $YB_LOG_DIR $daemon_type)
  echo "Num logs to keep for $daemon_type: $num_logs_to_keep"
  for log_level in $log_levels; do
    # Using print0 since printf is not supported on all UNIX systems.
    # xargs -0 -r stat -c '%Y %n' outputs: [unix time in millisecs] [name of file]
    find_non_gz_files="find $YB_LOG_DIR -type f -name
    'yb-$daemon_type*log.$log_level*' ! -name '*.gz' -print0 | xargs -0 -r stat -c '%Y %n' | sort | awk '{print \$2}'"
    non_gz_file_count=$(eval $find_non_gz_files | wc -l)

    # gzip all files but the current one.
    if [ $non_gz_file_count -gt 1 ]; then
      files_to_gzip=$(eval $find_non_gz_files | head -n$(($non_gz_file_count - 1)))
      for file in $files_to_gzip; do
        echo "Compressing file $file"
        gzip $file
      done
    fi

    if [ "$gzip_only" == false ]; then
      # now delete old gz files.
      # Using print0 since printf is not supported on all UNIX systems.
      # xargs -0 -r stat -c '%Y %n' outputs: [unix time in millisecs] [name of file]
      find_gz_files="find $YB_LOG_DIR -type f -name
      'yb-$daemon_type*log.$log_level*gz' -print0 | xargs -0 -r stat -c '%Y %n' | sort | awk '{print \$2}'"
      gz_file_count=$(eval $find_gz_files | wc -l)
      if [ $gz_file_count -gt $num_logs_to_keep ]; then
        files_to_delete=$(eval $find_gz_files | head -n$(($gz_file_count - $num_logs_to_keep)))
        for file in $files_to_delete; do
          echo "Delete file $file"
          rm $file
        done
      fi
    fi
  done
done
