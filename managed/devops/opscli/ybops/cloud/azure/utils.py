# Copyright 2020 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from azure.common.credentials import ServicePrincipalCredentials
from azure.mgmt.network import NetworkManagementClient
from azure.mgmt.resource import ResourceManagementClient
from azure.mgmt.compute import ComputeManagementClient
from azure.mgmt.compute.models import DiskCreateOption
from msrestazure.azure_exceptions import CloudError
from ybops.utils import is_valid_ip_address, validated_key_file, format_rsa_key, wait_for_ssh
from ybops.common.exceptions import YBOpsRuntimeError

import logging
import os
import re
from collections import defaultdict
import requests
import adal
import json

SUBSCRIPTION_ID = os.environ.get("AZURE_SUBSCRIPTION_ID")
RESOURCE_GROUP = os.environ.get("AZURE_RG")
SUBNET_ID_FORMAT_STRING = "/subscriptions/{}/resourceGroups/{}/providers/Microsoft.Network/virtualNetworks/{}/subnets/{}"
NSG_ID_FORMAT_STRING = "/subscriptions/{}/resourceGroups/{}/providers/Microsoft.Network/networkSecurityGroups/{}"
VNET_ID_FORMAT_STRING = "/subscriptions/{}/resourceGroups/{}/providers/Microsoft.Network/virtualNetworks/{}"
AZURE_SKU_FORMAT = {"premium_lrs": "Premium_LRS",
                    "standardssd_lrs": "StandardSSD_LRS",
                    "ultrassd_lrs": "UltraSSD_LRS"}
YUGABYTE_VNET_PREFIX = "yugabyte-vnet-{}"
YUGABYTE_SUBNET_PREFIX = "yugabyte-subnet-{}"
YUGABYTE_SG_PREFIX = "yugabyte-sg-{}"
YUGABYTE_PEERING_FORMAT = "yugabyte-peering-{}-{}"
RESOURCE_SKU_URL = "https://management.azure.com/subscriptions/{}/providers/Microsoft.Compute/skus".format(SUBSCRIPTION_ID)


def get_credentials():
    credentials = ServicePrincipalCredentials(
        client_id=os.environ.get("AZURE_CLIENT_ID"),
        secret=os.environ.get("AZURE_CLIENT_SECRET"),
        tenant=os.environ.get("AZURE_TENANT_ID")
    )
    return credentials


def create_resource_group(resource_group, region):
    resource_group_client = ResourceManagementClient(get_credentials(), SUBSCRIPTION_ID)
    if resource_group_client.resource_groups.check_existence(resource_group):
        return
    resource_group_params = {'location': region}
    return self.resource_group_client.resource_groups.create_or_update(
        RESOURCE_GROUP,
        resource_group_params
    )


def id_to_name(resourceId):
    return str(resourceId.split('/')[-1])


def get_zones(region, metadata):
    return ["{}-{}".format(region, zone)
            for zone in metadata["regions"].get(region, {}).get("zones", [])]


