#!/usr/bin/env bash

set -euo pipefail

# -------------------------------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------------------------------

readonly DEFAULT_MASTER_RPC_PORT="7100"
readonly DEFAULT_TSERVER_RPC_PORT="9100"
readonly DEFAULT_MASTER_WEBSERVER_PORT="7000"
readonly DEFAULT_TSERVER_WEBSERVER_PORT="9000"
readonly DEFAULT_UI=false
readonly DEFAULT_LISTEN_ADDR="0.0.0.0"

# YSQL
readonly DEFAULT_YSQL_PORT="5433"
readonly DEFAULT_YSQL_USER="yugabyte"
readonly DEFAULT_YSQL_PASSWORD="yugabyte"
readonly DEFAULT_YSQL_DB="yugabyte"
readonly TEST_YSQL_USER="test"
readonly TEST_YSQL_PASSWORD="testpassword"
readonly TEST_YSQL_DB="testdb"
readonly TEST_YSQL_PORT="40001"
readonly DEFAULT_YSQL_ENABLE_AUTH=false

# YCQL
readonly DEFAULT_YCQL_PORT="9042"
readonly DEFAULT_YCQL_USER="cassandra"
readonly DEFAULT_YCQL_PASSWORD="cassandra"
readonly DEFAULT_YCQL_KEYSPACE="cassandra"
readonly TEST_YCQL_USER="test"
readonly TEST_YCQL_PASSWORD="testpassword"
readonly TEST_YCQL_KEYSPACE="testks"
readonly TEST_YCQL_PORT="40002"
readonly DEFAULT_USE_CASSANDRA_AUTHENTICATION=false

readonly YUGABYTED_BINARY="bin/yugabyted"
readonly PARENT_TEST_DIR="/tmp/test-yugabyted/yugabyted-test-$( date +%Y-%m-%dT%H_%M_%S )-$RANDOM"
readonly EXTRACTED_YB_TAR="${PARENT_TEST_DIR}/yugabyte-db"
readonly DOWNLOADED_TAR="${PARENT_TEST_DIR}/tar"
readonly TEST_BASE_DIR="${PARENT_TEST_DIR}/base_dir"

TEST_IMAGE="yugabyted-test:latest"

declare -a CONTAINER_IDS
declare -a DOCKER_ENV_VARS
declare -a ENV_VARS

CONTAINER_ID=
DOCKER_ENV=false

# -------------------------------------------------------------------------------------------------
# Functions
# -------------------------------------------------------------------------------------------------

log() {
  echo >&2 "[$( date +%Y-%m-%dT%H:%M:%S )] $*"
}

fatal() {
  log "$@"
  exit 1
}

