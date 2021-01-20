#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

import json
import logging

from ybops.common.exceptions import YBOpsRuntimeError, get_exception_message
from ybops.cloud.common.cloud import AbstractCloud
from ybops.cloud.gcp.command import GcpInstanceCommand, GcpQueryCommand, GcpAccessCommand, \
    GcpNetworkCommand
from ybops.cloud.gcp.utils import GCP_PERSISTENT, GCP_SCRATCH, GoogleCloudAdmin, GcpMetadata


class GcpCloud(AbstractCloud):
    """Subclass specific to GCP cloud related functionality.
    """
    FREE_OS_LIST = ["Linux"]

    def __init__(self):
        super(GcpCloud, self).__init__("gcp")
        self.admin = None

    def get_admin(self):
        if self.admin is None:
            self.admin = GoogleCloudAdmin(self.metadata)
        return self.admin

    def add_subcommands(self):
        """Override to setup the cloud-specific instances of the subcommands.
        """
        self.add_subcommand(GcpInstanceCommand())
        self.add_subcommand(GcpQueryCommand())
        self.add_subcommand(GcpAccessCommand())
        self.add_subcommand(GcpNetworkCommand())

    def has_machine_credentials(self):
        """
        Override for superclass method to detect if current instance has cloud access credentials.
        """
        content = GcpMetadata.service_accounts()
        return content is not None and len(content) > 0

    def get_image(self, region):
        region_meta = self.metadata["regions"][region]
        return self.get_admin().get_image(region_meta["image"], region_meta["project"])

    def query_vpc(self, args):
        result = {}
        regions = [args.region] if args.region else self.get_regions(args)
        for region in regions:
            result[region] = self.get_admin().query_vpc(region)
            result[region]["default_image"] = self.get_image(region)["selfLink"]
        return result

    def create_instance(self, args, body):
        self.get_admin().create_instance(args.zone, body)

    def create_disk(self, args, body):
        self.get_admin().create_disk(args.zone, body)

    def mount_disk(self, args, instance, body):
        self.get_admin().mount_disk(args.zone, instance, body)

    def delete_instance(self, args):
        host_info = self.get_host_info(args)
        if args.node_ip is None or host_info['private_ip'] != args.node_ip:
            logging.error("Host {} IP does not match.".format(args.search_pattern))
            return
        self.get_admin().delete_instance(args.zone, args.search_pattern)

    def get_regions(self, args):
        regions_we_know_of = self.get_admin().get_regions()
        if args.network is None:
            return regions_we_know_of
        else:
            # TODO(WESLEY): CHECK ON WHY THIS WASN"T RETURNING ANYTHING
            return list(self.get_admin().network(
                per_region_meta=self.get_per_region_meta(args)).get_network_data(
                    args.network)["regions"].keys())

    def get_zones(self, args):
        """This method returns a map of regions to zones.
        If region is passed in args, the map has exactly one key: args.region.
        """
        regions = [args.region] if args.region else self.get_regions(args)

        result = {}
        metadata = self.get_per_region_meta(args)
        for region in regions:
            result[region] = {}
            result[region]["zones"] = self.get_admin().get_zones(region)
            subnets = self.get_admin().network(
                args.dest_vpc_id, per_region_meta=metadata).get_subnetworks(
                    region)
            result[region]["subnetworks"] = subnets
        return result

    def get_current_host_info(self):
        try:
            return GoogleCloudAdmin.get_current_host_info()
        except YBOpsRuntimeError as e:
            return {"error": get_exception_message(e)}

    def get_instance_types_map(self, args):
        """This method returns a dictionary mapping regions to a dictionary of zones
        mapped to a list of dictionaries containing available instance types and their
        descriptions. If region is passed in, we restrict results to the region and if
        both region and zone are passed in, we restrict to zone.
        """
        regions = args.regions if args.regions else self.get_regions(args)
        region_zones_map = {}
        for r in regions:
            region_zones_map[r] = self.get_admin().get_zones(r)

        result = {}
        for region, zones in region_zones_map.items():
            result[region] = {}
            for zone in zones:
                result[region][zone] = self.get_admin().get_instance_types_by_zone(zone)

        return result

    def get_pricing_map(self):
        return self.get_admin().get_pricing_map()

    def get_spot_pricing(self, args):
        pricing_map = self.get_pricing_map()
        return self.fetch_instance_price_from_map(
            pricing_map, args.instance_type, args.region, preemptible=True)

    def get_compute_image(self, name, preemptible=False):
        return self.get_admin().get_full_image_name(name, preemptible)

    def fetch_instance_price_from_map(self, pricing_map, name, region, preemptible=False):
        name_key = self.get_compute_image(name, preemptible)
        try:
            if region in pricing_map[name_key]:
                price_per_hour = pricing_map[name_key][region]
            else:
                price_per_hour = pricing_map[name_key][region[:-1]]
        except Exception as e:
            raise YBOpsRuntimeError(e)
        return price_per_hour

    def get_os_price_map(self, pricing_map, name, region, numCores, isShared):
        os_price_list = []

        for os in self.FREE_OS_LIST:
            os_map = {
                "os": os,
                "price": self.fetch_instance_price_from_map(pricing_map, name, region)
            }
            os_price_list.append(os_map)

        return os_price_list

    def get_instance_types(self, args):
        region_zones_instances_map = self.get_instance_types_map(args)
        pricing_map = self.get_pricing_map()

        result = {}
        for region, zone_instances_map in region_zones_instances_map.items():
            for zone, instances in zone_instances_map.items():
                for instance in instances:
                    name = instance["name"]
                    if name not in result:
                        compute_image_name = self.get_compute_image(name)
                        if compute_image_name not in pricing_map:
                            continue
                        if "memory" not in pricing_map[compute_image_name]:
                            continue
                        result[name] = {
                            "prices": {},
                            "numCores": instance["guestCpus"],
                            "isShared": instance["isSharedCpu"],
                            "description": instance["description"],
                            "memSizeGb": float(pricing_map[compute_image_name]["memory"])
                        }
                    if region in result[name]["prices"]:
                        continue
                    result[name]["prices"][region] = self.get_os_price_map(pricing_map,
                                                                           name,
                                                                           region,
                                                                           result[name]["numCores"],
                                                                           result[name]["isShared"])

        to_delete_instance_types = []
        for name in result:
            to_delete_regions = []
            for region in result[name]["prices"]:
                for zone in region_zones_instances_map[region]:
                    if name not in region_zones_instances_map[region][zone]:
                        to_delete_regions.append(region)
                        break
            for region in to_delete_regions:
                result[name].pop(region, None)
            if not result[name]:
                to_delete_instance_types.append(name)
        for name in to_delete_instance_types:
            result.pop(name, None)

        return result

    def network_bootstrap(self, args):
        custom_payload = json.loads(args.custom_payload)
        dest_vpc_id = custom_payload.get("destVpcId")
        host_vpc_id = custom_payload.get("hostVpcId")
        per_region_meta = custom_payload.get("perRegionMetadata")
        return self.get_admin().network(dest_vpc_id, host_vpc_id, per_region_meta).bootstrap()

    def network_cleanup(self, args):
        custom_payload = json.loads(args.custom_payload)
        dest_vpc_id = custom_payload.get("destVpcId")
        host_vpc_id = custom_payload.get("hostVpcId")
        self.get_admin().network(
            dest_vpc_id, host_vpc_id, per_region_meta=self.get_per_region_meta(args)).cleanup()
        return {"success": "VPC deleted."}

    def get_host_info(self, args, get_all=False):
        """Override to call the respective GCP specific API for returning hosts by name.

        Required fields in args:
          zone: the zone to search in
          search_pattern: the regex or direct name to search hosts by
        """
        zone = args.zone
        search_pattern = args.search_pattern
        return self.get_admin().get_instances(zone, search_pattern, get_all)

    def get_device_names(self, args):
        # Boot disk is also a persistent disk, so add persistent disks starting at index 1
        if args.volume_type == GCP_SCRATCH:
            disk_name = "local-ssd"
            first_disk = 0
        else:
            disk_name = "persistent-disk"
            first_disk = 1
        return ["disk/by-id/google-{}-{}".format(
            disk_name, first_disk + i) for i in range(args.num_volumes)]

    def update_disk(self, args):
        instance = self.get_host_info(args)
        self.get_admin().update_disk(args, instance['id'])

    def get_per_region_meta(self, args):
        if hasattr(args, "custom_payload") and args.custom_payload:
            metadata = json.loads(args.custom_payload).get("perRegionMetadata")
            if metadata:
                return metadata
        return {}
