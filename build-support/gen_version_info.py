#!/usr/bin/env python3

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
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
# This script generates a header file which contains definitions
# for the current YugaByte build (e.g. timestamp, git hash, etc)

import json
import logging
import argparse
import os
import re
import pwd
import subprocess
import sys
import time
import pipes
import socket
from time import strftime, localtime

sys.path.append(os.path.join(os.path.dirname(os.path.dirname(__file__)), 'python'))


from yb.common_util import get_yb_src_root_from_build_root  # noqa


def is_git_repo_clean(git_repo_dir):
    return subprocess.call(
        "cd {} && git diff --quiet && git diff --cached --quiet".format(
            pipes.quote(git_repo_dir)),
        shell=True) == 0


def boolean_to_json_str(bool_flag):
    return str(bool_flag).lower()


def get_git_sha1(git_repo_dir):
    try:
        sha1 = subprocess.check_output(
            'cd {} && git rev-parse HEAD'.format(pipes.quote(git_repo_dir)), shell=True
        ).decode('utf-8').strip()

        if re.match(r'^[0-9a-f]{40}$', sha1):
            return sha1
        logging.warning("Invalid git SHA1 in directory '%s': %s", git_repo_dir, sha1)

    except Exception as e:
        logging.warning("Failed to get git SHA1 in directory: %s", git_repo_dir)


def main():
    logging.basicConfig(
        level=logging.INFO,
        format="[" + os.path.basename(__file__) + "] %(asctime)s %(levelname)s: %(message)s")

    parser = argparse.ArgumentParser(usage="usage: %prog <output_path>")
    parser.add_argument("--build-type", help="Set build type", type=str)
    parser.add_argument("--git-hash", help="Set git hash", type=str)
    parser.add_argument("output_path", help="Output file to be generated.", type=str)
    args = parser.parse_args()

    output_path = args.output_path

    hostname = socket.gethostname()
    build_time = "%s %s" % (strftime("%d %b %Y %H:%M:%S", localtime()), time.tzname[0])

    try:
        username = os.getlogin()
    except OSError as ex:
        logging.warning(("Got an OSError trying to get the current user name, " +
                         "trying a workaround: {}").format(ex))
        # https://github.com/gitpython-developers/gitpython/issues/39
        try:
            username = pwd.getpwuid(os.getuid()).pw_name
        except KeyError:
            username = os.getenv('USER')
            if not username:
                id_output = subprocess.check_output('id').strip()
                ID_OUTPUT_RE = re.compile(r'^uid=\d+[(]([^)]+)[)]\s.*')
                match = ID_OUTPUT_RE.match(id_output)
                if match:
                    username = match.group(1)
                else:
                    logging.warning(
                        "Couldn't get user name from the environment, either parse 'id' output: %s",
                        id_output)
                    raise

    git_repo_dir = get_yb_src_root_from_build_root(os.getcwd(), must_succeed=False, verbose=True)
    clean_repo = bool(git_repo_dir) and is_git_repo_clean(git_repo_dir)

    if args.git_hash:
        # Git hash provided on the command line.
        git_hash = args.git_hash
    elif 'YB_VERSION_INFO_GIT_SHA1' in os.environ:
        git_hash = os.environ['YB_VERSION_INFO_GIT_SHA1']
        logging.info("Git SHA1 provided using the YB_VERSION_INFO_GIT_SHA1 env var: %s", git_hash)
    else:
        # No command line git hash, find it in the local git repository.
        git_hash = get_git_sha1(git_repo_dir)

    path_to_version_file = os.path.join(
        os.path.dirname(os.path.realpath(__file__)), "..", "version.txt")
    with open(path_to_version_file) as version_file:
        version_string = version_file.read().strip()
    match = re.match("(\d+\.\d+\.\d+\.\d+)", version_string)
    if not match:
        parser.error("Invalid version specified: {}".format(version_string))
        sys.exit(1)
    version_number = match.group(1)
    build_type = args.build_type

    # Add the Jenkins build ID
    build_id = os.getenv("BUILD_ID", "")
    # This will be replaced by the release process.
    build_number = "PRE_RELEASE"

    d = os.path.dirname(output_path)
    if not os.path.exists(d):
        os.makedirs(d)
    log_file_path = os.path.join(d, os.path.splitext(os.path.basename(__file__))[0] + '.log')
    file_log_handler = logging.FileHandler(log_file_path)
    file_log_handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s: %(message)s"))
    logging.getLogger('').addHandler(file_log_handler)

    data = {
            "git_hash": git_hash,
            "build_hostname": hostname,
            "build_timestamp": build_time,
            "build_username": username,
            # In version_info.cc we expect build_clean_repo to be a "true"/"false" string.
            "build_clean_repo": boolean_to_json_str(clean_repo),
            "build_id": build_id,
            "build_type": build_type,
            "version_number": version_number,
            "build_number": build_number
            }
    content = json.dumps(data)

    # Frequently getting errors here when rebuilding on NFS:
    # https://gist.githubusercontent.com/mbautin/572dc0ab6b9c269910c1a51f31d79b38/raw
    attempts_left = 10
    while attempts_left > 0:
        try:
            with open(output_path, "w") as f:
                f.write(content)
            break
        except IOError as ex:
            if attempts_left == 0:
                raise ex
            if 'Resource temporarily unavailable' in ex.message:
                time.sleep(0.1)
        attempts_left -= 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