class AzureBootstrapClient():
    def __init__(self, region_meta, network, metadata):
        self.credentials = get_credentials()
        self.network_client = network
        self.region_meta = region_meta
        self.metadata = metadata

    def create_default_vnet(self, cidr, region):

        vnet_params = {
            'location': region,
            'address_space': {
                'address_prefixes': [cidr]
            },
        }
        logging.debug("Creating Virtual Network {} with CIDR {}".format(
            YUGABYTE_VNET_PREFIX.format(region), cidr))
        creation_result = self.network_client.virtual_networks.create_or_update(
            RESOURCE_GROUP,
            YUGABYTE_VNET_PREFIX.format(region),
            vnet_params
        )
        return creation_result.result().name

    def create_default_subnet(self, vNet, cidr, region):
        subnet_params = {
            'address_prefix': cidr
        }
        logging.debug("Creating Subnet {} with CIDR {}".format(
            YUGABYTE_SUBNET_PREFIX.format(region),
            cidr
        ))
        creation_result = self.network_client.subnets.create_or_update(
            RESOURCE_GROUP,
            vNet,
            YUGABYTE_SUBNET_PREFIX.format(region),
            subnet_params
        )

        return creation_result.result().name

    def get_default_vnet(self, region):
        vnets = [resource.serialize() for resource in
                 self.network_client.virtual_networks.list(RESOURCE_GROUP)]
        for vnetJson in vnets:
            # parse vnet from ID
            vnetName = id_to_name(vnetJson.get("id"))
            if (vnetJson.get("location") == region and
                    vnetName.startswith(YUGABYTE_VNET_PREFIX.format(''))):
                logging.debug("Found default vnet {}".format(vnetName))
                return vnetName
        logging.info("Could not find default {} in region {}".format(YUGABYTE_VNET_PREFIX, region))
        return None

    def get_default_subnet(self, vnet):
        """
        vnet - name of the vnet in which to look for subnets
        """
        subnets = [resource.serialize() for resource in
                   self.network_client.subnets.list(RESOURCE_GROUP, vnet)]
        for subnet in subnets:
            # Maybe change to tags rather than filtering on name prefix
            if(subnet.get("name").startswith(YUGABYTE_SUBNET_PREFIX.format(''))):
                logging.debug("Found default subnet {}".format(subnet.get("name")))
                return subnet.get("name")
        logging.info("Could not find default {} in vnet {}".format(YUGABYTE_SUBNET_PREFIX, vnet))
        return None

    def get_default_sg(self, region):
        """
        This method is currently not used. Auto creation currently does not set up
        a default security groups so all VMs brought up by default will block
        all public Internt access.
        """
        sgs = [resource.serialize() for resource in
               self.network_client.network_security_groups.list(RESOURCE_GROUP)]
        for sg in sgs:
            if (sg.get("location") == region and
                    sg.get("name").startswith(YUGABYTE_SG_PREFIX.format(''))):
                return sg.get("name")
        logging.info("Could not find default {} in region {}".format(YUGABYTE_SG_PREFIX, region))
        return None

    def get_vnet_cidr(self, region):
        return self.metadata["region_cidr_format"].format(
            self.metadata["regions"][region]["cidr_prefix"])

    def get_subnet_cidr(self, region):
        return self.metadata["zone_cidr_format"].format(
            self.metadata["regions"][region]["cidr_prefix"], 16)

    def bootstrap(self, region):
        result = {}
        vnet = self.get_default_vnet(region)
        if not vnet:
            vnet = self.create_default_vnet(self.get_vnet_cidr(region), region)
        subnet = self.get_default_subnet(vnet)
        if not subnet:
            subnet = self.create_default_subnet(vnet, self.get_subnet_cidr(region), region)
        self.fill_metadata(region, vnet, subnet)
        return self

    def fill_metadata(self, region, vnet, subnet):
        region_meta = {}
        region_meta["vpcId"] = vnet
        zones = get_zones(region, self.metadata)
        azToSubnet = {zone: subnet for zone in zones}
        region_meta["azToSubnetIds"] = azToSubnet
        self.region_meta = region_meta

    def cleanup(self, region):
        vnet = self.get_default_vnet(region)
        if not vnet:
            logging.debug("Could not find default vnet in region {}".format(region))
            return
        subnet = self.get_default_subnet(vnet)
        if subnet:
            self.network_client.subnets.delete(RESOURCE_GROUP, vnet, subnet).result()
            logging.debug("Successfully deleted subnet {}".format(subnet))
        self.network_client.virtual_networks.delete(RESOURCE_GROUP, vnet).result()
        logging.debug("Successfully deleted vnet {}".format(vnet))

    def get_vnet_id(self, vnet):
        """
        Generate vnet id format from vnet name
        """
        return VNET_ID_FORMAT_STRING.format(SUBSCRIPTION_ID, RESOURCE_GROUP, vnet)

    def gen_peering_params(self, remote_region, remote_vnet):
        peering_params = {
            "remoteVirtualNetwork": {
                "id": self.get_vnet_id(remote_vnet)
            },
            "allowVirtualNetworkAccess": True,
            "allowForwardedTraffic": True,
            "allowGatewayTransit": False,
            "useRemoteGateways": False,
            "remoteAddressSpace": {
                "addressPrefixes": [
                    self.get_vnet_cidr(remote_region)
                ]
            }
        }
        return peering_params

    def create_peering(self, region1, vnet1, region2, vnet2):
        """
        Creates two-way peering
        """
        try:
            self.network_client.virtual_network_peerings.get(
                RESOURCE_GROUP,
                vnet1,
                YUGABYTE_PEERING_FORMAT.format(region1, region2)
            )
            self.network_client.virtual_network_peerings.get(
                RESOURCE_GROUP,
                vnet2,
                YUGABYTE_PEERING_FORMAT.format(region2, region1)
            )
            logging.debug("Found peerings on Virtual Network {} and Virtual Network {}.".format(
                vnet1, vnet2
            ))
            return
        except CloudError:
            logging.info("Could not find peerings on either {} or {} in regions {} and {}.".format(
                vnet1, vnet2, region1, region2
            ))
            pass
        pp1 = self.gen_peering_params(region2, vnet2)
        pp2 = self.gen_peering_params(region1, vnet1)
        # Peer 2 to 1
        peer1 = self.network_client.virtual_network_peerings.create_or_update(
            RESOURCE_GROUP,
            vnet1,
            YUGABYTE_PEERING_FORMAT.format(region1, region2),
            pp1
        )
        # Peer 1 to 2
        peer2 = self.network_client.virtual_network_peerings.create_or_update(
            RESOURCE_GROUP,
            vnet2,
            YUGABYTE_PEERING_FORMAT.format(region2, region1),
            pp2
        )
        peer1.result()
        peer2.result()
        return

    def peer(self, components):
        region_and_vnet_tuples = [(r, c.get("vpc_id")) for r, c in components.iteritems()]
        for i in xrange(len(region_and_vnet_tuples) - 1):
            i_region, i_vnet = region_and_vnet_tuples[i]
            for j in xrange(i + 1, len(region_and_vnet_tuples)):
                j_region, j_vnet = region_and_vnet_tuples[j]
                self.create_peering(i_region, i_vnet, j_region, j_vnet)
        return

    def to_components(self):
        reg_info = {}
        reg_info["vpc_id"] = self.region_meta.get("vpcId")
        sg = self.region_meta.get("customSecurityGroupId", None)
        if sg:
            reg_info["security_group"] = [{"id": sg, "name": sg}]
        reg_info["zones"] = self.region_meta.get("azToSubnetIds")
        return reg_info