print_usage() {
  cat <<-EOT
Usage: ${0##*/} [<options>]

Options:

  Mandatory: Either Package or Docker image should exist.
  -p, --package
    YugabyteDB package (release tar).
  -i, --image
    YugabyteDB Docker image.

  Optional:
  -h, --help
    Print usage information.
  -y, --yugabyted
    Custom yugabyted.
  -P, --python
    Provide Python interpreter.
  -T, --testsuite
    Provide test suite. Default: basic
    [basic|intermediate|advanced]
  -t, --testcase
    Provide test case names in comma seperated string.
    Available test cases:
    1. base_dir
    2. env_vars
    3. default_ysql_authentication
    4. default_ycql_authentication
    5. custom_ysql_port
    6. custom_ycql_port

  Advanced:
  --master_flags
    Set YB-Master gflags - comma-seperated string, passed directly to yugabyted.
  --tserver_flags
    Set YB-Tserver gflags -comma-seperated string, passed directly to yugabyted.

  Note: Custom combination of test cases (-t) can be used to test single combination externally.

  Examples:
    1. With package and custom yugabyted.
      yugabyted-test.sh -p /path/package.tar.gz -y /path/yugabyted

    2. With package and advanced test suite.
      yugabyted-test.sh -p /path/package.tar.gz -T advanced

    3. With docker image and package.
      yugabyted-test.sh -p /path/package.tar.gz -i image:tag

    4. With docker image and remote package url
      yugabyted-test.sh -p https://example.com/package.tar.gz -i image:tag

    5. With custom test case combination
      yugabyted-test.sh -p /path/package.tar.gz -t "custom_ysql_port,custom_ycql_port"

    6. Advanced flags
      yugabyted-test.sh -p package.tar.gz --master_flags="flag_name=flag_value,flag_name=flag_value"
EOT
}

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

yugabyted_start() {
  local ysql_port="${DEFAULT_YSQL_PORT}"
  local ycql_port="${DEFAULT_YCQL_PORT}"
  local master_rpc_port="${DEFAULT_MASTER_RPC_PORT}"
  local tserver_rpc_port="${DEFAULT_TSERVER_RPC_PORT}"
  local master_webserver_port="${DEFAULT_MASTER_WEBSERVER_PORT}"
  local tserver_webserver_port="${DEFAULT_TSERVER_WEBSERVER_PORT}"
  local ui=$DEFAULT_UI

  declare -a start_args_array

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --base_dir) shift; start_args_array+=(--base_dir="$1") ;;
      --data_dir) shift; start_args_array+=(--data_dir="$1") ;;
      --log_dir) shift; start_args_array+=(--log_dir="$1") ;;
      --listen) shift; start_args_array+=(--listen="$1") ;;
      --ysql_port) shift; start_args_array+=(--ysql_port="$1"); ysql_port=$1 ;;
      --ycql_port) shift; start_args_array+=(--ycql_port="$1"); ycql_port=$1 ;;
      --master_rpc_port) shift; start_args_array+=(--master_rpc_port="$1"); master_rpc_port=$1 ;;
      --tserver_rpc_port) shift; start_args_array+=(--tserver_rpc_port="$1"); tserver_rpc_port=$1 ;;
      --master_webserver_port) shift; start_args_array+=(--master_webserver_port="$1"); master_webserver_port=$1 ;;
      --tserver_webserver_port) shift; start_args_array+=(--tserver_webserver_port="$1"); tserver_webserver_port=$1 ;;
      --ysql_enable_auth) shift; start_args_array+=(--ysql_enable_auth="$1") ;;
      --use_cassandra_authentication) shift; start_args_array+=(--use_cassandra_authentication="$1") ;;
      --ui) shift; start_args_array+=(--ui="$1") ;;
    esac
    shift
  done

  if [[ "${DOCKER_ENV}" == "true" ]]; then
    start_args_array+=(--daemon=false)
  else
    start_args_array+=(--daemon=true)
  fi

  if ! [[ " ${start_args_array[*]} " == *"--ui"* ]]; then
    start_args_array+=(--ui="${ui}")
  fi

  # Set advanced flags directly to yugabyted.
  if [[ -n "${master_flags}" ]]; then
    start_args_array+=(--master_flags="${master_flags}")
  fi
  if [[ -n "${tserver_flags}" ]]; then
    start_args_array+=(--tserver_flags="${tserver_flags}")
  fi

  if [[ "${DOCKER_ENV}" == "true" ]]; then

    CONTAINER_ID=$(docker run -d \
    -p "${ysql_port}":"${ysql_port}" \
    -p "${ycql_port}":"${ycql_port}" \
    -p "${master_rpc_port}":"${master_rpc_port}" \
    -p "${tserver_rpc_port}":"${tserver_rpc_port}" \
    -p "${master_webserver_port}":"${master_webserver_port}" \
    -p "${tserver_webserver_port}":"${tserver_webserver_port}" \
    ${DOCKER_ENV_VARS[@]-} \
    ${TEST_IMAGE} ${YUGABYTED_BINARY} start "${start_args_array[@]}")

    log "Container ID: ${CONTAINER_ID}"
    CONTAINER_IDS+=("$CONTAINER_ID")
  else
    "${python_interpreter}" "${EXTRACTED_YB_TAR}/${YUGABYTED_BINARY}" start "${start_args_array[@]}"
  fi
}

