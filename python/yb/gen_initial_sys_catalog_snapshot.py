#!/usr/bin/env python

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
A wrapper script for a C++ program that generates the initial sys catalog snapshot.
"""

import sys
import os
import argparse
import subprocess
import logging
import shutil
import time

from yugabyte_pycommon import init_logging, run_program, WorkDirContext, mkdir_p
from yb.common_util import YB_SRC_ROOT


def main():
    if os.environ.get('YB_SKIP_INITIAL_SYS_CATALOG_SNAPSHOT', '0') == '1':
        logging.info('YB_SKIP_INITIAL_SYS_CATALOG_SNAPSHOT is set, skipping initdb')
        return

    build_root = os.environ['YB_BUILD_ROOT']
    tool_name = 'create_initial_sys_catalog_snapshot'
    tool_path = os.path.join(build_root, 'tests-pgwrapper', tool_name)
    snapshot_dest_path = os.path.join(build_root, 'share', 'initial_sys_catalog_snapshot')

    file_to_check = os.path.join(snapshot_dest_path, 'exported_tablet_metadata_changes')
    if (os.path.exists(file_to_check) and
            os.environ.get('YB_RECREATE_INITIAL_SYS_CATALOG_SNAPSHOT', '') != '1'):
        logging.info(
            "Initial sys catalog snapshot already exists, not re-creating: %s",
            snapshot_dest_path)
        return
    if os.path.exists(snapshot_dest_path):
        logging.info("Removing initial sys catalog snapshot data at: %s", snapshot_dest_path)
        shutil.rmtree(snapshot_dest_path)

    mkdir_p(os.path.dirname(snapshot_dest_path))

    start_time_sec = time.time()
    logging.info("Starting creating initial system catalog snapshot data")
    logging.info("Logging to: %s", os.path.join(build_root, tool_name + '.err'))
    os.environ['YB_EXTRA_GTEST_FLAGS'] = (
        '--initial_sys_catalog_snapshot_dest_path=' + snapshot_dest_path
    )
    os.environ['YB_CTEST_VERBOSE'] = '1'
    with WorkDirContext(build_root):
        initdb_result = run_program(
            [
                os.path.join(YB_SRC_ROOT, 'build-support', 'run-test.sh'),
                tool_path,
            ],
            stdout_stderr_prefix=tool_name,
            shell=True,
            error_ok=True
        )
        elapsed_time_sec = time.time() - start_time_sec
        if initdb_result.failure():
            initdb_result.print_output_to_stdout()
            raise RuntimeError("initdb failed in %.1f sec" % elapsed_time_sec)

    logging.info(
        "Initial system catalog snapshot data creation took %1.f sec. Wrote data to: %s",
        elapsed_time_sec, snapshot_dest_path)


if __name__ == '__main__':
    init_logging()
    main()
