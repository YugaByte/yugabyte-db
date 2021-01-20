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
    Show usage.
  -s, --postgres_max_log_size <size in mb>
    max size of disk to use for postgres logs (default=100mb).
  -d, --cores_disk_percent_max <number>
    max percentage of disk to use for core dump files (default=10).
EOT
}

gzip_only=false
YB_HOME_DIR=/home/yugabyte
YB_CORES_DIR="/var/yugabyte/cores"

logs_disk_percent_max=10
postgres_max_log_size_kb=$((100 * 1000))
cores_disk_percent_max=10

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--logs_disk_percent_max)
      logs_disk_percent_max=$2
      shift
    ;;
    -s|--postgres_max_log_size)
      postgres_max_log_size_kb=$(($2 * 1000))
      shift
    ;;
    -z|--gzip_only)
      gzip_only=true
    ;;
    -d|--cores_disk_percent_max)
      cores_disk_percent_max=$2
      shift
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

if [[ $cores_disk_percent_max -lt 1 || $cores_disk_percent_max -gt 100 ]]; then
  echo "--cores_disk_percent_max needs to be [1, 100]" >&2
  exit 1
fi

# half for tserver and half for master.
logs_disk_percent_max=$(($logs_disk_percent_max / 2))

delete_gz_files() {
  set -f
  local log_dir=$1
  local find_regex=$2
  local permitted_usage=$3
  local logs_disk_usage_bytes=$(find $log_dir -type f -name $find_regex -print0 | \
    xargs -0 -r stat -c '%s' | \
    awk '{sum+=$1;}END{print sum;}')
  if [ -z $logs_disk_usage_bytes ]; then
    logs_disk_usage_bytes=0
  fi
  local logs_disk_usage_kb=$(($logs_disk_usage_bytes / 1000 ))
  echo "Permitted disk usage for $find_regex files in kb: $permitted_usage"
  echo "Disk usage by $find_regex files in kb: $logs_disk_usage_kb"
  # get all the gz files.
  local gz_files=$(find $log_dir -type f -name $find_regex.gz -print0 | \
    xargs -0 -r stat -c '%Y %n' | \
    sort | awk '{print $2}')
  for file in $gz_files; do
    # If usage exceeds permitted, delete the old gz files.
    if [ $logs_disk_usage_kb -gt $permitted_usage ]; then
      local file_size=$(du -k $file | awk '{print $1}')
      logs_disk_usage_kb=$(($logs_disk_usage_kb-$file_size))
      echo "Delete file $file"
      rm $file
    else
      break
    fi
  done
}

delete_core_dump_files () {
  local core_dump_dir="$1"
  local permitted_usage="$2"
  local disk_usage_kb="$(du -sk $core_dump_dir | awk '{print $1}')"
  echo "Permitted disk usage for core dump files in kb: $permitted_usage"
  echo "Disk usage by core dump files in kb: $disk_usage_kb"

  # Sort by time: oldest first
  local files="$(ls -Acr $core_dump_dir)"
  # Handle space in a file name
  IFS=$'\n'
  for file in $files; do
    file="${core_dump_dir}/${file}"
    # If usage exceeds permitted, delete the old files.
    if [ $disk_usage_kb -gt $permitted_usage ]; then
      local file_size=$(du -k ${file} | awk '{print $1}')
      disk_usage_kb=$(($disk_usage_kb-$file_size))
      echo "Deleting core file ${file}"
      rm "${file}"
    else
      break
    fi
  done
  unset IFS
}

server_types="master tserver"
daemon_types=""
for server_type in $server_types; do
  if [[ -d "$YB_HOME_DIR/$server_type/logs" ]]; then
    daemon_types="${daemon_types} $server_type"
  fi
done
log_levels="INFO ERROR WARNING FATAL"
for daemon_type in $daemon_types; do
  YB_LOG_DIR="$YB_HOME_DIR/$daemon_type/logs/"
  log_regexes="postgres*log"

  for level in $log_levels; do
    log_regexes="${log_regexes} yb-$daemon_type*log.$level*"
  done

  for log_regex in $log_regexes; do
    # Using print0 since printf is not supported on all UNIX systems.
    # xargs -0 -r stat -c '%Y %n' outputs: [unix time in millisecs] [name of file]
    find_non_gz_files="find $YB_LOG_DIR -type f -name
    '$log_regex' ! -name '*.gz' -print0 | xargs -0 -r stat -c '%Y %n' | sort | awk '{print \$2}'"
    non_gz_file_count=$(eval $find_non_gz_files | wc -l)

    # gzip all files but the current one.
    if [ $non_gz_file_count -gt 1 ]; then
      files_to_gzip=$(eval $find_non_gz_files | head -n$(($non_gz_file_count - 1)))
      for file in $files_to_gzip; do
        echo "Compressing file $file"
        gzip $file
      done
    fi
  done

  if [ "$gzip_only" == false ]; then
    server_log="yb-$daemon_type*log.*"
    postgres_log="postgres*log*"
    # Get total size of disk in kb and then compute permitted usage for the log files.
    # We get the size of the target link of $YB_LOG_DIR
    disk_size_kb=$(df -k $YB_LOG_DIR | awk 'NR==2{print $2}')
    permitted_disk_usage_kb=$(($disk_size_kb * $logs_disk_percent_max / 100))
    delete_gz_files $YB_LOG_DIR $server_log $permitted_disk_usage_kb
    delete_gz_files $YB_LOG_DIR $postgres_log $postgres_max_log_size_kb
  fi
done

if [ -d "$YB_CORES_DIR" ]; then
  core_dump_disk_size_kb=$(df -k $YB_CORES_DIR | awk 'NR==2{print $2}')
  core_dump_max_size_kb=$(($core_dump_disk_size_kb * $cores_disk_percent_max / 100))
  delete_core_dump_files $YB_CORES_DIR $core_dump_max_size_kb
fi
