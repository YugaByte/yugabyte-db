#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

import yaml
import sys

from six import iteritems


def remove_leaf_items(root):
    if isinstance(root, dict):
        result = {}
        for x, y in iteritems(root):
            result[x] = remove_leaf_items(y)
        return result
    if isinstance(root, list):
        return [remove_leaf_items(x) for x in root]
    return '<hidden>'


if __name__ == '__main__':
    y = yaml.load(sys.stdin)
    print(yaml.dump(remove_leaf_items(y), default_flow_style=False))
