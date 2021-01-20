#!/usr/bin/env bash
#
# Copyright 2020 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

set -euo pipefail

SCRIPT_NAME=$(basename "$0")
USER=$(whoami)
PLATFORM_DUMP_FNAME="platform_dump.sql"
PLATFORM_DB_NAME="yugaware"
PROMETHEUS_SNAPSHOT_DIR="prometheus_snapshot"
# This is the UID for nobody user which is used by the prometheus container as the default user.
NOBODY_UID=65534

set +e
# Check whether the script is being run from a VM running replicated-based Yugabyte Platform.
docker ps -a 2> /dev/null | grep yugabyte-yugaware > /dev/null 2>&1
DOCKER_CHECK="$?"

if [[ $DOCKER_CHECK -eq 0 ]]; then
  DOCKER_BASED=true
else
  DOCKER_BASED=false
fi


# Check whether the script is being run from within a Yugabyte Platform docker container.
grep -E 'kubepods|docker' /proc/1/cgroup > /dev/null 2>&1
CONTAINER_CHECK="$?"

if [[ $CONTAINER_CHECK -eq 0 ]] && [[ "$DOCKER_BASED" = false ]]; then
  INSIDE_CONTAINER=true
else
  INSIDE_CONTAINER=false
fi
set -e

# Assume the script is being run from a systemctl-based Yugabyte Platform installation otherwise.
if [[ "$DOCKER_BASED" = false ]] && [[ "$INSIDE_CONTAINER" = false ]]; then
  SERVICE_BASED=true
else
  SERVICE_BASED=false
fi

# Takes docker container and command as arguments. Executes docker cmd if docker-based or not.
docker_aware_cmd() {
  if [[ "$DOCKER_BASED" = false ]]; then
    $2
  else
    docker exec -i "${1}" $2
  fi
}

run_sudo_cmd() {
  if [[ "${USER}" = "root" ]]; then
    $1
  else
    sudo $1
  fi
}

# Query prometheus for it's data directory and set as env var
set_prometheus_data_dir() {
  prometheus_host="$1"
  data_dir="$2"
  if [[ "$DOCKER_BASED" = true ]]; then
    PROMETHEUS_DATA_DIR="${data_dir}/prometheusv2"
  else
    PROMETHEUS_DATA_DIR=$(curl "http://${prometheus_host}:9090/api/v1/status/flags" |
    python -c "import sys, json; print(json.load(sys.stdin)['data']['storage.tsdb.path'])")
  fi
  if [[ -z "$PROMETHEUS_DATA_DIR" ]]; then
    echo "Failed to find prometheus data directory"
    exit 1
  fi
}

# Modify service status if the script is being run against a service-based Yugabyte Platform
modify_service() {
  if [[ "$SERVICE_BASED" = true ]]; then
    set +e
    service="$1"
    operation="$2"
    echo "Performing operation $operation on service $service"
    run_sudo_cmd "systemctl ${operation} ${service}"
    set -e
  fi
}

# Creates a Yugabyte Platform DB backup.
create_postgres_backup() {
  backup_path="$1"
  db_username="$2"
  db_host="$3"
  db_port="$4"
  verbose="$5"
  if [[ "${verbose}" = true ]]; then
    backup_cmd="pg_dump -h ${db_host} -p ${db_port} -U ${db_username} -Fc -v ${PLATFORM_DB_NAME}"
  else
    backup_cmd="pg_dump -h ${db_host} -p ${db_port} -U ${db_username} -Fc ${PLATFORM_DB_NAME}"
  fi
  # Run pg_dump.
  echo "Creating Yugabyte Platform DB backup ${backup_path}..."
  docker_aware_cmd "postgres" "${backup_cmd}" > "${backup_path}"
  echo "Done"
}

# Restores a Yugabyte Platform DB backup.
restore_postgres_backup() {
  backup_path="$1"
  db_username="$2"
  db_host="$3"
  db_port="$4"
  verbose="$5"
  if [[ "${verbose}" = true ]]; then
    restore_cmd="pg_restore -h ${db_host} -p ${db_port} -U ${db_username} -c -v -d \
    ${PLATFORM_DB_NAME}"
  else
    restore_cmd="pg_restore -h ${db_host} -p ${db_port} -U ${db_username} -c -d ${PLATFORM_DB_NAME}"
  fi
  # Run pg_restore.
  echo "Restoring Yugabyte Platform DB backup ${backup_path}..."
  docker_aware_cmd "postgres" "${restore_cmd}" < "${backup_path}"
  echo "Done"
}

# Deletes a Yugabyte Platform DB backup.
delete_postgres_backup() {
  backup_path="$1"
  echo "Deleting Yugabyte Platform DB backup ${backup_path}..."
  if [[ -f "${backup_path}" ]]; then
    cleanup "${backup_path}"
    echo "Done"
  else
    echo "${backup_path} does not exist. Cannot delete"
    exit 1
  fi
}