yugabyted_stop() {
  declare -a stop_args_array

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --base_dir) shift; stop_args_array+=(--base_dir="$1") ;;
      --data_dir) shift; stop_args_array+=(--data_dir="$1") ;;
    esac
    shift
  done

  if [[ "${DOCKER_ENV}" == "true" ]]; then
    docker stop "${CONTAINER_ID}"
  else
    if [[ -z "${stop_args_array[*]-}" ]]; then
      "${python_interpreter}" "${EXTRACTED_YB_TAR}/${YUGABYTED_BINARY}" stop
    else
      "${python_interpreter}" "${EXTRACTED_YB_TAR}/${YUGABYTED_BINARY}" stop "${stop_args_array[@]}"
    fi
  fi
}

setup_test_env() {

  log_heading "Setup: Test Environment - Started"

  mkdir -p "${PARENT_TEST_DIR}" "${EXTRACTED_YB_TAR}"

  tar_name="${package##*/}"
  # basename "${package}"

  # If URL is passed: Download and extract
  # else extract the providied tar directly.
  if [[ "${package}" == "https://"* ]]; then
    mkdir -p "${DOWNLOADED_TAR}"
    wget "${package}" -P "${DOWNLOADED_TAR}"
    tar -xzf "${DOWNLOADED_TAR}/${tar_name}" --strip 1 -C "${EXTRACTED_YB_TAR}"
  else
    tar -xzf "${package}" --strip 1 -C "${EXTRACTED_YB_TAR}"
  fi

  # Replace yugabyted in package
  if [[ -n "$yugabyted" ]]; then
    if ! [[ "${yugabyted}" -ef "${EXTRACTED_YB_TAR}/${YUGABYTED_BINARY}" ]]; then
      cp -f "${yugabyted}" "${EXTRACTED_YB_TAR}/${YUGABYTED_BINARY}"
      log "Yugabyted copied Successfully."
    fi
  fi

  log_heading "Setup: Test Environment - Completed"
}

setup_docker_test_env() {

  log_heading "Setup: Docker Test Environment - Started"

  mkdir -p "${PARENT_TEST_DIR}"

  if ! [[ -x "$(command -v docker)" ]]; then
    fatal "docker not found"
  fi

  if [[ -n "$docker_image" ]]; then
    # Create docker image
    if [[ -n "$yugabyted" ]]; then
      mkdir -p "${PARENT_TEST_DIR}/docker"

      cat >"${PARENT_TEST_DIR}/docker/Dockerfile" <<EOL
FROM ${docker_image}
COPY yugabyted /home/yugabyte/${YUGABYTED_BINARY}
EOL

      cp -f "${yugabyted}" "${PARENT_TEST_DIR}/docker/yugabyted"

      docker build -t "${TEST_IMAGE}" \
      -f "${PARENT_TEST_DIR}/docker/Dockerfile" "${PARENT_TEST_DIR}/docker/"
    else
      TEST_IMAGE="${docker_image}"
    fi
  fi

  DOCKER_ENV=true

  log_heading "Setup: Docker Test Environment - Completed"
}

cleanup() {
  local exit_code=$?
  if [[ $exit_code -ne 0 ]]; then
    log "^^^ SEE THE ERROR MESSAGE ABOVE ^^^"
    thick_log_heading "Test case failed."

    if [[ "${DOCKER_ENV}" == "false" ]]; then
      # Kill yugabyted forcefully
      pkill -f "${python_interpreter} ${EXTRACTED_YB_TAR}/${YUGABYTED_BINARY}" -SIGKILL
    fi
  fi

  if [[ "${DOCKER_ENV}" == "true" ]] && [[ -n "${CONTAINER_IDS[*]-}" ]]; then
    docker rm -f "${CONTAINER_IDS[@]}"
  fi

  if [[ $exit_code -eq 0 ]]; then
    rm -rf /tmp/test-yugabyted
    thick_log_heading "All test cases are passed."
  fi

  log "Exiting with code $exit_code"
  exit "$exit_code"
}

