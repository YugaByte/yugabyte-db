# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from setuptools import setup

setup(name='ybops',
      version='0.4',
      description='YugaByte Ops Utility',
      url='https://github.com/YugaByte/yugabyte-db/tree/master/managed/devops',
      author='Ram Sri',
      author_email='ramkumarvs@users.noreply.github.com',
      license='YugaByte, Inc.',
      packages=[
          'ybops',
          'ybops/cloud',
          'ybops/cloud/aws',
          'ybops/cloud/common',
          'ybops/cloud/gcp',
          'ybops/cloud/onprem',
          'ybops/cloud/azure',
          'ybops/common',
          'ybops/utils',
      ],
      package_data={'ybops': ['data/*.yml', 'data/internal/*.yml', 'data/*.j2']},
      scripts=[
          'ybops/scripts/ybcloud.py'
      ],
      zip_safe=False)
