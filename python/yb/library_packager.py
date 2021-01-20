#!/usr/bin/env python3

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

"""
Copyright (c) YugaByte, Inc.

Finds all Linux dynamic libraries that have to be packaged with the YugaByte distribution tarball by
starting from a small set of executables and walking the dependency graph. Creates a self-sufficient
distribution directory.

Run doctest tests as follows:

  python -m doctest python/yb/library_packager.py
"""


import argparse
import collections
import glob
import json
import logging
import os
import re
import shutil
import subprocess
import sys

from functools import total_ordering

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from yb.command_util import run_program, mkdir_p, copy_deep  # nopep8
from yb.linuxbrew import get_linuxbrew_dir  # nopep8
from yb.common_util import get_thirdparty_dir, YB_SRC_ROOT, sorted_grouped_by, \
                           safe_path_join  # nopep8

# A resolved shared library dependency shown by ldd.
# Example (split across two lines):
#   libmaster.so => /home/mbautin/code/yugabyte/build/debug-gcc-dynamic/lib/libmaster.so
#   (0x00007f941fa5f000)
RESOLVED_DEP_RE = re.compile(r'^\s*(\S+)\s+=>\s+(\S.*\S)\s+[(]')

SYSTEM_LIBRARY_PATH_RE = re.compile(r'^/(usr|lib|lib64)/.*')
SYSTEM_LIBRARY_PATHS = ['/usr/lib', '/usr/lib64', '/lib', '/lib64',
                        # This is used on Ubuntu
                        '/usr/lib/x86_64-linux-gnu',
                        '/lib/x86_64-linux-gnu']

HOME_DIR = os.path.expanduser('~')


ADDITIONAL_LIB_NAME_GLOBS = ['libnss_*', 'libresolv*', 'libthread_db*']

YB_SCRIPT_BIN_DIR = os.path.join(YB_SRC_ROOT, 'bin')
YB_BUILD_SUPPORT_DIR = os.path.join(YB_SRC_ROOT, 'build-support')

PATCHELF_NOT_AN_ELF_EXECUTABLE = 'not an ELF executable'

LIBRARY_PATH_RE = re.compile('^(.*[.]so)(?:$|[.].*$)')
LIBRARY_CATEGORIES = ['system', 'yb', 'yb-thirdparty', 'linuxbrew']

linuxbrew_home = None


# This is an alternative to global variables, bundling a few commonly used things.
DistributionContext = collections.namedtuple(
        'DistributionContext',
        ['build_dir',
         'dest_dir',
         'verbose_mode'])


@total_ordering
class Dependency:
    """
    Describes a dependency of an executable or a shared library on another shared library.
    @param name: the name of the library as requested by the original executable/shared library
    @param target: target file pointed to by the dependency
    """
    def __init__(self, name, target, origin, context):
        self.name = name
        self.target = target
        self.category = None
        self.context = context
        self.origin = origin

    def __hash__(self):
        return hash(self.name) ^ hash(self.target)

    def __eq__(self, other):
        return self.name == other.name and \
               self.target == other.target

    def __str__(self):
        return "Dependency(name='{}', target='{}', origin='{}')".format(
                self.name, self.target, self.origin)

    def __repr__(self):
        return str(self)

    def __lt__(self, other):
        return (self.name, self.target) < (other.name, other.target)

    def get_category(self):
        """
        Categorizes binaries into a few buckets:
        - yb -- YugaByte product itself
        - yb-thirdparty -- built with YugaByte
        - linuxbrew -- built using Linuxbrew
        - system -- grabbed from a system-wide library directory
        """
        if self.category:
            return self.category

        if linuxbrew_home.path_is_in_linuxbrew_dir(self.target):
            self.category = 'linuxbrew'
        elif self.target.startswith(get_thirdparty_dir() + '/'):
            self.category = 'yb-thirdparty'
        elif self.target.startswith(self.context.build_dir + '/'):
            self.category = 'yb'
        elif (self.target.startswith(YB_SCRIPT_BIN_DIR + '/') or
              self.target.startswith(YB_BUILD_SUPPORT_DIR + '/')):
            self.category = 'yb-scripts'

        if not self.category:
            for system_library_path in SYSTEM_LIBRARY_PATHS:
                if self.target.startswith(system_library_path + '/'):
                    self.category = 'system'
                    break

        if self.category:
            if self.category not in LIBRARY_CATEGORIES:
                raise RuntimeError(
                    ("Internal error: library category computed as '{}', must be one of: {}. " +
                     "Dependency: {}").format(self.category, LIBRARY_CATEGORIES, self))
            return self.category

        raise RuntimeError(
            ("Could not determine the category of this binary "
             "(yugabyte / yb-thirdparty / linuxbrew / system): '{}'. "
             "Does not reside in the Linuxbrew directory ({}), "
             "YB third-party directory ('{}'), "
             "YB build directory ('{}'), "
             "YB general-purpose script directory ('{}'), "
             "YB build support script directory ('{}'), "
             "and does not appear to be a system library (does not start with any of {})."
             ).format(
                self.target,
                linuxbrew_home.get_human_readable_dirs(),
                get_thirdparty_dir(),
                self.context.build_dir,
                YB_SCRIPT_BIN_DIR,
                YB_BUILD_SUPPORT_DIR,
                SYSTEM_LIBRARY_PATHS))