# -------------------------------------------------------------------------------------------------
# Test cases
# -------------------------------------------------------------------------------------------------

declare -a yugabyted_start_args
declare -a yugabyted_restart_args
declare -a yugabyted_stop_args
declare -a ysql_args
declare -a ycql_args

parse_test_case(){

  local testcases="$*"

  thick_log_heading "Test case: ${testcases[*]}"

  if [[ " ${testcases[*]-} " == *"env_vars"* ]]; then
    case_number=1

    read -r -a testcases <<< "${testcases[@]#*env_vars}"
    while (( case_number < 8 )); do

      env_vars "$case_number"

      execute_test_case "${testcases[@]-}"

      case_number=$(( case_number+1 ))
    done
  else
    execute_test_case "${testcases[@]-}"
  fi
}

execute_test_case() {

  if ! [[ "$*" == "default" ]]; then
    for test in "$@"; do
      $test
    done
  fi

  set_env_vars
  yugabyted_start "${yugabyted_start_args[@]-}"
  verify_ysqlsh "${ysql_args[@]-}"
  verify_ycqlsh "${ycql_args[@]-}"
  yugabyted_stop "${yugabyted_stop_args[@]-}"
  unset_env_vars

  # Restart the yugabyted to verify the configuration persist or not.
  # In Docker, it won't verify the same.
  if [[ "${DOCKER_ENV}" == "false" ]]; then
    log_heading "Restarting with previous configuration"
    yugabyted_start "${yugabyted_restart_args[@]-}"
    verify_ysqlsh "${ysql_args[@]-}"
    verify_ycqlsh "${ycql_args[@]-}"
    yugabyted_stop "${yugabyted_stop_args[@]-}"

    if [[ -d "${TEST_BASE_DIR}" ]]; then
      rm -rf "${TEST_BASE_DIR}"
    fi

    if [[ -d "${EXTRACTED_YB_TAR}/var" ]]; then
      rm -rf "${EXTRACTED_YB_TAR:?}"/var
    fi
  fi

  # Clean old configuration
  yugabyted_start_args=()
  yugabyted_stop_args=()
  yugabyted_restart_args=()
  ysql_args=()
  ycql_args=()
  DOCKER_ENV_VARS=()
  ENV_VARS=()

  if [[ "${DOCKER_ENV}" == "true" ]] && [[ -n "${CONTAINER_IDS[*]-}" ]]; then
    docker rm -f "${CONTAINER_ID}"
    read -r -a CONTAINER_IDS <<< "${CONTAINER_IDS[@]#*$CONTAINER_ID}"
  fi
}

base_dir() {
  yugabyted_start_args+=(--base_dir "${TEST_BASE_DIR}")
  yugabyted_restart_args+=(--base_dir "${TEST_BASE_DIR}")
  yugabyted_stop_args+=(--base_dir "${TEST_BASE_DIR}")
}

custom_ysql_port() {
  yugabyted_start_args+=(--ysql_port "$TEST_YSQL_PORT")
  ysql_args+=(--port "$TEST_YSQL_PORT")
}

custom_ycql_port() {
  yugabyted_start_args+=(--ycql_port "$TEST_YCQL_PORT")
  ycql_args+=(--port "$TEST_YCQL_PORT")
}

default_ysql_authentication() {
  yugabyted_start_args+=(--ysql_enable_auth true)
  ysql_args+=(--auth true)
  ENV_VARS+=(PGPASSWORD="$DEFAULT_YSQL_PASSWORD")
}

default_ycql_authentication() {
  yugabyted_start_args+=(--use_cassandra_authentication true)
  ycql_args+=(--auth true)
}

