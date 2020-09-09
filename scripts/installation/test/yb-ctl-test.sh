#!/usr/bin/env bash

set -euo pipefail

export YB_PG_FALLBACK_SYSTEM_USER_NAME=$USER
export YB_DISABLE_CALLHOME=1

readonly YSQL_DEFAULT_PORT=5433
ysql_ip=127.0.0.1

# This will be auto-detected the first time yb-ctl auto-downloads and installs YugaByte DB.
installation_dir=""

log() {
  echo >&2 "[$( date +%Y-%m-%dT%H:%M:%S )] $*"
}

fatal() {
  log "$@"
  exit 1
}

detect_installation_dir() {
  if [[ -z $installation_dir ]]; then
    installation_dir=$( ls -td "$HOME/yugabyte-db/yugabyte-"* | grep -v .tar.gz | head -1 )
    log "YugaByte DB has been automatically installed into directory: $installation_dir"
  fi
}

verify_ysqlsh() {
  local node_number=${1:-1}
  local ysql_port=${2:-$YSQL_DEFAULT_PORT}

  local ysql_ip=127.0.0.$node_number
  log "Waiting for YSQL to listen on port $ysql_ip:$ysql_port"

  local attempts=0
  while ! nc -z "$ysql_ip" "$ysql_port"; do
    if [[ $attempts -gt 600 ]]; then
      fatal "Timed out waiting for YSQL on $ysql_ip:$ysql_port after $(( $attempts / 10 )) sec"
    fi
    sleep 0.1
    let attempts+=1
  done

  log "YSQL listening on port $ysql_ip:$ysql_port"

  # Give the YSQL a chance to start up, or we'll get an error:
  # FATAL:  the database system is starting up
  sleep 1

  local ysqlsh_cmd=( "$installation_dir"/bin/ysqlsh -h "$ysql_ip" -p "$ysql_port" -U postgres )
  local table_name="mytable$RANDOM"
  log "Creating a YSQL table and inserting a bit of data"
  "${ysqlsh_cmd[@]}" <<-EOF
create table $table_name (k int primary key, v text);
insert into $table_name (k, v) values (10, 'sometextvalue');
insert into $table_name (k, v) values (20, 'someothertextvalue');
EOF
  log "Running a simple select from our YSQL table"
  echo "select * from $table_name where k = 10; drop table $table_name;" | \
    "${ysqlsh_cmd[@]}"| \
    grep "sometextvalue"
}