create_backup() {
  now=$(date +"%y-%m-%d-%H-%M")
  output_path="$1"
  data_dir="$2"
  exclude_prometheus="$3"
  exclude_releases="$4"
  db_username="$5"
  db_host="$6"
  db_port="$7"
  verbose="$8"
  prometheus_host="$9"
  exclude_releases_flag=""

  if [[ "$exclude_releases" = true ]]; then
    exclude_releases_flag="--exclude release*"
  fi

  exclude_dirs="--exclude postgresql --exclude devops --exclude yugaware/lib \
  --exclude yugaware/logs --exclude yugaware/README.md --exclude yugaware/bin \
  --exclude yugaware/conf --exclude backup_*.tgz --exclude helm"

  modify_service yb-platform stop

  mkdir -p "${output_path}"
  tar_name="${output_path}/backup_${now}.tgz"
  db_backup_path="${data_dir}/${PLATFORM_DUMP_FNAME}"
  trap 'delete_postgres_backup ${db_backup_path}' RETURN
  create_postgres_backup "${db_backup_path}" "${db_username}" "${db_host}" "${db_port}" "${verbose}"

  # Backup prometheus data.
  if [[ "$exclude_prometheus" = false ]]; then
    echo "Creating prometheus snapshot..."
    set_prometheus_data_dir "${prometheus_host}" "${data_dir}"
    snapshot_dir=$(curl -X POST "http://${prometheus_host}:9090/api/v1/admin/tsdb/snapshot" |
      python -c "import sys, json; print(json.load(sys.stdin)['data']['name'])")
    mkdir -p "$data_dir/$PROMETHEUS_SNAPSHOT_DIR"
    run_sudo_cmd "cp -aR ${PROMETHEUS_DATA_DIR}/snapshots/${snapshot_dir} \
    ${data_dir}/${PROMETHEUS_SNAPSHOT_DIR}"
    run_sudo_cmd "rm -rf ${PROMETHEUS_DATA_DIR}/snapshots/${snapshot_dir}"
  fi
  echo "Creating platform backup package..."
  if [[ "${verbose}" = true ]]; then
    tar ${exclude_releases_flag} ${exclude_dirs} -czvf "${tar_name}" -C "${data_dir}" .
  else
    tar ${exclude_releases_flag} ${exclude_dirs} -czf "${tar_name}" -C "${data_dir}" .
  fi

  echo "Finished creating backup ${tar_name}"
  modify_service yb-platform restart
}

restore_backup() {
  input_path="$1"
  destination="$2"
  db_host="$3"
  db_port="$4"
  db_username="$5"
  verbose="$6"
  prometheus_host="$7"
  data_dir="$8"
  prometheus_dir_regex="^${PROMETHEUS_SNAPSHOT_DIR}/$"

  modify_service yb-platform stop

  db_backup_path="${destination}/${PLATFORM_DUMP_FNAME}"
  trap 'delete_postgres_backup ${db_backup_path}' RETURN
  if [[ "${verbose}" = true ]]; then
    tar -xzvf "${input_path}" --directory "${destination}"
  else
    tar -xzf "${input_path}" --directory "${destination}"
  fi

  restore_postgres_backup "${db_backup_path}" "${db_username}" "${db_host}" "${db_port}" \
  "${verbose}"
  # Restore prometheus data.
  if tar -tf "${input_path}" | grep $prometheus_dir_regex; then
    echo "Restoring prometheus snapshot..."
    set_prometheus_data_dir "${prometheus_host}" "${data_dir}"
    modify_service prometheus stop
    trap 'modify_service prometheus restart' RETURN
    run_sudo_cmd "rm -rf ${PROMETHEUS_DATA_DIR}/*"
    run_sudo_cmd "mv ${destination}/${PROMETHEUS_SNAPSHOT_DIR}/* ${PROMETHEUS_DATA_DIR}"
    if [[ "$SERVICE_BASED" = true ]]; then
      run_sudo_cmd "chown -R prometheus:prometheus ${PROMETHEUS_DATA_DIR}"
    else
      run_sudo_cmd "chown -R ${NOBODY_UID}:${NOBODY_UID} ${PROMETHEUS_DATA_DIR}"
    fi
  fi
  # Create following directory if it wasn't created yet so restore will succeed.
  mkdir -p "${destination}/release"

  modify_service yb-platform restart

  echo "Finished restoring backup"
}

print_backup_usage() {
  echo "Create: ${SCRIPT_NAME} create [options]"
  echo "options:"
  echo "  -o, --output                   the directory that the platform backup is written to (default: ${HOME})"
  echo "  -m, --exclude_prometheus       exclude prometheus metric data from backup (default: false)"
  echo "  -r, --exclude_releases         exclude Yugabyte releases from backup (default: false)"
  echo "  -d, --data_dir=DIRECTORY       data directory (default: /opt/yugabyte)"
  echo "  -v, --verbose                  verbose output of script (default: false)"
  echo "  -u, --db_username=USERNAME     postgres username (default: postgres)"
  echo "  -h, --db_host=HOST             postgres host (default: localhost)"
  echo "  -P, --db_port=PORT             postgres port (default: 5432)"
  echo "  -n, --prometheus_host=HOST     prometheus host (default: localhost)"
  echo "  -?, --help                     show create help, then exit"
  echo
}