env_vars() {

  case "$1" in
    1)
      log_heading "First case: YSQL_PASSWORD, YCQL_PASSWORD"
      ysql_args=(--auth true --password "$TEST_YSQL_PASSWORD")
      ycql_args=(--auth true --password "$TEST_YCQL_PASSWORD")
      ENV_VARS=(
        YSQL_PASSWORD="$TEST_YSQL_PASSWORD"
        YCQL_PASSWORD="$TEST_YCQL_PASSWORD"
      );;
    2)
      log_heading "Second case: YSQL_PASSWORD, YSQL_DB, YCQL_PASSWORD, YCQL_KEYSPACE"
      ysql_args=(--auth true --password "$TEST_YSQL_PASSWORD" --db "$TEST_YSQL_DB")
      ycql_args=(--auth true --password "$TEST_YCQL_PASSWORD" --keyspace "$TEST_YCQL_KEYSPACE")
      ENV_VARS=(
        YSQL_PASSWORD="$TEST_YSQL_PASSWORD"
        YSQL_DB="$TEST_YSQL_DB"
        YCQL_PASSWORD="$TEST_YCQL_PASSWORD"
        YCQL_KEYSPACE="$TEST_YCQL_KEYSPACE"
      );;
    3)
      log_heading "Third case: YSQL_PASSWORD, YSQL_USER, YCQL_USER, YCQL_PASSWORD"
      ysql_args=(--auth true --password "$TEST_YSQL_PASSWORD" --user "$TEST_YSQL_USER")
      ycql_args=(--auth true --password "$TEST_YCQL_PASSWORD" --user "$TEST_YCQL_USER")
      ENV_VARS=(
        YSQL_PASSWORD="$TEST_YSQL_PASSWORD"
        YSQL_USER="$TEST_YSQL_USER"
        YCQL_PASSWORD="$TEST_YCQL_PASSWORD"
        YCQL_USER="$TEST_YCQL_USER"
      );;
    4)
      log_heading "Fourth case: YSQL_USER, YCQL_USER"
      ysql_args=(--user "$TEST_YSQL_USER" --password "$TEST_YSQL_USER" --db "$TEST_YSQL_USER")
      ycql_args=(--auth true --password "$TEST_YCQL_USER" --user "$TEST_YCQL_USER" --keyspace "$TEST_YCQL_USER")
      ENV_VARS=(
        YSQL_USER="$TEST_YSQL_USER"
        YCQL_USER="$TEST_YCQL_USER"
      );;
    5)
      log "Fifth case: YSQL_USER, YSQL_DB, YCQL_USER, YCQL_KEYSPACE"
      ysql_args=(--user "$TEST_YSQL_USER" --password "$TEST_YSQL_USER" --db "$TEST_YSQL_DB")
      ycql_args=(--auth true --password "$TEST_YCQL_USER" --user "$TEST_YCQL_USER" --keyspace "$TEST_YCQL_KEYSPACE")
      ENV_VARS=(
        YSQL_USER="$TEST_YSQL_USER"
        YSQL_DB="$TEST_YSQL_DB"
        YCQL_USER="$TEST_YCQL_USER"
        YCQL_KEYSPACE="$TEST_YCQL_KEYSPACE"
      );;
    6)
      log_heading "Sixth case: YSQL_DB, YCQL_KEYSPACE"
      ysql_args=(--db "$TEST_YSQL_DB")
      ycql_args=(--keyspace "$TEST_YCQL_KEYSPACE")
      ENV_VARS=(
        YSQL_DB="$TEST_YSQL_DB"
        YCQL_KEYSPACE="$TEST_YCQL_KEYSPACE"
      );;
    7)
      log_heading "Seventh case: YSQL_USER, YSQL_PASSWORD, YSQL_DB, YCQL_USER, YCQL_PASSWORD, YCQL_KEYSPACE"
      ysql_args=(--auth true  --user "$TEST_YSQL_USER" --password "$TEST_YSQL_PASSWORD" --db "$TEST_YSQL_DB")
      ycql_args=(--auth true --password "$TEST_YCQL_PASSWORD" --user "$TEST_YCQL_USER" --keyspace "$TEST_YCQL_KEYSPACE")
      ENV_VARS=(
        YSQL_PASSWORD="$TEST_YSQL_PASSWORD"
        YSQL_USER="$TEST_YSQL_USER"
        YSQL_DB="$TEST_YSQL_DB"
        YCQL_USER="$TEST_YCQL_USER"
        YCQL_PASSWORD="$TEST_YCQL_PASSWORD"
        YCQL_KEYSPACE="$TEST_YCQL_KEYSPACE"
      );;
  esac
}