start_cluster_run_tests() {
  if [[ $# -ne 1 ]]; then
    fatal "One arg expected: root directory to run in"
  fi
  local root_dir=$1
  ( set -x; "$python_interpreter" "$root_dir"/yb-ctl start $create_flags "${yb_ctl_args[@]}" )
  verify_ysqlsh
  (
    set -x
    "$python_interpreter" "$root_dir"/yb-ctl add_node $create_flags "${yb_ctl_args[@]}"
  )
  verify_ysqlsh
  ( set -x; "$python_interpreter" "$root_dir"/yb-ctl stop_node 1 "${yb_ctl_args[@]}" )

  # It looks like if we try to create a table in this state, the master is trying to assign
  # tablets to node 1, which is down, and times out:
  #
  # TODO: re-enable when https://github.com/YugaByte/yugabyte-db/issues/1508 is fixed.
  if false; then
    verify_ysqlsh 2
  fi
  (
    set -x
    "$python_interpreter" "$root_dir"/yb-ctl start_node 1 $create_flags "${yb_ctl_args[@]}"
  )
  verify_ysqlsh
  ( set -x; "$python_interpreter" "$root_dir"/yb-ctl stop "${yb_ctl_args[@]}" )
  ( set -x; "$python_interpreter" "$root_dir"/yb-ctl destroy "${yb_ctl_args[@]}" )
}

readonly yb_data_dir_parent="/tmp/yb-ctl-test-data-$( date +%Y-%m-%dT%H_%M_%S )-$RANDOM"
readonly yb_data_dir=$yb_data_dir_parent/single_univ
readonly yb_univ_1_data_dir=$yb_data_dir_parent/cdc_1
readonly yb_univ_2_data_dir=$yb_data_dir_parent/cdc_2

yb_ctl_args=(
  --data_dir "$yb_data_dir"
)

create_flags=""

thick_log_heading() {
  (
    echo
    echo "========================================================================================"
    echo "$@"
    echo "========================================================================================"
    echo
  ) >&2
}

log_heading() {
  (
    echo
    echo "----------------------------------------------------------------------------------------"
    echo "$@"
    echo "----------------------------------------------------------------------------------------"
    echo
  ) >&2
}

cleanup() {
  local exit_code=$?
  if [[ $exit_code -ne 0 ]]; then
    log "^^^ SEE THE ERROR MESSAGE ABOVE ^^^"
    thick_log_heading "Dumping all the log files below:"
  fi

  if [[ -d $yb_data_dir_parent ]]; then
    find "$yb_data_dir_parent" \
      -name "*.out" -or \
      -name "*.err" -or \
      -name "*.INFO" -or \
      \( -name "*.log" -and -not -wholename "*/tablet-*/*.log" \) | sort |
    while read log_path; do
      log_heading "$log_path"
      cat "$log_path" >&2
    done
  else
    log_heading "No data at $yb_data_dir_parent"
  fi

  thick_log_heading "End of dumping various logs"

  if [[ $exit_code -ne 0 ]]; then
    echo "Scroll up past the various logs to where it says 'SEE THE ERROR MESSAGE'."
  fi

  if "$keep"; then
    log "Not killing yb-master/yb-tserver processes or removing data directories at " \
        "$yb_data_dir_parent"
  else
    if [[ -d $yb_data_dir_parent ]]; then
      find "$yb_data_dir_parent" \
          -maxdepth 1 -type d | sort |
      while read data_dir; do
        log "Killing yb-master/yb-tserver processes for $data_dir"
        set +e
        (
          set -x
          pkill -f "yb-master --fs_data_dirs $data_dir/" -SIGKILL
          pkill -f "yb-tserver --fs_data_dirs $data_dir/" -SIGKILL
        )

        set -e
        if [[ -d $data_dir ]]; then
          log "Removing data directory at $data_dir"
          ( set -x; rm -rf "$data_dir" )
        fi
      done
    fi
  fi

  log "Exiting with code $exit_code"
  exit "$exit_code"
}

print_usage() {
  cat <<-EOT
Usage: ${0##*/} [<options>]
Options:
  -h, --help
    Print usage information.
  -k, --keep
    Keep the cluster data directory around and keep servers running on shutdown.
  --python3
    Use python3
  --verbose
    Produce verbose output -- passed down to yb-ctl.
EOT
}

# -------------------------------------------------------------------------------------------------
# Parsing test arguments
# -------------------------------------------------------------------------------------------------

verbose=false
keep=false
python_interpreter=python

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      print_usage
      exit 0
    ;;
    -v|--verbose)
      verbose=true
    ;;
    -k|--keep)
      keep=true
    ;;
    --python27)
      python_interpreter=python2.7
    ;;
    --python3)
      python_interpreter=python3
    ;;
    --python_interpreter)
      python_interpreter=$2
      shift
    ;;
    *)
      print_usage >&2
      echo >&2
      echo "Invalid option: $1" >&2
      exit 1
  esac
  shift
done

if "$verbose"; then
  yb_ctl_args+=( --verbose )
fi

# -------------------------------------------------------------------------------------------------
# Main test code
# -------------------------------------------------------------------------------------------------

script_dir=$( cd "$( dirname "$0" )" && pwd )
cd "$script_dir"/..




CI_RUN=${TRAVIS:-${GITHUB_ACTIONS:-undefined}}
log "OSTYPE: $OSTYPE"
log "USER: $USER"
log "CI_RUN: $CI_RUN"

# Mac needs loopback aliases explicitly created:
#   https://docs.yugabyte.com/latest/quick-start/install/
if [[ ${CI_RUN} == "true" && $OSTYPE == darwin* ]]; then
  sudo ifconfig lo0 alias 127.0.0.2
  sudo ifconfig lo0 alias 127.0.0.3
  sudo ifconfig lo0 alias 127.0.0.4
  sudo ifconfig lo0 alias 127.0.0.5
  sudo ifconfig lo0 alias 127.0.0.6
  sudo ifconfig lo0 alias 127.0.0.7
