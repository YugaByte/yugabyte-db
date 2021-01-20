#!/usr/bin/env python3
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

import os
import logging
import argparse
import shutil
from subprocess import check_call
from ybops.utils import init_env, log_message
from ybops.common.exceptions import YBOpsRuntimeError
from ybops.release_manager import ReleaseManager


"""This scripts calls the yugabyte release manager api to generate the
   release based on the release manifest.
"""
parser = argparse.ArgumentParser()
parser.add_argument("--destination", help="Copy release to Destination folder.")
parser.add_argument("--force", action="store_true",
                    help="Force build the package even if having local changes.")
args = parser.parse_args()

try:
    init_env(logging.INFO)
    script_dir = os.path.dirname(os.path.realpath(__file__))
    options = {
        "repository": script_dir,
        "name": "devops",
        "force_yes": args.force}
    release_manager = ReleaseManager(options)
    release_file = release_manager.generate_release()

    if args.destination:
        if not os.path.exists(args.destination):
            raise YBOpsRuntimeError("Destination {} not a directory.".format(args.destination))
        shutil.copy(release_file, args.destination)

except (RuntimeError) as e:
    log_message(logging.ERROR, e)
    raise e