def add_common_arguments(parser):
    """
    Add command-line arguments common between library_packager_old.py invoked as a script, and
    the yb_release.py script.
    """
    parser.add_argument('--verbose',
                        help='Enable verbose output.',
                        action='store_true')


def run_patchelf(*args):
    patchelf_result = run_program([linuxbrew_home.patchelf_path] + list(args), error_ok=True)
    if patchelf_result.returncode != 0 and patchelf_result.stderr not in [
            'cannot find section .interp',
            'cannot find section .dynamic',
            PATCHELF_NOT_AN_ELF_EXECUTABLE]:
        raise RuntimeError(patchelf_result.error_msg)
    return patchelf_result


def symlink(source, link_path):
    if os.path.exists(link_path):
        if not source.startswith('/') or os.path.realpath(link_path) != os.path.realpath(source):
            raise RuntimeError(
                    "Trying to create symlink '{}' -> '{}' but it already points to '{}'".format(
                        link_path, source, os.readlink(link_path)))
        # In the case source is an absolute path and the link already points at it there will be
        # no error.
    else:
        os.symlink(source, link_path)


class LibraryPackager:
    """
    A utility for starting with a set of 'seed' executables, and walking the dependency tree of
    libraries to find all libraries that need to be packaged with the product.
    """
    def __init__(self,
                 build_dir,
                 seed_executable_patterns,
                 dest_dir,
                 verbose_mode=False):
        build_dir = os.path.realpath(build_dir)
        if not os.path.exists(build_dir):
            raise IOError("Build directory '{}' does not exist".format(build_dir))
        self.seed_executable_patterns = seed_executable_patterns
        self.dest_dir = dest_dir
        logging.debug(
            "Traversing the dependency graph of executables/libraries, starting "
            "with seed executable patterns: {}".format(", ".join(seed_executable_patterns)))
        self.context = DistributionContext(
            dest_dir=dest_dir,
            build_dir=build_dir,
            verbose_mode=verbose_mode)
        self.installed_dyn_linked_binaries = []
        self.main_dest_bin_dir = os.path.join(self.dest_dir, 'bin')
        self.postgres_dest_bin_dir = os.path.join(self.dest_dir, 'postgres', 'bin')

    def install_dyn_linked_binary(self, src_path, dest_dir):
        if not os.path.isdir(dest_dir):
            raise RuntimeError("Not a directory: '{}'".format(dest_dir))
        shutil.copy(src_path, dest_dir)
        installed_binary_path = os.path.join(dest_dir, os.path.basename(src_path))
        self.installed_dyn_linked_binaries.append(installed_binary_path)
        return installed_binary_path

    def find_elf_dependencies(self, elf_file_path):
        """
        Run ldd on the given ELF file and find libraries that it depends on. Also run patchelf and
        get the dynamic linker used by the file.

        @param elf_file_path: ELF file (executable/library) path
        """

        elf_file_path = os.path.realpath(elf_file_path)
        if SYSTEM_LIBRARY_PATH_RE.match(elf_file_path):
            ldd_path = '/usr/bin/ldd'
        else:
            ldd_path = linuxbrew_home.ldd_path

        ldd_result = run_program([ldd_path, elf_file_path], error_ok=True)
        dependencies = set()

        ldd_result_stdout_str = ldd_result.stdout
        if ldd_result.returncode != 0:
            # Interestingly, the below error message is printed to stdout, not stderr.
            if ldd_result_stdout_str == 'not a dynamic executable':
                logging.debug(
                    "Not a dynamic executable: {}, ignoring dependency tracking".format(
                        elf_file_path))
                return dependencies
            raise RuntimeError(ldd_result.error_msg)

        for ldd_output_line in ldd_result_stdout_str.split("\n"):
            resolved_dep_match = RESOLVED_DEP_RE.match(ldd_output_line)
            if resolved_dep_match:
                lib_name = resolved_dep_match.group(1)
                lib_resolved_path = os.path.realpath(resolved_dep_match.group(2))
                dependencies.add(Dependency(lib_name, lib_resolved_path, elf_file_path,
                                            self.context))

            tokens = ldd_output_line.split()
            if len(tokens) >= 4 and tokens[1:4] == ['=>', 'not', 'found']:
                missing_lib_name = tokens[0]
                raise RuntimeError("Library not found for '{}': {}".format(
                    elf_file_path, missing_lib_name))

                # If we matched neither RESOLVED_DEP_RE or the "not found" case, that is still fine,
                # e.g. there could be a line of the following form in the ldd output:
                #   linux-vdso.so.1 =>  (0x00007ffc0f9d2000)

        return dependencies

    @staticmethod
    def is_postgres_binary(file_path):
        return os.path.dirname(file_path).endswith('/postgres/bin')

    def get_dest_bin_dir_for_executable(self, file_path):
        if self.is_postgres_binary(file_path):
            dest_bin_dir = self.postgres_dest_bin_dir
        else:
            dest_bin_dir = self.main_dest_bin_dir
        return dest_bin_dir

    @staticmethod
    def join_binary_names_for_bash(binary_names):
        return ' '.join(['"{}"'.format(name) for name in binary_names])

    def package_binaries(self):
        """
        The main entry point to this class. Arranges binaries (executables and shared libraries),
        starting with the given set of "seed executables", in the destination directory so that
        the executables can find all of their dependencies.
        """
        all_deps = []

        dest_lib_dir = os.path.join(self.dest_dir, 'lib')
        mkdir_p(dest_lib_dir)

        mkdir_p(self.main_dest_bin_dir)
        mkdir_p(self.postgres_dest_bin_dir)

        main_elf_names_to_patch = []
        postgres_elf_names_to_patch = []

        for seed_executable_glob in self.seed_executable_patterns:
            glob_results = glob.glob(seed_executable_glob)
            if not glob_results:
                raise RuntimeError("No files found matching the pattern '{}'".format(
                    seed_executable_glob))
            for executable in glob_results:
                deps = self.find_elf_dependencies(executable)
                all_deps += deps
                dest_bin_dir = self.get_dest_bin_dir_for_executable(executable)
                if deps:
                    self.install_dyn_linked_binary(executable, dest_bin_dir)
                    executable_basename = os.path.basename(executable)
                    if self.is_postgres_binary(executable):
                        postgres_elf_names_to_patch.append(executable_basename)
                    else:
                        main_elf_names_to_patch.append(executable_basename)
                else:
                    # This is probably a script.
                    shutil.copy(executable, dest_bin_dir)

        # Not using the install_dyn_linked_binary method for copying patchelf and ld.so as we won't
        # need to do any post-processing on these two later.
        shutil.copy(linuxbrew_home.patchelf_path, self.main_dest_bin_dir)

        ld_path = linuxbrew_home.ld_so_path
        shutil.copy(ld_path, dest_lib_dir)

        all_deps = sorted(set(all_deps))

        for dep_name, deps in sorted_grouped_by(all_deps, lambda dep: dep.name):
            targets = sorted(set([dep.target for dep in deps]))
            if len(targets) > 1:
                raise RuntimeError(
                    "Multiple dependencies with the same name {} but different targets: {}".format(
                        dep_name, deps
                    ))

        linuxbrew_dest_dir = os.path.join(self.dest_dir, 'linuxbrew')
        linuxbrew_lib_dest_dir = os.path.join(linuxbrew_dest_dir, 'lib')

        # Add libresolv and libnss_* libs explicitly because they are loaded by glibc at runtime.
        additional_libs = set()
        for additional_lib_name_glob in ADDITIONAL_LIB_NAME_GLOBS:
            additional_libs.update(lib_path for lib_path in
                                   glob.glob(os.path.join(linuxbrew_home.cellar_glibc_dir,
                                                          '*',
                                                          'lib',
                                                          additional_lib_name_glob))
                                   if not lib_path.endswith('.a'))

        for category, deps_in_category in sorted_grouped_by(all_deps,
                                                            lambda dep: dep.get_category()):
            logging.info("Found {} dependencies in category '{}':".format(
                len(deps_in_category), category))

            max_name_len = max([len(dep.name) for dep in deps_in_category])
            for dep in sorted(deps_in_category, key=lambda dep: dep.target):
                logging.info("    {} -> {}".format(
                    dep.name + ' ' * (max_name_len - len(dep.name)), dep.target))

            if category == 'linuxbrew':
                category_dest_dir = linuxbrew_lib_dest_dir
            else:
                category_dest_dir = os.path.join(dest_lib_dir, category)
            mkdir_p(category_dest_dir)

            for dep in deps_in_category:
                additional_libs.discard(dep.target)
                self.install_dyn_linked_binary(dep.target, category_dest_dir)
                target_name = os.path.basename(dep.target)
                if target_name != dep.name:
                    target_src = os.path.join(os.path.dirname(dep.target), dep.name)
                    additional_libs.discard(target_src)
                    symlink(target_name, os.path.join(category_dest_dir, dep.name))

        for lib_path in additional_libs:
            if os.path.isfile(lib_path):
                self.install_dyn_linked_binary(lib_path, linuxbrew_lib_dest_dir)
                logging.info("Installed additional lib: " + lib_path)
            elif os.path.islink(lib_path):
                link_target_basename = os.path.basename(os.readlink(lib_path))
                logging.info("Installed additional symlink: " + lib_path)
                symlink(link_target_basename,
                        os.path.join(linuxbrew_lib_dest_dir, lib_basename))
            else:
                raise RuntimeError(
                    "Expected '{}' to be a file or a symlink".format(lib_path))

        for installed_binary in self.installed_dyn_linked_binaries:
            # Sometimes files that we copy from other locations are not even writable by user!
            subprocess.check_call(['chmod', 'u+w', installed_binary])
            # Remove rpath (we will set it appropriately in post_install.sh).
            run_patchelf('--remove-rpath', installed_binary)

        # Add other files used by glibc at runtime.
        linuxbrew_glibc_real_path = os.path.normpath(
            os.path.join(os.path.realpath(linuxbrew_home.ldd_path), '..', '..'))

        linuxbrew_glibc_rel_path = os.path.relpath(
            linuxbrew_glibc_real_path, os.path.realpath(linuxbrew_home.linuxbrew_dir))
        # We expect glibc to live under a path like "Cellar/glibc/2.23" in the Linuxbrew directory.
        if not linuxbrew_glibc_rel_path.startswith('Cellar/glibc/'):
            raise ValueError(
                "Expected to find glibc under Cellar/glibc/<version> in Linuxbrew, but found it "
                "at: '%s'" % linuxbrew_glibc_rel_path)

        rel_paths = []
        for glibc_rel_path in [
            'etc/ld.so.cache',
            'etc/localtime',
            'lib/locale/locale-archive',
            'lib/gconv',
            'libexec/getconf',
            'share/locale',
            'share/zoneinfo',
        ]:
            rel_paths.append(os.path.join(linuxbrew_glibc_rel_path, glibc_rel_path))

        terminfo_glob_pattern = os.path.join(
                linuxbrew_home.linuxbrew_dir, 'Cellar/ncurses/*/share/terminfo')
        terminfo_paths = glob.glob(terminfo_glob_pattern)
        if len(terminfo_paths) != 1:
            raise ValueError(
                "Failed to find the terminfo directory using glob pattern %s. "
                "Found: %s" % (terminfo_glob_pattern, terminfo_paths))
        terminfo_rel_path = os.path.relpath(terminfo_paths[0], linuxbrew_home.linuxbrew_dir)
        rel_paths.append(terminfo_rel_path)

        for rel_path in rel_paths:
            src = os.path.join(linuxbrew_home.linuxbrew_dir, rel_path)
            dst = os.path.join(linuxbrew_dest_dir, rel_path)
            copy_deep(src, dst, create_dst_dir=True)

        post_install_path = os.path.join(self.main_dest_bin_dir, 'post_install.sh')
        with open(post_install_path) as post_install_script_input:
            post_install_script = post_install_script_input.read()

        new_post_install_script = post_install_script
        replacements = [
            ("original_linuxbrew_path_to_patch", linuxbrew_home.linuxbrew_dir),
            ("original_linuxbrew_path_length", len(linuxbrew_home.linuxbrew_dir)),
        ]
        for macro_var_name, list_of_binary_names in [
            ("main_elf_names_to_patch", main_elf_names_to_patch),
            ("postgres_elf_names_to_patch", postgres_elf_names_to_patch),
        ]:
            replacements.append(
                (macro_var_name, self.join_binary_names_for_bash(list_of_binary_names)))

        for macro_var_name, value in replacements:
            new_post_install_script = new_post_install_script.replace(
                '${%s}' % macro_var_name, str(value))

        with open(post_install_path, 'w') as post_install_script_output:
            post_install_script_output.write(new_post_install_script)


def set_build_root(build_root):
    global linuxbrew_home
    from yb import linuxbrew
    linuxbrew_home = linuxbrew.LinuxbrewHome(build_root)