fi

# Make top level data dir.
mkdir -p "$yb_data_dir_parent"

trap cleanup EXIT

log_heading "Running basic tests"
(
  set -x
  "$python_interpreter" bin/yb-ctl create $create_flags "${yb_ctl_args[@]}" --install-if-needed
)

detect_installation_dir
verify_ysqlsh

(
  set -x
  "$python_interpreter" bin/yb-ctl stop "${yb_ctl_args[@]}"
  "$python_interpreter" bin/yb-ctl destroy "${yb_ctl_args[@]}"
)

start_cluster_run_tests "bin"

log_heading "Testing YSQL port override"
custom_ysql_port=54320
(
  set -x
  "$python_interpreter" bin/yb-ctl create --ysql_port "$custom_ysql_port" \
      $create_flags "${yb_ctl_args[@]}"
)
verify_ysqlsh 1 "$custom_ysql_port"
(
  set -x
  "$python_interpreter" bin/yb-ctl stop "${yb_ctl_args[@]}"
)
log "Checking that the custom YSQL port persists across restarts"
(
  set -x
  "$python_interpreter" bin/yb-ctl start $create_flags "${yb_ctl_args[@]}"
)
verify_ysqlsh 1 "$custom_ysql_port"
(
  set -x
  "$python_interpreter" bin/yb-ctl destroy "${yb_ctl_args[@]}"
)

log_heading "Test creating multiple universes"
(
  set -x
  "$python_interpreter" bin/yb-ctl create $create_flags --data_dir "$yb_univ_1_data_dir" --rf 1
)
verify_ysqlsh

log_heading "Creating second universe with custom ip_start"
custom_ip_start=2
(
  set -x
  "$python_interpreter" bin/yb-ctl create --ip_start $custom_ip_start $create_flags \
      --data_dir "$yb_univ_2_data_dir" --rf 1
)
verify_ysqlsh $custom_ip_start

(
  set -x
  "$python_interpreter" bin/yb-ctl stop --data_dir "$yb_univ_1_data_dir"
)
(
  set -x
  "$python_interpreter" bin/yb-ctl stop --data_dir "$yb_univ_2_data_dir"
)

log "Checking that the universes and custom ip addresses persist across restarts"
(
  set -x
  "$python_interpreter" bin/yb-ctl start $create_flags --data_dir "$yb_univ_1_data_dir"
)
(
  set -x
  "$python_interpreter" bin/yb-ctl start $create_flags --data_dir "$yb_univ_2_data_dir"
)
verify_ysqlsh
verify_ysqlsh $custom_ip_start
(
  set -x
  "$python_interpreter" bin/yb-ctl destroy --data_dir "$yb_univ_1_data_dir"
)
(
  set -x
  "$python_interpreter" bin/yb-ctl destroy --data_dir "$yb_univ_2_data_dir"
)

# -------------------------------------------------------------------------------------------------
log_heading "Testing putting this version of yb-ctl inside the installation directory"
( set -x; cp bin/yb-ctl "${installation_dir}/bin" )
start_cluster_run_tests "${installation_dir}/bin"

log_heading \
  "Pretending we've just built the code and are running yb-ctl from the bin directory in the code"
yb_src_root=$HOME/yugabyte-db-src-root
submodule_bin_dir=$yb_src_root/scripts/installation/bin
mkdir -p "$submodule_bin_dir"
cp bin/yb-ctl "$submodule_bin_dir"
mkdir -p "$yb_src_root/build"
yb_build_root=$yb_src_root/build/latest

( set -x; time cp -R "$installation_dir" "$yb_build_root" )

if [[ $OSTYPE == linux* ]]; then
  ( set -x; time "$yb_build_root/bin/post_install.sh" )
fi

(
  set -x
  installation_dir=$yb_build_root
  "$python_interpreter" "$submodule_bin_dir/yb-ctl" start $create_flags "${yb_ctl_args[@]}"
)
verify_ysqlsh
(
  "$python_interpreter" "$submodule_bin_dir/yb-ctl" stop "${yb_ctl_args[@]}"
  "$python_interpreter" "$submodule_bin_dir/yb-ctl" destroy "${yb_ctl_args[@]}"
)

log_heading "TESTS SUCCEEDED"
