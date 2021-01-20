#!/usr/bin/env bash

# Post-installation script. Set dynamic linker path on executables in the "bin" directory. This
# script is expected to be installed into the "bin" directory of the YugaByte distribution.

#
# The following only applies to changes made to this file as part of YugaByte development.
#
# Portions Copyright (c) YugaByte, Inc.
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

patch_binary() {
  if [[ $# -ne 1 ]]; then
    echo >&2 "patch_binary expects exactly one argument, the binary name to patch"
    exit 1
  fi
  local f=$1
  (
    set -x;
    "$patchelf_path" --set-interpreter "$ld_path" "$f";
    "$patchelf_path" --set-rpath "$rpath" "$f";
  )
}

# Use pwd -P to resolve any symlinks on the path. Otherwise the find command at the bottom of this
# script will not actually find any files.
bin_dir=$( cd "${BASH_SOURCE%/*}" && pwd -P )
distribution_dir=$( cd "$bin_dir/.." && pwd -P )

lib_dir="$distribution_dir/lib"
lib_pg_dir="$distribution_dir/postgres/lib"
lib_tp_dir="$lib_dir/yb-thirdparty"
linuxbrew_dir="$distribution_dir/linuxbrew"
rpath="$lib_dir/yb:$lib_tp_dir:$linuxbrew_dir/lib"
patchelf_path=$bin_dir/patchelf
script_name=$(basename "$0")
completion_file="$distribution_dir/.${script_name}.completed"

supported_extensions="postgis"
declare -A libfiles
libfiles["postgis"]="$(ls $lib_pg_dir/*postgis*.so 2>/dev/null || exit 0)"

if [[ "${1:-}" == "-e" ]]; then
  ext_mode="true"
else
  ext_mode="false"
fi


if [[ -f $completion_file ]]; then
  echo "${script_name} was already run (marker at ${completion_file})"
  if [[ $ext_mode == "true" ]]; then
    install_mode="false"
  else
    exit 0
  fi
else
  install_mode="true"
fi

if [[ ! -x $patchelf_path ]]; then
  echo >&2 "patchelf not found or is not executable: '$patchelf_path'"
  exit 1
fi

ld_path=$distribution_dir/lib/ld.so

if [[ ! -x $ld_path ]]; then
  echo >&2 "Dynamic linker not found or is not executable: '$ld_path'"
  exit 1
fi

if [[ $ext_mode == "true" ]]; then
  if [[ -z "$(which ldd 2>/dev/null)" ]]; then
    echo >&2 "ldd command not found."
    exit 1
  fi
fi

if [[ $install_mode == "true" ]]; then
  cd "$bin_dir"
  # ${...} macro variables will be substituted during packaging.
  # If you are looking at the actual post_install.sh script in an installed distribution of
  # YugabyteDB, you won't see the ${...} macro variables because they have been replaced with their
  # actual values.
  for f in ${main_elf_names_to_patch}; do
    patch_binary "$f"
  done

  cd "$bin_dir/../postgres/bin"
  for f in ${postgres_elf_names_to_patch}; do
    patch_binary "$f"
  done
fi

if [[ $ext_mode == "true" ]]; then
  # Pull in extension shared lib dependencies
  for extension in $supported_extensions; do
    if [[ -z "${libfiles[$extension]}" ]]; then
      echo "No shared libraries found for $extension in ${lib_pg_dir}."
      echo "Skipping ${extension}."
    else
      echo "Installing $extension extension."
      for lib in ${libfiles[$extension]}; do
        "$patchelf_path" --set-rpath "$rpath" $lib
      done
      ldd ${libfiles[$extension]} 2>/dev/null | awk '/=>/ {print $1,$3}' | while read file loc; do
        [[ "${loc:-}" =~ ^/ ]] || continue
        yb_loc="$(ls $linuxbrew_dir/lib/$file $lib_dir/yb/$file $lib_tp_dir/$file \
                  2>/dev/null || exit 0)"
        if [[ -z "$yb_loc" ]]; then
          echo "Installing dependency: $file"
          cp -f $loc $lib_tp_dir/
        fi
      done
    fi
  done
fi

# We are filtering out warning from stderr which are produced by this bug:
# https://github.com/NixOS/patchelf/commit/c4deb5e9e1ce9c98a48e0d5bb37d87739b8cfee4
# This bug is harmless, it only could unnecessarily increase file size when patching.
find "$lib_dir" "$linuxbrew_dir" -name "*.so*" ! -name "ld.so*" -exec "$patchelf_path" \
  --set-rpath "$rpath" {} 2> \
  >(grep -v 'warning: working around a Linux kernel bug by creating a hole' >&2) \;


if [[ $install_mode == "true" ]]; then
  ORIG_BREW_HOME=${original_linuxbrew_path_to_patch}
  ORIG_LEN=${original_linuxbrew_path_length}

  # Take $ORIG_LEN number of '\0' from /dev/zero, replace '\0' with 'x', then prepend to
  # "$distribution_dir/linuxbrew-" and keep first $ORIG_LEN symbols, so we have a path of $ORIG_LEN
  # length.
  BREW_HOME=$(echo "$distribution_dir/linuxbrew-$(head -c "$ORIG_LEN" </dev/zero | tr '\0' x)" | \
    cut -c-"$ORIG_LEN")
  LEN=${#BREW_HOME}
  if [[ "$LEN" != "$ORIG_LEN" ]]; then
   echo "Linuxbrew should be linked to a directory having absolute path length of $ORIG_LEN" \
        "bytes, but actual length is $LEN bytes."
   exit 1
  fi

  ln -sfT "$linuxbrew_dir" "$BREW_HOME"

  # We are relying on the fact that $distribution_dir is not a symlink. We don't want to add symlink
  # resolution to the find command because someone may accidentally add a symlink pointing to a
  # directory outside of the distribution directory, and we would recurse into that directory and
  # try to modify files there.
  find "$distribution_dir" \( \
     -type f -and \
     -not -path "$distribution_dir/var/*" -and \
     -not -name "post_install.sh" \
  \) -exec sed -i --binary "s%$ORIG_BREW_HOME%$BREW_HOME%g" {} \;

  touch "$completion_file"
fi
