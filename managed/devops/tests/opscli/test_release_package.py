#!/bin/python3

from unittest import TestCase

from ybops.utils import ReleasePackage
from ybops.common.exceptions import YBOpsRuntimeError, get_exception_message


class TestReleasePackage(TestCase):
    good_inputs = [
        # Unofficial releases, old EE.
        ('yugabyte-ee-1.3.1.0-94463ba1b01fd4c5ed0ed16f53be5314f4ff9919-release-centos-x86_64.tar.gz', False),
        ('yugaware-ee-1.3.1.0-94463ba1b01fd4c5ed0ed16f53be5314f4ff9919-centos-x86_64.tar.gz', False),
        ('devops-ee-1.3.1.0-94463ba1b01fd4c5ed0ed16f53be5314f4ff9919-centos-x86_64.tar.gz', False),
        # Unofficial releases, new non-EE.
        ('yugabyte-1.3.1.0-94463ba1b01fd4c5ed0ed16f53be5314f4ff9919-centos-release-x86_64.tar.gz', False),
        ('yugaware-1.3.1.0-94463ba1b01fd4c5ed0ed16f53be5314f4ff9919-centos-x86_64.tar.gz', False),
        ('devops-1.3.1.0-94463ba1b01fd4c5ed0ed16f53be5314f4ff9919-centos-x86_64.tar.gz', False),
        # Official releases, old EE.
        ('yugabyte-1.3.1.0-b1-centos-x86_64.tar.gz', True),
        ('yugaware-1.3.1.0-b1-centos-x86_64.tar.gz', True),
        ('devops-1.3.1.0-b1-centos-x86_64.tar.gz', True),
        # Unofficial releases, new non-EE.
        ('yugabyte-ee-1.3.1.0-b1-centos-x86_64.tar.gz', True),
        ('yugaware-ee-1.3.1.0-b1-centos-x86_64.tar.gz', True),
        ('devops-ee-1.3.1.0-b1-centos-x86_64.tar.gz', True),
    ]

    bad_inputs = [
        ('yugabyte-ee-1.3.1.0-invalid-centos-x86_64.tar.gz', True),
    ]

    def test_from_package_name_success(self):
        for i in self.good_inputs:
            release, official = i
            r = ReleasePackage.from_package_name(release, official)
            # This will throw exceptions on failure.
            r.get_release_package_name()
            self.assertEquals(official, r.build_number is not None)
            self.assertEquals(official, r.commit is None)

    def test_from_package_name_failure(self):
        for i in self.bad_inputs:
            release, official = i
            # This will throw exceptions on failure.
            with self.assertRaises(Exception) as context:
                ReleasePackage.from_package_name(release, official)
            self.assertTrue(
                'Invalid package name format' in get_exception_message(context.exception))