set_env_vars() {
  if [[ -n "${ENV_VARS[*]-}" ]]; then
    if [[ "${DOCKER_ENV}" == "true" ]]; then
      for var_to_set in "${ENV_VARS[@]}"; do
        DOCKER_ENV_VARS+=(-e "$var_to_set")
      done
    else
      for var_to_set in "${ENV_VARS[@]}"; do
        export "${var_to_set?}"
      done
    fi
  fi
}

unset_env_vars() {
  if [[ -n "${ENV_VARS[*]-}" ]]; then
    for var_to_unset in "${ENV_VARS[@]}"; do
      unset "${var_to_unset%=*}"
    done
  fi
}

# -------------------------------------------------------------------------------------------------
# Test Suites
# -------------------------------------------------------------------------------------------------

testcases_array=(
  env_vars
  custom_ysql_port
  custom_ycql_port
  base_dir
  default_ysql_authentication
  default_ycql_authentication
)

basic_testcases=(
  "default"
  "env_vars,custom_ysql_port,custom_ycql_port,base_dir,default_ysql_authentication,default_ycql_authentication"
)

intermediate_testcases=(
  "default"
  "env_vars"
  "env_vars,custom_ysql_port,custom_ycql_port"
  "env_vars,base_dir"
  "env_vars,default_ysql_authentication,default_ycql_authentication"
  "base_dir"
  "base_dir,custom_ysql_port,custom_ycql_port"
  "base_dir,default_ysql_authentication,default_ycql_authentication"
  "default_ysql_authentication,default_ycql_authentication"
  "custom_ysql_port,custom_ycql_port"
)

declare -a advanced_testcases

basic() {
  local combination=
  local basic_case=

  for combination in "${basic_testcases[@]}"; do
    IFS=',' read -r -a basic_case <<< "$combination"
    parse_test_case "${basic_case[@]}"
  done
}

intermediate() {
  local combination=
  local intermediate_case=

  for combination in "${intermediate_testcases[@]}"; do
    IFS=',' read -r -a intermediate_case <<< "$combination"
    parse_test_case "${intermediate_case[@]}"
  done
}

advanced() {
  local outer_loop=
  local inner_loop=
  local combination=
  local advanced_case=

  for outer_loop in "${!testcases_array[@]}"; do
    inner_loop=1
    while (( outer_loop+inner_loop < "${#testcases_array[@]}"+1 )); do
      #testname="${testcases_array[@]:$outer_loop:$inner_loop}"
      testname="${testcases_array[*]:$outer_loop:$inner_loop}"
      advanced_testcases+=("${testname// /,}")
      inner_loop=$(( inner_loop+1 ))
    done
  done

  for combination in "${advanced_testcases[@]}"; do
    IFS=',' read -r -a advanced_case <<< "$combination"
    parse_test_case "${advanced_case[@]}"
  done
}

# -------------------------------------------------------------------------------------------------
# Test cases verfication checks
# -------------------------------------------------------------------------------------------------

