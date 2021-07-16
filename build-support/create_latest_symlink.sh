#!/usr/bin/env bash

if [[ $# -ne 2 ]]; then
  echo >&2 "${0##*/}: create the 'latest' symlink"
  echo >&2 "Arguments: CMAKE_CURRENT_BINARY_DIR LATEST_BUILD_SYMLINK_PATH"
  exit 1
fi

if [[ ${YB_DISABLE_LATEST_SYMLINK:-0} != "1" ]]; then
  if [[ $OSTYPE == darwin* ]]; then
    # -h    If the target_file or target_dir is a symbolic link, do not follow it.  This is most
    #       useful with the -f option, to replace a symlink which may point to a directory.
    ln_args=-h
  else
    # -T, --no-target-directory
    #       treat LINK_NAME as a normal file always
    ln_args=-T
  fi
  ( set -x; /bin/ln $ln_args -sf "$@" )
fi
