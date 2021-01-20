#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.cloud.ybcloud import YbCloud


if __name__ == "__main__":
    YbCloud().run()
