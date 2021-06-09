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
Helps find the Linuxbrew installation.
"""

import os
import sys
from yb.command_util import run_program
from yb.common_util import safe_path_join


g_linuxbrew_dir = None
g_build_root = None

YB_SRC_ROOT = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', '..')


class LinuxbrewHome:
    def __init__(self, build_root=None):
        old_build_root = os.environ.get('BUILD_ROOT')
        if build_root is not None:
            os.environ['BUILD_ROOT'] = build_root
        else:
            build_root = os.environ.get('BUILD_ROOT')
        self.linuxbrew_dir = None
        self.linuxbrew_link_target = None
        self.cellar_glibc_dir = None
        self.ldd_path = None
        self.ld_so_path = None
        self.patchelf_path = None

        try:
            find_script_result = run_program(os.path.join(
                YB_SRC_ROOT, 'build-support', 'find_linuxbrew.sh'))
            linuxbrew_dir = find_script_result.stdout.strip()
            if not linuxbrew_dir:
                return
            if not os.path.isdir(linuxbrew_dir) and os.path.exists('/etc/centos-release'):
                raise RuntimeError(
                        ("Directory returned by the '{}' script does not exist: '{}'. " +
                         "This is only an error on CentOS. Details: {}").format(
                                find_script_result.program_path,
                                linuxbrew_dir,
                                find_script_result))
            self.linuxbrew_dir = os.path.realpath(linuxbrew_dir)

            # Directories derived from the Linuxbrew top-level one.
            self.linuxbrew_link_target = os.path.realpath(linuxbrew_dir)
            self.cellar_glibc_dir = safe_path_join(self.linuxbrew_dir, 'Cellar', 'glibc')
            self.ldd_path = safe_path_join(self.linuxbrew_dir, 'bin', 'ldd')
            self.ld_so_path = safe_path_join(self.linuxbrew_dir, 'lib', 'ld.so')
            self.patchelf_path = safe_path_join(self.linuxbrew_dir, 'bin', 'patchelf')
        finally:
            if old_build_root is None:
                if 'BUILD_ROOT' in os.environ:
                    del os.environ['BUILD_ROOT']
            else:
                os.environ['BUILD_ROOT'] = old_build_root

    def path_is_in_linuxbrew_dir(self, path):
        if not self.linuxbrew_dir:
            return False
        path = os.path.abspath(path)
        return (path.startswith(self.linuxbrew_dir + '/') or
                path.startswith(self.linuxbrew_link_target + '/'))

    def get_human_readable_dirs(self):
        return ', '.join("'%s'" % d for d in
                         sorted(set([self.linuxbrew_dir, self.linuxbrew_link_target])))

    def is_enabled(self) -> bool:
        return self.linuxbrew_dir is not None


def set_build_root(build_root):
    global g_build_root
    g_build_root = build_root


def get_linuxbrew_dir():
    if not sys.platform.startswith('linux'):
        return None

    global g_linuxbrew_dir
    if not g_linuxbrew_dir:
        g_linuxbrew_dir = LinuxbrewHome(g_build_root).linuxbrew_dir
    return g_linuxbrew_dir
