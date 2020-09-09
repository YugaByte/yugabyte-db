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

"""
Validates that the given build root directory is within one of the allowed parent directories.
"""
import os
import sys

if __name__ == '__main__':
    build_root = os.path.realpath(sys.argv[1])
    internal_build_root_parent_dir = os.path.realpath(sys.argv[2]) + '/'
    external_build_root_parent_dir = os.path.realpath(sys.argv[3]) + '/'

    if not build_root.startswith(internal_build_root_parent_dir) and \
       not build_root.startswith(external_build_root_parent_dir):
        print ("Build root '{}' is not within either '{}' or '{}'".format(
            build_root,
            internal_build_root_parent_dir,
            internal_build_root_parent_dir), file=sys.stderr)
        sys.exit(1)
