#!/bin/bash
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

set -euo pipefail

. "${BASH_SOURCE%/*}"/common.sh

if [[ ! ${1:-} =~ ^(-y|--yes)$ ]]; then
  echo >&2 "This will remove and re-create the entire virtualenv from ${virtualenv_dir} in order"
  echo >&2 "to re-generate 'frozen' python dependency versions. It is only necessary to run this"
  echo >&2 "script once in a while, when we want to upgrade versions of some third-party Python"
  echo >&2 "modules."
  echo >&2
  echo >&2 "The frozen Python requirements file will be generated at: $FROZEN_REQUIREMENTS_FILE"
  echo >&2 -n "Continue? [y/N] "

  read confirmation

  if [[ ! $confirmation =~ ^(y|Y|yes|YES)$ ]]; then
    log "Operation canceled."
    exit 1
  fi
fi

delete_virtualenv
activate_virtualenv

if [[ $YB_MANAGED_DEVOPS_USE_PYTHON3 == "0" ]]; then
  # looks like there is some issue with setuptools and virtualenv on python2.
  # https://github.com/pypa/virtualenv/issues/1493, adding this requirement
  pip_install "setuptools<45"
fi

cd "$yb_devops_home"

log "There should be no pre-installed Python modules in the virtualenv"
( set -x; run_pip list )

log "Installing Python modules according to the ${REQUIREMENTS_FILE_NAME} file"
( set -x; run_pip install -r "${REQUIREMENTS_FILE_NAME}" )

log "Generating $FROZEN_REQUIREMENTS_FILE"

# Use LANG=C to force case-sensitive sorting.
# https://stackoverflow.com/questions/10326933/case-sensitive-sort-unix-bash
( set -x; run_pip freeze | LANG=C sort >"$FROZEN_REQUIREMENTS_FILE" )

log_empty_line
log "Contents of $FROZEN_REQUIREMENTS_FILE:"
cat "$FROZEN_REQUIREMENTS_FILE"

# This will validate that exactly the right set of packages is installed, and install ybops.
set -x
"$yb_devops_home/bin/install_python_requirements.sh"
