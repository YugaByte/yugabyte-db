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
import shlex

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'python'))  # noqa

from yb import remote


def main():
    parser = argparse.ArgumentParser(prog=sys.argv[0])
    parser.add_argument('--host', type=str, default=None,
                        help=('Host to build on. Can also be specified using the {} environment ' +
                              'variable.').format(remote.REMOTE_BUILD_HOST_ENV_VAR))
    home = os.path.expanduser('~')
    cwd = os.getcwd()
    default_path = '~/{0}'.format(cwd[len(home) + 1:] if cwd.startswith(home) else 'code/yugabyte')

    # Note: don't specify default arguments here, because they may come from the "profile".
    parser.add_argument('--remote-path', type=str, default=None,
                        help='path used for build')
    parser.add_argument('--branch', type=str, default=None,
                        help='base branch for build')
    parser.add_argument('--build-type', type=str, default=None,
                        help='build type, defaults to release')
    parser.add_argument('--skip-build', action='store_true',
                        help='skip build, only sync files')
    parser.add_argument('--build-args', type=str, default=None,
                        help='build arguments to pass')
    parser.add_argument('--wait-for-ssh', action='store_true',
                        help='Wait for the remote server to be ssh-able')
    parser.add_argument('--profile',
                        help='Use a "profile" specified in the {} file'.format(
                            remote.CONFIG_FILE_PATH))

    args = parser.parse_args()

    remote.load_profile(args, args.profile)

    # ---------------------------------------------------------------------------------------------
    # Default arguments go here.

    args.host = remote.apply_default_host_value(args.host)

    if args.branch is None:
        args.branch = remote.DEFAULT_BASE_BRANCH

    if args.remote_path is None:
        args.remote_path = default_path

    if args.build_type is None:
        args.build_type = "release"

    # End of default arguments.
    # ---------------------------------------------------------------------------------------------

    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    print("Host: {0}, build type: {1}, remote path: {2}".format(args.host,
                                                                args.build_type,
                                                                args.remote_path))
    print("Arguments to remote build: {}".format(args.build_args))

    escaped_remote_path = remote.sync_changes(args.host, args.branch, args.remote_path,
                                              args.wait_for_ssh)

    if args.skip_build:
        sys.exit(0)

    remote_args = []
    remote_args.append("--build {}".format(args.build_type))
    remote_args.append("--force")
    if args.build_args is not None:
        remote_args.append("--build_args=\"{}\"".format(args.build_args))

    remote.exec_command(args.host, escaped_remote_path, 'yb_release', remote_args, False)


if __name__ == '__main__':
    main()
