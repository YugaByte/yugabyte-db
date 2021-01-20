#!/usr/bin/env bash

set -euo pipefail

# -------------------------------------------------------------------------------------------------
# Variables
# -------------------------------------------------------------------------------------------------

readonly absolute_dir_path="$(dirname "$(readlink -f "$0")")"
readonly test_script="${absolute_dir_path}/yugabyted-test.sh"
readonly logfile="/tmp/yugabyted-test-runner-$( date +%Y-%m-%dT%H_%M_%S ).log"

python_interpreter=python
yb_latest_version="$(curl --silent "https://api.github.com/repos/yugabyte/yugabyte-db/tags" \
  | jq '.[].name' | head -1)"
docker_image="yugabytedb/yugabyte:latest"
testsuite="basic"
yugabyted=

declare -a test_args

if [[ $OSTYPE == linux* ]]; then
  package="https://github.com/yugabyte/yugabyte-db/releases/download/${yb_latest_version//\"/}/yugabyte-${yb_latest_version//[v\"]/}-linux.tar.gz"
fi

if [[ $OSTYPE == darwin* ]]; then
  package="https://github.com/yugabyte/yugabyte-db/releases/download/${yb_latest_version//\"/}/yugabyte-${yb_latest_version//[v\"]/}-darwin.tar.gz"
fi

# -------------------------------------------------------------------------------------------------
# Helper functions
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
  -h, --help
    Print usage information.
  -p, --package
    YugabyteDB package.
  -i, --image
    YugabyteDB Docker image with tag.
  -y, --yugabyted
    Custom Yugabyted.
  -P, --python
    Provide Python interpreter.
  -T, --testsuite
    Provide Test Suite. Default: basic
    [basic|intermediate|advanced]

  Examples:
    1. With defaults.
      yugabyted-test-runner.sh

    2. With Custom yugabyted.
      yugabyted-test-runner.sh -y /path/yugabyted

    3. With Custom yugabyted, docker image and package.
      yugabyted-test-runner.sh -y /path/yugabyted -i image:tag -p /path/package.tar.gz

    4. With different python interpreter.
      yugabyted-test-runner.sh -P python37

    5. With different test suite
      yugabyted-test-runner.sh -T advanced
EOT
}

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

# -------------------------------------------------------------------------------------------------
# Parsing arguments
# -------------------------------------------------------------------------------------------------

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
    ;;
    -i|--image)
      shift
      is_empty "${1-}" "Docker image"
      docker_image=$1
    ;;
    -y|--yugabyted)
      shift
      is_empty "${1-}" "Yugabyted"
      is_exist "$1" "Yugabyted"
      yugabyted=$1
    ;;
    -P|--python)
      shift
      is_empty "${1-}" "Python interpreter"
      if ! $1 --version; then
        fatal "Python Interpreter not found"
      fi
      python_interpreter=$1
    ;;
    -T|--testsuite)
      shift
      is_empty "${1-}" "Test suite"
      if ! [[ "basic intermediate advanced" == *"${1}"* ]]; then
        fatal "Provide valid test suite"
      fi
      testsuite=$1
    ;;
    *)
      print_usage >&2
      echo >&2
      echo "Invalid option: $1" >&2
      exit 1
  esac
  shift
done

# -------------------------------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------------------------------

test_args+=(
  -p "$package"
  -i "$docker_image"
  -P "$python_interpreter"
  -T "$testsuite"
)

if [[ -n "$yugabyted" ]]; then
  test_args+=(-y "$yugabyted")
fi

log "Test log at ${logfile}"

time "$test_script" "${test_args[@]}" | tee "${logfile}"
