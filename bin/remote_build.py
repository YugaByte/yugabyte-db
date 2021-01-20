#!/usr/bin/env python3

#
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

import argparse
import os
import sys
import logging

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'python'))  # noqa

from yb import remote
from yb.common_util import init_env


def add_extra_ybd_args(ybd_args, extra_args):
    """
    Inserts extra arguments into a list of yb_build.sh arguments. If a "--" argument is present,
    new arguments are inserted before it, because the rest of yb_build.sh's arguments may be passed
    along to yet another command.

    :param ybd_args: existing yb_build.sh arguments
    :param extra_args: extra arguments to insert
    :return: new list of yb_build.sh arguments
    """
    for i in range(len(ybd_args)):
        if ybd_args[i] == '--':
            return ybd_args[:i] + extra_args + ybd_args[i:]

    return ybd_args + extra_args


def main():
    parser = argparse.ArgumentParser(prog=sys.argv[0])
    parser.add_argument('--host', type=str, default=None,
                        help=('Host to build on. Can also be specified using the {} environment ' +
                              'variable.').format(remote.REMOTE_BUILD_HOST_ENV_VAR))
    home = os.path.expanduser('~')
    cwd = os.getcwd()
    default_path = '~/{0}'.format(
        cwd[len(home) + 1:] if cwd.startswith(home + '/') else 'code/yugabyte'
    )

    # Note: don't specify default arguments here, because they may come from the "profile".
    parser.add_argument('--remote-path', type=str,
                        help='path used for build')
    parser.add_argument('--branch', type=str, default=None,
                        help='base branch for build')
    parser.add_argument('--build-type', type=str, default=None,
                        help='build type')
    parser.add_argument('--skip-build', action='store_true',
                        help='skip build, only sync files')
    parser.add_argument('--wait-for-ssh', action='store_true',
                        help='Wait for the remote server to be ssh-able')
    parser.add_argument('--profile',
                        help='Use a "profile" specified in the {} file'.format(
                            remote.CONFIG_FILE_PATH))
    parser.add_argument('--verbose',
                        action='store_true',
                        help='Verbose output')
    parser.add_argument('build_args', nargs=argparse.REMAINDER,
                        help='arguments for yb_build.sh')

    if len(sys.argv) >= 2 and sys.argv[1] in ['ybd', 'yb_build.sh']:
        # Allow the first argument to be 'ybd' so we can copy and paste a ybd command line directly
        # after remote_build.py.
        sys.argv[1:2] = ['--']
    args = parser.parse_args()
    init_env(verbose=args.verbose)

    remote.load_profile(args, args.profile)

    # ---------------------------------------------------------------------------------------------
    # Default arguments go here.

    args.host = remote.apply_default_host_value(args.host)

    if args.branch is None:
        args.branch = remote.DEFAULT_BASE_BRANCH

    if args.remote_path is None:
        args.remote_path = default_path

    # End of default arguments.
    # ---------------------------------------------------------------------------------------------

    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    print("Host: {0}, build type: {1}, remote path: {2}".format(args.host,
                                                                args.build_type or 'N/A',
                                                                args.remote_path))
    print("Arguments to remote build: {}".format(args.build_args))

    escaped_remote_path = \
        remote.sync_changes(args.host, args.branch, args.remote_path, args.wait_for_ssh)

    if args.skip_build:
        sys.exit(0)

    remote_args = []
    if args.build_type:
        remote_args.append(args.build_type)

    if len(args.build_args) != 0 and args.build_args[0] == '--':
        remote_args += args.build_args[1:]
    else:
        remote_args += args.build_args

    if '--host-for-tests' not in remote_args and 'YB_HOST_FOR_RUNNING_TESTS' in os.environ:
        remote_args = add_extra_ybd_args(remote_args,
                                         ['--host-for-tests',
                                          os.environ['YB_HOST_FOR_RUNNING_TESTS']])

    remote.exec_command(args.host, escaped_remote_path, 'yb_build.sh', remote_args,
                        do_quote_args=True)


if __name__ == '__main__':
    main()
