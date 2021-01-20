#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
#
# Make Ansible failure output more readable.
# TODO: do not require stderr to be piped to stdout.

from __future__ import print_function

import re
import json
import sys


FATAL_JSON_RE = re.compile(r'^(.*(?:: FAILED!|changed: \[.*\]) .*=> )({.*})([^}]+)$')


def main():
    for line in sys.stdin:
        line = line.rstrip()
        match = FATAL_JSON_RE.search(line)
        if not match:
            print(line)
            continue
        pre = match.group(1)
        json_str = match.group(2)
        post = match.group(3)
        failure_json = json.loads(json_str)
        json_replacement = ""
        # Only keep std{out,err}_lines, remove the single-line counterpart.
        for key in ['stdout', 'stderr']:
            if key + '_lines' in failure_json:
                del failure_json[key]
        json_replacement = json.dumps(failure_json,  indent=2, sort_keys=True)
        print(pre + json_replacement + post)


if __name__ == '__main__':
    main()