verify_ysqlsh() {
  log_heading "Running YSQLSH Check: verify_ysqlsh"

  local ysql_ip="${DEFAULT_LISTEN_ADDR}"
  local ysql_port="${DEFAULT_YSQL_PORT}"
  local ysql_user="${DEFAULT_YSQL_USER}"
  local ysql_password="${DEFAULT_YSQL_PASSWORD}"
  local ysql_db="${DEFAULT_YSQL_DB}"
  local ysql_enable_auth=$DEFAULT_YSQL_ENABLE_AUTH

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --ip) shift; ysql_ip=$1 ;;
      --port) shift; ysql_port=$1 ;;
      --user) shift; ysql_user=$1 ;;
      --password) shift; ysql_password=$1 ;;
      --db) shift; ysql_db=$1 ;;
      --auth) shift; ysql_enable_auth=$1 ;;
    esac
    shift
  done

  is_up "$ysql_ip" "$ysql_port" "YSQL"

  local ysqlsh_cmd=

  if [[ "${DOCKER_ENV}" == "true" ]]; then
    ysqlsh_cmd=( docker exec -e PGPASSWORD="$ysql_password" "${CONTAINER_ID}" bin/ysqlsh -h "$ysql_ip" -p "$ysql_port" -U "$ysql_user" -d "$ysql_db")
  else
    ysqlsh_cmd=( "${EXTRACTED_YB_TAR}"/bin/ysqlsh -h "$ysql_ip" -p "$ysql_port" -U "$ysql_user" -d "$ysql_db")
  fi

  if "$ysql_enable_auth" && ! [[ "${DOCKER_ENV}" == "true" ]]; then
    export PGPASSWORD="$ysql_password"
  fi

  local table_name="mytable$RANDOM"
  log "Creating a YSQL table and inserting a bit of data"
  "${ysqlsh_cmd[@]}" \
  "-c CREATE SCHEMA IF NOT EXISTS ${ysql_db};" \
  "-c CREATE TABLE IF NOT EXISTS ${ysql_db}.${table_name} (k int PRIMARY KEY, v text);" \
  "-c INSERT INTO ${ysql_db}.${table_name} (k, v) VALUES (10, 'somevalue'), (20, 'someothervalue');"

  log "Running a simple select on YSQL table"
  "${ysqlsh_cmd[@]}" "-c select * from ${ysql_db}.${table_name} where k = 10;" | grep "somevalue"
  "${ysqlsh_cmd[@]}" "-c drop table ${ysql_db}.${table_name};"
}

verify_ycqlsh() {
  log_heading "Running YCQLSH Check: verify_ycqlsh"

  local ycql_ip="${DEFAULT_LISTEN_ADDR}"
  local ycql_port="${DEFAULT_YCQL_PORT}"
  local ycql_user="${DEFAULT_YCQL_USER}"
  local ycql_password="${DEFAULT_YCQL_PASSWORD}"
  local ycql_keyspace="${DEFAULT_YCQL_KEYSPACE}"
  local use_cassandra_authentication=$DEFAULT_USE_CASSANDRA_AUTHENTICATION

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --ip) shift; ycql_ip=$1 ;;
      --port) shift; ycql_port=$1 ;;
      --user) shift; ycql_user=$1 ;;
      --password) shift; ycql_password=$1 ;;
      --keyspace) shift; ycql_keyspace=$1 ;;
      --auth) shift; use_cassandra_authentication=$1 ;;
    esac
    shift
  done

  is_up "$ycql_ip" "$ycql_port" "YCQL"

  local ycqlsh_cmd=

  if [[ "${DOCKER_ENV}" == "true" ]]; then
    ycqlsh_cmd=( docker exec "${CONTAINER_ID}" bin/ycqlsh "$ycql_ip" "$ycql_port" )
  else
    ycqlsh_cmd=( "${EXTRACTED_YB_TAR}"/bin/ycqlsh "$ycql_ip" "$ycql_port" )
  fi

  if "$use_cassandra_authentication"; then
    ycqlsh_cmd+=( -u "${ycql_user}" -p "${ycql_password}" )
  fi

  local table_name="stock_market$RANDOM"
  log "Creating a YCQL keyspace and inserting a bit of data"

  "${ycqlsh_cmd[@]}" "-e \
CREATE KEYSPACE IF NOT EXISTS ${ycql_keyspace}; \
CREATE TABLE IF NOT EXISTS ${ycql_keyspace}.${table_name} (tick text, ts text, price float, PRIMARY KEY (tick, ts)); \
INSERT INTO ${ycql_keyspace}.${table_name} (tick,ts,price) VALUES ('AA','2017-10-26 09:00:00',15.4); \
INSERT INTO ${ycql_keyspace}.${table_name} (tick,ts,price) VALUES ('AA','2017-10-26 10:00:00',15); \
INSERT INTO ${ycql_keyspace}.${table_name} (tick,ts,price) VALUES ('FB','2017-10-26 09:00:00',17.6); \
INSERT INTO ${ycql_keyspace}.${table_name} (tick,ts,price) VALUES ('FB','2017-10-26 10:00:00',17.1);"

  log "Running a simple select on YCQL table"
  "${ycqlsh_cmd[@]}" "-e SELECT * FROM ${ycql_keyspace}.${table_name} WHERE tick = 'AA';" | grep "AA"
}