print_restore_usage() {
  echo "Restore: ${SCRIPT_NAME} restore --input <input_path> [options]"
  echo "<input_path> the path to the platform backup tar.gz"
  echo "options:"
  echo "  -o, --destination=DIRECTORY    where to un-tar the backup (default: /opt/yugabyte)"
  echo "  -d, --data_dir=DIRECTORY       data directory (default: /opt/yugabyte)"
  echo "  -v, --verbose                  verbose output of script (default: false)"
  echo "  -u, --db_username=USERNAME     postgres username (default: postgres)"
  echo "  -h, --db_host=HOST             postgres host (default: localhost)"
  echo "  -P, --db_port=PORT             postgres port (default: 5432)"
  echo "  -n, --prometheus_host=HOST     prometheus host (default: localhost)"
  echo "  -?, --help                     show restore help, then exit"
  echo
}

print_help() {
  echo "Create or restore a Yugabyte Platform backup"
  echo
  echo "Usage: ${SCRIPT_NAME} <command>"
  echo "command:"
  echo "  create                         create a Yugabyte Platform backup"
  echo "  restore                        restore a Yugabyte Platform backup"
  echo "  -?, --help                     show this help, then exit"
  echo
  print_backup_usage
  print_restore_usage
}

cleanup () {
  rm -f "$1"
}

if [[ $# -eq 0 ]]; then
  print_help
  exit 1
fi

command=$1
shift

# Default global options.
db_username=postgres
db_host=localhost
db_port=5432
prometheus_host=localhost
data_dir=/opt/yugabyte
verbose=false

case $command in
  -?|--help)
    print_help
    exit 0
    ;;
  create)
    # Default create options.
    exclude_prometheus=false
    exclude_releases=false
    output_path="${HOME}"

    if [[ $# -eq 0 ]]; then
      print_backup_usage
      exit 1
    fi

    while (( "$#" )); do
      case "$1" in
        -o|--output)
          output_path=$2
          shift 2
          ;;
        -m|--exclude_prometheus)
          exclude_prometheus=true
          shift
          ;;
        -r|--exclude_releases)
          exclude_releases=true
          shift
          ;;
        -d|--data_dir)
          data_dir=$2
          shift 2
          ;;
        -v|--verbose)
          verbose=true
          set -x
          shift
          ;;
        -u|--db_username)
          db_username=$2
          shift 2
          ;;
        -h|--db_host)
          db_host=$2
          shift 2
          ;;
        -P|--db_port)
          db_port=$2
          shift 2
          ;;
        -n|--prometheus_host)
          prometheus_host=$2
          shift 2
          ;;
        -?|--help)
          print_backup_usage
          exit 0
          ;;
        *)
          echo "${SCRIPT_NAME}: Unrecognized argument ${1}"
          echo
          print_backup_usage
          exit 1
      esac
    done

    create_backup "$output_path" "$data_dir" "$exclude_prometheus" "$exclude_releases" \
    "$db_username" "$db_host" "$db_port" "$verbose" "$prometheus_host"
    exit 0
    ;;
  restore)
    # Default restore options.
    destination=/opt/yugabyte
    input_path=""

    if [[ $# -eq 0 ]]; then
      print_restore_usage
      exit 1
    fi

    while (( "$#" )); do
      case "$1" in
        -i|--input)
          input_path=$2
          shift 2
          ;;
        -o|--destination)
          destination=$2
          shift 2
          ;;
        -d|--data_dir)
          data_dir=$2
          shift 2
          ;;
        -v|--verbose)
          verbose=true
          set -x
          shift
          ;;
        -u|--db_username)
          db_username=$2
          shift 2
          ;;
        -h|--db_host)
          db_host=$2
          shift 2
          ;;
        -P|--db_port)
          db_port=$2
          shift 2
          ;;
        -n|--prometheus_host)
          prometheus_host=$2
          shift 2
          ;;
        -?|--help)
          print_restore_usage
          exit 0
          ;;
        *)
          echo "${SCRIPT_NAME}: Unrecognized option ${1}"
          echo
          print_restore_usage
          exit 1
      esac
    done
    if [[ -z "$input_path" ]]; then
      echo "${SCRIPT_NAME}: input_path is required"
      echo
      print_restore_usage
      exit 1
    fi
    restore_backup "$input_path" "$destination" "$db_host" "$db_port" "$db_username" "$verbose" \
    "$prometheus_host" "$data_dir"
    exit 0
    ;;
  *)
    echo "${SCRIPT_NAME}: Unrecognized command ${command}"
    echo
    print_help
    exit 1
esac
