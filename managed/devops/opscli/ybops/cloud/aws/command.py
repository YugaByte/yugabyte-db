#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.cloud.aws.method import AwsProvisionInstancesMethod, AwsCreateInstancesMethod, \
    AwsListInstancesMethod, AwsDestroyInstancesMethod, AwsAccessAddKeyMethod, \
    AwsAccessListKeysMethod, AwsQueryRegionsMethod, AwsQueryZonesMethod, AwsQueryPricingMethod, \
    AwsNetworkBootstrapMethod, AwsNetworkQueryMethod, AwsNetworkCleanupMethod, \
    AwsQueryCurrentHostMethod, AwsAccessDeleteKeyMethod, \
    AwsQueryVPCMethod, AwsQuerySpotPricingMethod, AwsCreateDnsEntryMethod, AwsEditDnsEntryMethod, \
    AwsDeleteDnsEntryMethod, AwsListDnsEntryMethod, AwsTagsMethod
from ybops.cloud.common.command import InstanceCommand, NetworkCommand, AccessCommand, \
    QueryCommand, DnsCommand
from ybops.cloud.common.base import AbstractPerCloudCommand
from ybops.cloud.common.method import ConfigureInstancesMethod, AccessCreateVaultMethod, \
    InitYSQLMethod, UpdateDiskMethod, CronCheckMethod


class AwsInstanceCommand(InstanceCommand):
    """Subclass for AWS specific instance command baseclass. Supplies overrides for method hooks.
    """
    def __init__(self):
        super(AwsInstanceCommand, self).__init__()

    def add_methods(self):
        self.add_method(AwsProvisionInstancesMethod(self))
        self.add_method(AwsCreateInstancesMethod(self))
        self.add_method(AwsDestroyInstancesMethod(self))
        self.add_method(AwsListInstancesMethod(self))
        self.add_method(ConfigureInstancesMethod(self))
        self.add_method(AwsTagsMethod(self))
        self.add_method(InitYSQLMethod(self))
        self.add_method(UpdateDiskMethod(self))
        self.add_method(CronCheckMethod(self))


class AwsNetworkCommand(NetworkCommand):
    def __init__(self):
        super(AwsNetworkCommand, self).__init__()

    def add_methods(self):
        self.add_method(AwsNetworkBootstrapMethod(self))
        self.add_method(AwsNetworkQueryMethod(self))
        self.add_method(AwsNetworkCleanupMethod(self))


class AwsAccessCommand(AccessCommand):
    def __init__(self):
        super(AwsAccessCommand, self).__init__()

    def add_methods(self):
        self.add_method(AwsAccessAddKeyMethod(self))
        self.add_method(AwsAccessListKeysMethod(self))
        self.add_method(AccessCreateVaultMethod(self))
        self.add_method(AwsAccessDeleteKeyMethod(self))


class AwsQueryCommand(QueryCommand):
    def __init__(self):
        super(AwsQueryCommand, self).__init__()

    def add_methods(self):
        self.add_method(AwsQueryRegionsMethod(self))
        self.add_method(AwsQueryZonesMethod(self))
        self.add_method(AwsQueryPricingMethod(self))
        self.add_method(AwsQueryCurrentHostMethod(self))
        self.add_method(AwsQueryVPCMethod(self))
        self.add_method(AwsQuerySpotPricingMethod(self))


class AwsDnsCommand(DnsCommand):
    def __init__(self):
        super(AwsDnsCommand, self).__init__()

    def add_methods(self):
        self.add_method(AwsCreateDnsEntryMethod(self))
        self.add_method(AwsEditDnsEntryMethod(self))
        self.add_method(AwsDeleteDnsEntryMethod(self))
        self.add_method(AwsListDnsEntryMethod(self))
