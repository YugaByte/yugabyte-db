#!/bin/bash
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

. "${0%/*}/common.sh"
should_create_package="0"
should_use_package="0"
show_usage() {
  cat <<-EOT
Usage: ${0##*/} [<options>]
Options:
  --no-virtual-env
    Do not use a virtualenv. Instead, use the --user flag when installing modules. Note that if a
    virtualenv is already activated outside of this script, it will still be used.
  --create_package
    Create a portable python virtualenv package after installing python virtualenv.
  -h, --help
    Show usage
EOT
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --no-virtual-env)
      export YB_NO_VIRTUAL_ENV=1
    ;;
    -h|--help)
      show_help >&2
      exit 1
    ;;
    --create_package)
      should_create_package="1"
    ;;
    --use_package)
      should_use_package=1
    ;;
    *)
      fatal "Invalid option: $1"
  esac
  shift
done

if should_use_virtual_env; then
  activate_virtualenv
  fix_virtualenv_permissions
fi

if [[ $should_use_package == "1" && -f "$virtualenv_package" ]]; then
  log "Found virtualenv package $virtualenv_package"
  tar -xf $virtualenv_package
else
  if [[ $YB_MANAGED_DEVOPS_USE_PYTHON3 == "0" ]]; then
    # looks like there is some issue with setuptools and virtualenv on python2.
    # https://github.com/pypa/virtualenv/issues/1493, adding this requirement
    pip_install "setuptools<45"
  fi

  pip_install -r "$FROZEN_REQUIREMENTS_FILE"
  log "Installing ybops package"
  install_ybops_package

  log "Changing virtualenv absolute paths to dynamic paths"
  # Change shebangs to use local python instead of absolute python path - required for our Jenkins
  # pipeline and packaging (e.g. "/tmp/python_virtual_env/bin/python" -> "/usr/bin/env python")
  LC_ALL=C find $virtualenv_dir ! -name '*.pyc' -type f -exec sed -i.yb_tmp \
    -e "1s|${virtualenv_dir}/bin/python|/usr/bin/env python|" {} \; -exec rm {}.yb_tmp \;

  # Change VIRTUAL_ENV variable to be dynamic. Instead of hardcoding the path to the vitualenv
  # directory, the VIRTUAL_ENV variable should print the filepath of the directory two above it.
  new_venv_assignment='VIRTUAL_ENV="$(cd "$(dirname "$(dirname "${BASH_SOURCE[0]}" )")" \&\& pwd -P)"'
  sed -i.yb_tmp \
    -e "s|VIRTUAL_ENV=\"${virtualenv_dir}\"|${new_venv_assignment}|" $virtualenv_dir/bin/activate
  rm $virtualenv_dir/bin/activate.yb_tmp

  if should_use_virtual_env; then
    log "Expecting there to be no differences between the output of 'pip freeze' and the contents" \
        "of $FROZEN_REQUIREMENTS_FILE"
    if grep -Fxvf <(run_pip freeze | grep -v ybops) "$FROZEN_REQUIREMENTS_FILE" >/dev/null; then
      log_warn "WARNING: discrepancies found between the contents of '$FROZEN_REQUIREMENTS_FILE'" \
              "and what's installed in the virtualenv $virtualenv_dir."
      log_error "Showing full diff output, but please ignore extra modules that were installed."
      # Use "LANG=C" to enforce case-sensitive sorting.
      # https://stackoverflow.com/questions/10326933/case-sensitive-sort-unix-bash
      diff <( LANG=C sort <"$FROZEN_REQUIREMENTS_FILE" ) \
          <(run_pip freeze | grep -v ybops | LANG=C sort)
      exit 1
    else
      log "Successfully validated the set of installed Python modules in the virtualenv" \
          "$virtualenv_dir."
    fi
  fi
fi

if [[ $should_create_package == "1" ]]; then
  log "Creating virtualenv package $virtualenv_package"
  create_virtualenv_package
fi
