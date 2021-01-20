#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.cloud.common.command import InstanceCommand, QueryCommand, AccessCommand, \
    NetworkCommand
from ybops.cloud.common.method import ConfigureInstancesMethod, DestroyInstancesMethod, \
    ListInstancesMethod, AccessCreateVaultMethod, InitYSQLMethod, UpdateDiskMethod, CronCheckMethod
from ybops.cloud.gcp.method import GcpCreateInstancesMethod, GcpProvisionInstancesMethod, \
    GcpQueryRegionsMethod, GcpQueryZonesMethod, GcpQueryInstanceTypesMethod, \
    GcpQueryCurrentHostMethod, GcpQueryPreemptibleInstanceMethod, GcpDestroyInstancesMethod, \
    GcpAccessAddKeyMethod, GcpNetworkBootstrapMethod, GcpNetworkQueryMethod, \
    GcpNetworkCleanupMethod, GcpQueryVpcMethod


class GcpInstanceCommand(InstanceCommand):
    """Subclass for GCP specific instance command baseclass. Supplies overrides for method hooks.
    """
    def __init__(self):
        super(GcpInstanceCommand, self).__init__()

    def add_methods(self):
        self.add_method(GcpProvisionInstancesMethod(self))
        self.add_method(GcpCreateInstancesMethod(self))
        self.add_method(GcpDestroyInstancesMethod(self))
        self.add_method(ListInstancesMethod(self))
        self.add_method(ConfigureInstancesMethod(self))
        self.add_method(InitYSQLMethod(self))
        self.add_method(UpdateDiskMethod(self))
        self.add_method(CronCheckMethod(self))


class GcpQueryCommand(QueryCommand):
    def __init__(self):
        super(GcpQueryCommand, self).__init__()

    def add_methods(self):
        self.add_method(GcpQueryRegionsMethod(self))
        self.add_method(GcpQueryZonesMethod(self))
        self.add_method(GcpQueryInstanceTypesMethod(self))
        self.add_method(GcpQueryCurrentHostMethod(self))
        self.add_method(GcpQueryPreemptibleInstanceMethod(self))
        self.add_method(GcpQueryVpcMethod(self))


class GcpAccessCommand(AccessCommand):
    def __init__(self):
        super(GcpAccessCommand, self).__init__()

    def add_methods(self):
        self.add_method(GcpAccessAddKeyMethod(self))
        self.add_method(AccessCreateVaultMethod(self))


class GcpNetworkCommand(NetworkCommand):
    def __init__(self):
        super(GcpNetworkCommand, self).__init__()

    def add_methods(self):
        self.add_method(GcpNetworkBootstrapMethod(self))
        self.add_method(GcpNetworkQueryMethod(self))
        self.add_method(GcpNetworkCleanupMethod(self))
