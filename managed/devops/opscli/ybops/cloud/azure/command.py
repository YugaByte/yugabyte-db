# Copyright 2020 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.cloud.common.command import InstanceCommand, NetworkCommand, AccessCommand, \
    QueryCommand, DnsCommand
from ybops.cloud.azure.method import AzureNetworkBootstrapMethod, AzureProvisionInstancesMethod, \
    AzureCreateInstancesMethod, AzureAccessAddKeyMethod, AzureQueryVPCMethod, \
    AzureQueryRegionsMethod, AzureQueryZonesMethod, AzureDestroyInstancesMethod, \
    AzureQueryInstanceTypesMethod, AzureQueryVnetMethod, AzureAccessDeleteKeyMethod, \
    AzureNetworkCleanupMethod, AzureQueryUltraMethod
from ybops.cloud.common.method import AccessCreateVaultMethod, ConfigureInstancesMethod, \
    ListInstancesMethod, InitYSQLMethod, UpdateDiskMethod


class AzureNetworkCommand(NetworkCommand):
    def __init__(self):
        super(AzureNetworkCommand, self).__init__()

    def add_methods(self):
        self.add_method(AzureNetworkBootstrapMethod(self))
        self.add_method(AzureNetworkCleanupMethod(self))


class AzureInstanceCommand(InstanceCommand):
    def __init__(self):
        super(AzureInstanceCommand, self).__init__()

    def add_methods(self):
        self.add_method(AzureProvisionInstancesMethod(self))
        self.add_method(AzureCreateInstancesMethod(self))
        self.add_method(AzureDestroyInstancesMethod(self))
        self.add_method(ConfigureInstancesMethod(self))
        self.add_method(ListInstancesMethod(self))
        self.add_method(InitYSQLMethod(self))
        self.add_method(UpdateDiskMethod(self))


class AzureAccessCommand(AccessCommand):
    def __init__(self):
        super(AzureAccessCommand, self).__init__()

    def add_methods(self):
        self.add_method(AzureAccessAddKeyMethod(self))
        self.add_method(AccessCreateVaultMethod(self))
        self.add_method(AzureAccessDeleteKeyMethod(self))


class AzureQueryCommand(QueryCommand):
    def __init__(self):
        super(AzureQueryCommand, self).__init__()

    def add_methods(self):
        self.add_method(AzureQueryVPCMethod(self))
        self.add_method(AzureQueryRegionsMethod(self))
        self.add_method(AzureQueryZonesMethod(self))
        self.add_method(AzureQueryInstanceTypesMethod(self))
        self.add_method(AzureQueryVnetMethod(self))
        self.add_method(AzureQueryUltraMethod(self))