class AzureCloudAdmin():
    def __init__(self, metadata):
        self.metadata = metadata
        self.credentials = get_credentials()
        self.compute_client = ComputeManagementClient(self.credentials, SUBSCRIPTION_ID)
        self.network_client = NetworkManagementClient(self.credentials, SUBSCRIPTION_ID)

    def network(self, per_region_meta={}):
        return AzureBootstrapClient(per_region_meta, self.network_client, self.metadata)

    def appendDisk(self, vm, vm_name, disk_name, size, lun, zone, vol_type, region):
        data_disk = self.compute_client.disks.create_or_update(
            RESOURCE_GROUP,
            disk_name,
            {
                "location": region,
                "disk_size_gb": size,
                "creation_data": {
                    "create_option": DiskCreateOption.empty
                },
                "sku": {
                    "name": AZURE_SKU_FORMAT[vol_type]
                },
                "zones": [
                    zone
                ]
            }
        ).result()

        vm.storage_profile.data_disks.append({
            "lun": lun,
            "name": disk_name,
            "create_option": DiskCreateOption.attach,
            "managed_disk": {
                "storageAccountType": AZURE_SKU_FORMAT[vol_type],
                "id": data_disk.id
            }
        })

        async_disk_attach = self.compute_client.virtual_machines.create_or_update(
            RESOURCE_GROUP,
            vm_name,
            vm
        )

        async_disk_attach.wait()
        return async_disk_attach.result()

    def get_public_ip_name(self, vm_name):
        return vm_name + '-IP'

    def get_nic_name(self, vm_name):
        return vm_name + '-NIC'

    def create_public_ip_address(self, vm_name, zone, region):
        public_ip_addess_params = {
            "location": region,
            "sku": {
                "name": "Standard"  # Only standard SKU supports zone
            },
            "public_ip_allocation_method": "Static",
            "zones": [
                zone
            ]
        }
        creation_result = self.network_client.public_ip_addresses.create_or_update(
            RESOURCE_GROUP,
            self.get_public_ip_name(vm_name),
            public_ip_addess_params
        )
        return creation_result.result()

    def create_nic(self, vm_name, vnet, subnet, zone, nsg, region, public_ip):
        """
        Creates network interface and returns the id of the resource for use in
        vm creation.
            vm_name, vnet, subnet - String representing name of resource
            public_ip - bool if public_ip should be assigned
        """
        nic_params = {
            "location": region,
            "ip_configurations": [{
                "name": vm_name + "-IPConfig",
                "subnet": {
                    "id": self.get_subnet_id(vnet, subnet)
                },
            }],
        }
        if public_ip:
            publicIPAddress = self.create_public_ip_address(vm_name, zone, region)
            nic_params["ip_configurations"][0]["public_ip_address"] = publicIPAddress
        if nsg:
            nic_params['networkSecurityGroup'] = {'id': self.get_nsg_id(nsg)}
        creation_result = self.network_client.network_interfaces.create_or_update(
            RESOURCE_GROUP,
            self.get_nic_name(vm_name),
            nic_params
        )

        return creation_result.result().id

    def destroy_instance(self, vm_name, host_info):
        if not host_info:
            try:
                logging.info("Could not find VM {}. Deleting network interface and public IP."
                             .format(vm_name))
                nic_name = self.get_nic_name(vm_name)
                ip_name = self.get_public_ip_name(vm_name)
                nicdel = self.network_client.network_interfaces.delete(RESOURCE_GROUP, nic_name)
                nicdel.wait()
                ipdel = self.network_client.public_ip_addresses.delete(RESOURCE_GROUP, ip_name)
                ipdel.wait()
                logging.info("Sucessfully deleted related resources from {}".format(vm_name))
                return
            except CloudError:
                logging.error("Error deleting Network Interface or Public IP when {} not found"
                              .format(vm_name))

        # Since we have host_info, we know the virtual machine exists
        vm = self.compute_client.virtual_machines.get(RESOURCE_GROUP, vm_name)

        nic_name = host_info.get("nic", None)
        public_ip_name = host_info.get("ip_name", None)
        os_disk_name = vm.storage_profile.os_disk.name
        data_disks = vm.storage_profile.data_disks

        logging.debug("About to delete vm {}".format(vm_name))
        delete_op1 = self.compute_client.virtual_machines.delete(RESOURCE_GROUP, vm_name)
        delete_op1.wait()

        diskdels = []
        for disk in data_disks:
            logging.debug("About to delete disk {}".format(disk.name))
            disk_delop = self.compute_client.disks.delete(RESOURCE_GROUP, disk.name)
            diskdels.append(disk_delop)

        logging.debug("About to delete os disk {}".format(os_disk_name))
        disk_delop = self.compute_client.disks.delete(RESOURCE_GROUP, os_disk_name)
        diskdels.append(disk_delop)

        logging.debug("About to delete network interface {}".format(nic_name))
        delete_op2 = self.network_client.network_interfaces.delete(RESOURCE_GROUP, nic_name)
        delete_op2.wait()

        logging.debug("About to delete public ip {}".format(public_ip_name))
        if (public_ip_name):
            delete_op3 = self.network_client.public_ip_addresses.delete(RESOURCE_GROUP,
                                                                        public_ip_name)
            delete_op3.wait()

        for diskdel in diskdels:
            diskdel.wait()

        logging.info("Sucessfully deleted {} and all related resources".format(vm_name))
        return

    def get_subnet_id(self, vnet, subnet):
        return SUBNET_ID_FORMAT_STRING.format(
            SUBSCRIPTION_ID, RESOURCE_GROUP, vnet, subnet
        )

    def get_nsg_id(self, nsg):
        if nsg:
            return NSG_ID_FORMAT_STRING.format(
                SUBSCRIPTION_ID, RESOURCE_GROUP, nsg
            )
        else:
            return

    def add_tag_resource(self, params, key, value):
        result = params.get("tags", {})
        result[key] = value
        params["tags"] = result
        return params

    def create_vm(self, vm_name, zone, num_vols, private_key_file, volume_size,
                  instance_type, ssh_user, image, nsg, pub, offer, sku, vol_type, server_type,
                  region, nic_id):
        try:
            return self.compute_client.virtual_machines.get(RESOURCE_GROUP, vm_name)
        except CloudError:
            pass

        disk_names = [vm_name + "-Disk-" + str(i) for i in range(1, num_vols + 1)]
        private_key = validated_key_file(private_key_file)
        vm_parameters = {
            "location": region,
            "os_profile": {
                "computer_name": vm_name,
                "admin_username": ssh_user,
                "linux_configuration": {
                    "disable_password_authentication": True,
                    "ssh": {
                        "public_keys": [{
                            "path": "/home/{}/.ssh/authorized_keys".format(ssh_user),
                            "key_data": format_rsa_key(private_key, public_key=True)
                        }]
                    }
                }
            },
            "hardware_profile": {
                "vm_size": instance_type
            },
            "storage_profile": {
                "osDisk": {
                    "createOption": "fromImage",
                    "managedDisk": {
                        "storageAccountType": "Standard_LRS"
                    }
                },
                "image_reference": {
                        "publisher": pub,
                        "offer": offer,
                        "sku": sku,
                        "version": image
                }
            },
            "network_profile": {
                "network_interfaces": [{
                    "id": nic_id
                }]
            },
            "zones": [
                zone
            ]
        }

        if (vol_type == "ultrassd_lrs"):
            vm_parameters["additionalCapabilities"] = {"ultraSSDEnabled": True}

        # Tag VM as cluster-server for ansible configure-{} script
        self.add_tag_resource(vm_parameters, "yb-server-type", server_type)

        creation_result = self.compute_client.virtual_machines.create_or_update(
            RESOURCE_GROUP,
            vm_name,
            vm_parameters
        )

        vm_result = creation_result.result()
        vm = self.compute_client.virtual_machines.get(RESOURCE_GROUP, vm_name)

        # Attach disks
        for idx, disk_name in enumerate(disk_names):
            self.appendDisk(vm, vm_name, disk_name, volume_size, idx, zone, vol_type, region)

        return

    def query_vpc(self):
        """
        TODO: Similar to GCP, not implemented. Only used for getting default image.
        """
        return {}

    def format_zones(self, region, zones):
        return ["{}-{}".format(region, zone) for zone in zones]

    def get_zone_to_subnets(self, vnet, region):
        regionZones = self.metadata["regions"].get(region, {}).get("zones", [])
        zones = self.format_zones(region, regionZones)
        subnet = self.network().get_default_subnet(vnet)
        return {zone: subnet for zone in zones}

    def parse_vm_info(self, vm):
        vm_info = {}
        vm_info["numCores"] = vm.get("numberOfCores")
        vm_info["memSizeGb"] = float(vm.get("memoryInMB")) / 1000.0
        vm_info["maxDiskCount"] = vm.get("maxDataDiskCount")
        return vm_info

    def get_instance_types(self, regions):
        # TODO: This regex probably should be refined? It returns a LOT of VMs right now.
        premium_regex_format = 'Standard_.*s'
        regex = re.compile(premium_regex_format, re.IGNORECASE)

        all_vms = {}
        regionsPresent = defaultdict(set)

        for region in regions:
            vm_list = [vm.serialize() for vm in
                       self.compute_client.virtual_machine_sizes.list(location=region)]
            for vm in vm_list:
                vm_size = vm.get("name")
                # We only care about VMs that support Premium storage. Promo is pricing special.
                if (not regex.match(vm_size) or vm_size.endswith("Promo")):
                    continue
                all_vms[vm_size] = self.parse_vm_info(vm)
                regionsPresent[vm_size].add(region)

        num_regions = len(regions)
        # Only return VMs that are present in all regions
        return {vm: info for vm, info in all_vms.items() if len(regionsPresent[vm]) == num_regions}

    def ultra_ssd_available(self, capabilities):
        if not capabilities:
            return False
        for capability in capabilities:
            if (capability.get("name", None) == "UltraSSDAvailable"
                    and capability.get("value", None) == "True"):
                return True

    def get_ultra_instances(self, regions, folder):
        FOLDER_FORMAT = folder + "{}.json"
        tenant = os.environ['AZURE_TENANT_ID']
        authority_url = 'https://login.microsoftonline.com/' + tenant
        client_id = os.environ['AZURE_CLIENT_ID']
        client_secret = os.environ['AZURE_CLIENT_SECRET']
        resourceURL = 'https://management.azure.com/'
        context = adal.AuthenticationContext(authority_url)
        token = context.acquire_token_with_client_credentials(resourceURL, client_id, client_secret)
        headers = {'Authorization': 'Bearer ' + token['accessToken'],
                   'Content-Type': 'application/json'}
        for region in regions:
            vms = {}
            payload = {"api-version": "2019-04-01", "$filter": "location eq '{}'".format(region)}
            listOfResourcces = requests.get(RESOURCE_SKU_URL, params=payload,
                                            headers=headers).json().get("value", [])
            for resource in listOfResourcces:
                # We only care about virtual machines
                if resource.get("resourceType") != "virtualMachines":
                    continue
                # No special location info
                location_info = resource.get("locationInfo", [])
                if not location_info:
                    continue
                location_info = location_info[0]
                # No special zone info
                zone_details = location_info.get("zoneDetails", [])
                if not zone_details:
                    continue
                zone_details = zone_details[0]
                capabilities = zone_details.get("capabilities", [])
                # Checks zone details to see if detail is ultraSSD capability
                if self.ultra_ssd_available(capabilities):
                    instance_type = resource.get("name")
                    region = location_info.get("location")
                    zones = zone_details.get("Name")
                    yw_zones = self.format_zones(region, zones)
                    vms[instance_type] = yw_zones
            with open(FOLDER_FORMAT.format(region), 'w') as writefile:
                json.dump(vms, writefile)
        return

    def get_host_info(self, vm_name, get_all=False):
        try:
            vm = self.compute_client.virtual_machines.get(RESOURCE_GROUP, vm_name)
        except Exception as e:
            return None
        nic_name = id_to_name(vm.network_profile.network_interfaces[0].id)
        nic = self.network_client.network_interfaces.get(RESOURCE_GROUP, nic_name)
        region = vm.location
        zone = vm.zones[0] if vm.zones else None
        private_ip = nic.ip_configurations[0].private_ip_address
        public_ip = None
        ip_name = None
        if (nic.ip_configurations[0].public_ip_address):
            ip_name = id_to_name(nic.ip_configurations[0].public_ip_address.id)
            public_ip = (self.network_client.public_ip_addresses
                         .get(RESOURCE_GROUP, ip_name).ip_address)

        subnet = id_to_name(nic.ip_configurations[0].subnet.id)
        server_type = vm.tags.get("yb-server-type", None) if vm.tags else None
        return {"private_ip": private_ip, "public_ip": public_ip, "region": region,
                "zone": "{}-{}".format(region, zone), "name": vm.name, "ip_name": ip_name,
                "instance_type": vm.hardware_profile.vm_size, "server_type": server_type,
                "subnet": subnet, "nic": nic_name, "id": vm.name}