is_up() {
  local attempts=0
  local ip=$1
  local port=$2
  local program=$3

  log "Waiting for $program to listen on port $ip:$port"

  while ! nc -z "$ip" "$port"; do
    if [[ $attempts -gt 600 ]]; then
      fatal "Timed out waiting for $program on $ip:$port after $(( attempts / 10 )) sec"
    fi
    sleep 0.1
    (( attempts+=1 ))
  done

  log "$program listening on port $ip:$port"

  # Give the program a chance to start up, or we'll get an error:
  # FATAL:  the database system is starting up
  sleep 20
}

# -------------------------------------------------------------------------------------------------
# Parsing test arguments
# -------------------------------------------------------------------------------------------------

python_interpreter=python
package=
yugabyted=
docker_image=
master_flags=
tserver_flags=
testsuite="basic"
declare -a testcase

thick_log_heading "Parameters"

is_empty() {
  if [[ -z "$1" ]]; then
    fatal "$2 is empty."
  fi
}

is_exist() {
  if  [ ! -f "$1" ]; then
    fatal "Error: Provided $2 doesn't exists."
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      print_usage
      exit 0
    ;;
    -p|--package)
      shift
      is_empty "${1-}" "Package"
      if ! [[ "$1" == "https://"* ]]; then
        is_exist "$1" "Package"
      fi
      package=$1
      log "YugabyteDB Package: ${package}"
    ;;
    -i|--image)
      shift
      is_empty "${1-}" "Docker image"
      docker_image=$1
      log "YugabyteDB Docker Image: ${docker_image}"
    ;;
    -y|--yugabyted)
      shift
      is_empty "${1-}" "Yugabyted"
      is_exist "$1" "Yugabyted"
      yugabyted=$1
      log "Custom YugabyteD: ${yugabyted}"
    ;;
    -P|--python)
      shift
      is_empty "${1-}" "Python interpreter"
      log "Checking... Python Interpreter"
      if ! $1 --version; then
        fatal "Python Interpreter not found"
      fi
      python_interpreter=$1
      log "Python Interpreter: ${python_interpreter}"
    ;;
    -T|--testsuite)
      shift
      is_empty "${1-}" "Test suite"
      if ! [[ "basic intermediate advanced" == *"${1}"* ]]; then
        fatal "Provide valid test suite"
      fi
      testsuite=$1
    ;;
    -t|--testcase)
      shift
      is_empty "${1-}" "Test case"
      IFS=',' read -r -a testcase <<< "$1"
      log "Testcases: ${testcase[*]}"
    ;;
    --master_flags)
      shift
      is_empty "${1-}" "Master Flags"
      master_flags=$1
    ;;
    --tserver_flags)
      shift
      is_empty "${1-}" "Tserver Flags"
      tserver_flags=$1
    ;;
    *)
      print_usage >&2
      echo >&2
      echo "Invalid option: $1" >&2
      exit 1
  esac
  shift
done

# Check Mandatory arguments
if [[ -z "${package}" ]] && [[ -z "${docker_image}" ]]; then
  print_usage >&2
  fatal "Please provide either package or docker image"
fi

# -------------------------------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------------------------------

trap cleanup EXIT

run_test_cases() {
  if [[ -n "${testcase[*]-}" ]]; then
    parse_test_case "${testcase[@]}"
  else
    "${testsuite}"
  fi
}

# Non Docker
if [[ -n "$package" ]]; then
  setup_test_env
  run_test_cases
fi

# Docker
if [[ -n "$docker_image" ]]; then
  setup_docker_test_env
  run_test_cases
fi
