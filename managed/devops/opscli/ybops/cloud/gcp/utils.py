# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

import httplib2
import logging
import os
import requests
import six
import socket
import time


from googleapiclient import discovery
from googleapiclient.errors import HttpError
import oauth2client
from six.moves import http_client

from ybops.common.exceptions import YBOpsRuntimeError
from ybops.cloud.common.utils import request_retry_decorator


RESOURCE_BASE_URL = "https://www.googleapis.com/compute/beta/projects/"
REGIONS_RESOURCE_URL_FORMAT = RESOURCE_BASE_URL + "{}/regions/{}"
PRICING_JSON_URL = "https://cloudpricingcalculator.appspot.com/static/data/pricelist.json"
IMAGE_NAME_PREFIX = "CP-COMPUTEENGINE-VMIMAGE-"
IMAGE_NAME_PREEMPTIBLE_SUFFIX = "-PREEMPTIBLE"
OPERATION_WAIT_TIME = 5
MAX_NUM_RETRY_COUNT = 120
LIST_MAX_RESULTS = 500


YB_NETWORK_NAME = "yb-gcp-network"
YB_SUBNET_FORMAT = "yb-subnet-{}"
YB_PEERING_CONNECTION_FORMAT = "yb-peering-{}-with-{}"
YB_FIREWALL_NAME = "yb-internal-firewall"
YB_FIREWALL_TARGET_TAGS = "cluster-server"


GCP_SCRATCH = "scratch"
GCP_PERSISTENT = "persistent"


# Code 429 does not have a name in httplib.
TOO_MANY_REQUESTS = 429


def gcp_exception_handler(e):
    """GCP specific exception handler. Modeled after:
    http://testcompany.info/google-cloud-sdk/lib/third_party/apitools/base/py/http_wrapper.py

    Args:
        e: the exception that was raised by the underlying API call that just failed.
    Returns:
        True if this exception can be retried, False otherwise.
    """
    # Transport failures
    if isinstance(e, (http_client.BadStatusLine,
                      http_client.IncompleteRead,
                      http_client.ResponseNotReady)):
        logging.warn('Caught HTTP error %s, retrying: %s', type(e).__name__, e)
    elif isinstance(e, socket.error):
        # Note: this also catches ssl.SSLError flavors of:
        # ssl.SSLError: ('The read operation timed out',)
        logging.warn('Caught socket error, retrying: %s', e)
    elif isinstance(e, socket.gaierror):
        logging.warn('Caught socket address error, retrying: %s', e)
    elif isinstance(e, socket.timeout):
        logging.warn('Caught socket timeout error, retrying: %s', e)
    elif isinstance(e, httplib2.ServerNotFoundError):
        logging.warn('Caught server not found error, retrying: %s', e)
    elif isinstance(e, ValueError):
        # oauth2client tries to JSON-decode the response, which can result
        # in a ValueError if the response was invalid. Until that is fixed in
        # oauth2client, need to handle it here.
        logging.warn('Response content was invalid (%s), retrying', e)
    elif (isinstance(e, oauth2client.client.HttpAccessTokenRefreshError) and
          (e.status == TOO_MANY_REQUESTS or
           e.status >= 500)):
        logging.warn('Caught transient credential refresh error (%s), retrying', e)
    else:
        return False
    return True


def gcp_request_limit_retry(fn):
    """A decorator for retrying GCP operations in case of expected transient errors.
    """
    return request_retry_decorator(fn, gcp_exception_handler)


def get_firewall_tags():
    return os.environ.get('YB_FIREWALL_TAGS', YB_FIREWALL_TARGET_TAGS).split(',')


class GcpMetadata():
    METADATA_URL_BASE = "http://metadata.google.internal/computeMetadata/v1"
    CUSTOM_HEADERS = {
        "Metadata-Flavor": "Google"
    }

    @staticmethod
    def _query_endpoint(endpoint):
        try:
            url = "{}/{}".format(GcpMetadata.METADATA_URL_BASE, endpoint)
            req = requests.get(url, headers=GcpMetadata.CUSTOM_HEADERS, timeout=2)
            return req.content if req.status_code == requests.codes.ok else None
        except requests.exceptions.ConnectionError as e:
            return None

    @staticmethod
    def project():
        return GcpMetadata._query_endpoint("/project/project-id")

    @staticmethod
    def host_project():
        network_data = GcpMetadata._query_endpoint("instance/network-interfaces/0/network")
        try:
            # Network data is of format projects/PROJECT_NUMBER/networks/NETWORK_NAME
            return network_data.split('/')[1]
        except (IndexError, AttributeError):
            return None

    @staticmethod
    def network():
        return GcpMetadata._query_endpoint("instance/network-interfaces/0/network")

    @staticmethod
    def service_accounts():
        return GcpMetadata._query_endpoint("instance/service-accounts/")


class Waiter():
    def __init__(self, project, compute):
        self.project = project
        self.compute = compute

    def wait(self, operation, region=None, zone=None):
        # This allows easier chaining of waits on functions that are NOOPs if items already exist.
        if operation is None:
            return
        retry_count = 0
        name = operation["name"]
        while retry_count < MAX_NUM_RETRY_COUNT:
            if zone is not None:
                cmd = self.compute.zoneOperations().get(
                    project=self.project,
                    zone=zone,
                    operation=name)
            elif region is not None:
                cmd = self.compute.regionOperations().get(
                    project=self.project,
                    region=region,
                    operation=name)
            else:
                cmd = self.compute.globalOperations().get(
                    project=self.project,
                    operation=name)
            result = cmd.execute()

            if result['status'] == 'DONE':
                if 'error' in result:
                    raise YBOpsRuntimeError(result['error'])
                return result

            time.sleep(OPERATION_WAIT_TIME)
            retry_count += 1

        raise YBOpsRuntimeError("Operation {} timed out".format(name))


class NetworkManager():
    def __init__(self, project, compute, metadata, dest_vpc_id, host_vpc_id, per_region_meta):
        self.project = project
        self.compute = compute
        self.metadata = metadata
        # This dictates if we want to provision a new network or just query an existing one.
        self.dest_vpc_id = dest_vpc_id
        # This will be the network we currently are in and that we will be peering with.
        # TODO: fix the network/project combos
        # Allow setting host_vpc_id and check if you do not have a target, meaning you are
        # asked to bootstrap, then the host's project ID matches self.project, else we'll try to
        # peer across projects...
        self.host_vpc_id = host_vpc_id if host_vpc_id is not None else GcpMetadata.network()
        if self.host_vpc_id is not None:
            self.host_vpc_id = self.host_vpc_id.split("/")[-1]
        self.per_region_meta = per_region_meta

        self.waiter = Waiter(self.project, self.compute)

    def network_info_as_json(self, network_name, region_to_subnet_map):
        return {
            "network": network_name,
            "regions": region_to_subnet_map
        }

    def get_network_data(self, network_name):
        # If user has specified network data through per_region_meta, then do not validate any
        # data through GCP API and simply return the relevant fields.
        output_region_to_subnet_map = {}
        for region in self.per_region_meta.keys():
            output_region_to_subnet_map[region] = self.per_region_meta.get(
                region, {}).get("subnetId")

        if output_region_to_subnet_map:
            return self.network_info_as_json(network_name, output_region_to_subnet_map)

        networks = self.get_networks(network_name)
        if len(networks) == 0:
            raise YBOpsRuntimeError("Invalid target VPC: {}".format(network_name))
        network = networks[0]

        subnet_map = self.get_region_subnet_map(network.get("selfLink"))
        for region_name, scope in subnet_map.items():
            region = region_name.split("/")[1]
            subnets = [s["name"] for s in scope.get("subnetworks", [])]
            if len(subnets) > 0:
                output_region_to_subnet_map[region] = subnets
        return self.network_info_as_json(network_name, output_region_to_subnet_map)

    def bootstrap(self):
        # If given a target VPC, then don't create anything.
        if self.dest_vpc_id:
            return self.get_network_data(self.dest_vpc_id)
        # If we were not given a target VPC, then we'll try to provision our custom network.
        networks = self.get_networks(YB_NETWORK_NAME)
        if len(networks) > 0:
            network_url = networks[0].get("selfLink")
        else:
            # Create the network if it didn't already exist.
            op = self.waiter.wait(self.create_network())
            network_url = op.get("targetLink")
        ops_by_region = {}
        # List all relevant subnets.
        output_region_to_subnet_map = {}
        subnet_map = self.get_region_subnet_map(network_url)
        for region_name, scope in subnet_map.items():
            region = region_name.split("/")[1]
            # Ignore regions not asked to bootstrap.
            if region not in self.metadata["regions"]:
                continue
            custom_subnet_name = YB_SUBNET_FORMAT.format(region)
            output_region_to_subnet_map[region] = [custom_subnet_name]
            subnet_names = [s["name"] for s in scope.get("subnetworks", [])]
            if custom_subnet_name not in subnet_names:
                ops_by_region[region] = self.create_subnetwork(
                    network_url, region, custom_subnet_name)
        # Wait for all created subnets.
        for r, op in ops_by_region.items():
            self.waiter.wait(op, region=r)
        peer_network_url = None
        # Setup the VPC peering.
        if self.host_vpc_id is not None:
            peer_networks = self.get_networks(self.host_vpc_id)
            if len(peer_networks) != 1:
                raise YBOpsRuntimeError("Invalid peer VPC network: {}".format(
                    self.host_vpc_id))
            # Prepare the connection to the peer
            self.setup_peering(YB_NETWORK_NAME, self.host_vpc_id)
            peer_network_url = peer_networks[0].get("selfLink")
        # Setup the firewall rules now that we have all subnets.
        self.setup_firewall_rules(network_url, peer_network_url)
        return self.network_info_as_json(YB_NETWORK_NAME, output_region_to_subnet_map)

    def cleanup(self):
        if self.dest_vpc_id:
            return
        # Check if network exists.
        networks = self.get_networks(YB_NETWORK_NAME)
        if len(networks) == 0:
            return
        network = networks[0]
        network_url = network.get("selfLink")
        ops_by_region = {}
        # List all relevant subnets.
        subnet_map = self.get_region_subnet_map(network_url)
        for region_name, scope in subnet_map.items():
            region = region_name.split("/")[1]
            subnets = scope.get("subnetworks", [])
            for s in subnets:
                subnet = s["name"]
                # Delete each subnet and keep track of op by region to wait for.
                op = self.delete_subnetwork(region, subnet)
                ops_by_region.setdefault(region, []).append(op)
        # Wait for all subnet deletions.
        for region, ops in ops_by_region.items():
            for op in ops:
                self.waiter.wait(op, region=region)
        # Remove all firewall rules.
        self.teardown_firewall_rules(network_url)
        # Remove the VPC peerings.
        if self.host_vpc_id is not None:
            self.teardown_peering(YB_NETWORK_NAME, self.host_vpc_id)
        # Delete the actual network.
        self.waiter.wait(self.delete_network(YB_NETWORK_NAME))

    def get_networks(self, network_name):
        filter = "(name eq {})".format(network_name)
        networks = self.compute.networks().list(project=self.project, filter=filter).execute()
        return networks.get("items", [])

    def create_network(self):
        body = {
            "name": YB_NETWORK_NAME,
            "autoCreateSubnetworks": False
        }
        return self.compute.networks().insert(project=self.project, body=body).execute()

    def delete_network(self, network_name):
        networks = self.get_networks(network_name)
        if len(networks) == 0:
            return
        # Delete the actual network.
        return self.compute.networks().delete(
            project=self.project, network=network_name).execute()

    def get_subnetworks(self, region, name=None):
        saved_subnet = self.per_region_meta.get(region, {}).get("subnetId")
        if saved_subnet:
            return saved_subnet

        network_name = YB_NETWORK_NAME
        if self.dest_vpc_id is not None:
            network_name = self.dest_vpc_id
        networks = self.get_networks(network_name)
        if len(networks) == 0:
            raise YBOpsRuntimeError("Invalid network: {}".format(network_name))
        filter = "(network eq {})".format(networks[0].get("selfLink"))
        if name is not None:
            filter += "(name eq {})".format(name)
        subnets = self.compute.subnetworks().list(
            project=self.project, region=region, filter=filter).execute()
        return [s["name"] for s in subnets.get("items", [])]

    def get_region_subnet_map(self, network_url):
        return self.compute.subnetworks().aggregatedList(
            project=self.project,
            filter="(network eq {})".format(network_url)
        ).execute().get("items", {})

    def create_subnetwork(self, network_url, region, name):
        cidr = self.metadata["region_cidr_format"].format(
            self.metadata["regions"][region]["cidr_prefix"])
        body = {
            "name": name,
            "network": network_url,
            "region": region,
            "ipCidrRange": cidr
        }
        return self.compute.subnetworks().insert(
            project=self.project, region=region, body=body).execute()

    def delete_subnetwork(self, region, subnet):
        return self.compute.subnetworks().delete(
            project=self.project, region=region, subnetwork=subnet).execute()

    def setup_firewall_rules(self, network_url, peer_network_url=None):
        ip_cidr_list = []
        # Get all the relevant subnets and their CIDR blocks.
        for url in [network_url, peer_network_url]:
            # Skip the peer_network_url if we did not set up peering.
            if url is None:
                continue
            data = self.compute.subnetworks().aggregatedList(
                project=self.project,
                filter="(network eq {})".format(url)).execute()
            data = data.get("items", {})
            subnet_cidrs = [s["ipCidrRange"]
                            for i in data.values()
                            for s in i.get("subnetworks", [])]
            ip_cidr_list.extend(subnet_cidrs)
        self.waiter.wait(self.update_firewall_rules(
            network_url, ip_cidr_list, YB_FIREWALL_NAME))

    def teardown_firewall_rules(self, network_url):
        firewall_name = YB_FIREWALL_NAME
        firewalls = self.get_firewall_rules(network_url, firewall_name)
        if len(firewalls) == 0:
            return
        return self.waiter.wait(self.compute.firewalls().delete(
          project=self.project,
          firewall=firewall_name).execute())

    def get_firewall_rules(self, network_url, name):
        filter = "(network eq {})(name eq {})".format(network_url, name)
        firewalls = self.compute.firewalls().list(
            project=self.project,
            filter=filter).execute()
        return firewalls.get("items", [])

    def update_firewall_rules(self, network_url, ip_cidr_list, firewall_name):
        # If we have the firewall rule already, we need an update, else insert.
        firewalls = self.get_firewall_rules(network_url, firewall_name)
        firewall_exists = len(firewalls) != 0
        # Setup the body of the request.
        body = {
            "name": firewall_name,
            "network": network_url,
            "description": "Open up all ports for internal IPs",
            "targetTags": get_firewall_tags(),
            "sourceRanges": ip_cidr_list,
            "allowed": [{"IPProtocol": p} for p in ["tcp", "udp", "icmp"]]
            }
        fw_object = self.compute.firewalls()
        if firewall_exists:
            # Only update if any of these CIDRs are not already there or if targetFlags should
            # be updated.
            firewall = firewalls[0]
            source_cidrs = set(firewall.get("sourceRanges"))
            new_cidrs = set(ip_cidr_list)
            union_cidrs = source_cidrs | new_cidrs

            current_tags = set(firewall.get("targetTags"))
            new_tags = set(get_firewall_tags())
            union_tags = current_tags | new_tags
            if source_cidrs != union_cidrs or current_tags != union_tags:
                body["sourceRanges"] = list(union_cidrs)
                body["targetTags"] = list(union_tags)
                # Use 'fw_object.update' and not 'fw_object.patch' to update firewall metadata
                # without removing pre-existing info.
                return fw_object.update(
                    project=self.project,
                    firewall=firewall_name,
                    body=body).execute()
            return None
        else:
            return fw_object.insert(
                project=self.project,
                body=body).execute()

    def setup_peering(self, src_network_name, dest_network_name):
        src_network = self.get_networks(src_network_name)
        if len(src_network) != 1:
            raise YBOpsRuntimeError("Invalid network peer: {}".format(src_network_name))
        src_network = src_network[0]
        dest_network = self.get_networks(dest_network_name)
        if len(dest_network) != 1:
            raise YBOpsRuntimeError("Invalid network peer: {}".format(dest_network_name))
        dest_network = dest_network[0]
        src_peerings = [p.get("network") for p in src_network.get("peerings", [])]
        dest_peerings = [p.get("network") for p in dest_network.get("peerings", [])]
        body = {
            # TODO: this is mandatory in the API for current version of peering...
            "autoCreateRoutes": True
        }
        ops_to_wait_on = []
        if dest_network.get("selfLink") not in src_peerings:
            body.update({
                "name": YB_PEERING_CONNECTION_FORMAT.format(
                    src_network_name, dest_network_name),
                "peerNetwork": dest_network.get("selfLink")
            })
            ops_to_wait_on.append(self.compute.networks().addPeering(
                project=self.project,
                network=src_network_name,
                body=body).execute())
        if src_network.get("selfLink") not in dest_peerings:
            body.update({
                "name": YB_PEERING_CONNECTION_FORMAT.format(
                    dest_network_name, src_network_name),
                "peerNetwork": src_network.get("selfLink")
            })
            ops_to_wait_on.append(self.compute.networks().addPeering(
                project=self.project,
                network=dest_network_name,
                body=body).execute())
        for op in ops_to_wait_on:
            self.waiter.wait(op)

    def teardown_peering(self, src_network_name, dest_network_name):
        # TODO: dedup handling of peering.
        src_network = self.get_networks(src_network_name)
        if len(src_network) != 1:
            raise YBOpsRuntimeError("Invalid network peer: {}".format(src_network_name))
        src_network = src_network[0]
        dest_network = self.get_networks(dest_network_name)
        if len(dest_network) != 1:
            raise YBOpsRuntimeError("Invalid network peer: {}".format(dest_network_name))
        dest_network = dest_network[0]
        src_peerings = [p.get("network") for p in src_network.get("peerings", [])]
        dest_peerings = [p.get("network") for p in dest_network.get("peerings", [])]
        if dest_network.get("selfLink") in src_peerings:
            body = {
                "name": YB_PEERING_CONNECTION_FORMAT.format(
                    src_network_name, dest_network_name)
            }
            self.waiter.wait(self.compute.networks().removePeering(
                project=self.project,
                network=src_network_name,
                body=body).execute())
        if src_network.get("selfLink") in dest_peerings:
            body = {
                "name": YB_PEERING_CONNECTION_FORMAT.format(
                    dest_network_name, src_network_name)
            }
            self.waiter.wait(self.compute.networks().removePeering(
                project=self.project,
                network=dest_network_name,
                body=body).execute())


class GoogleCloudAdmin():
    def __init__(self, metadata):
        # The following environment variables are required, if we want to use custom credentials:
        #   GCE_PROJECT: set to the name of the project
        #   GOOGLE_APPLICATION_CREDENTIALS: set to the path of the credentials json file
        # If these are not provided, then get_application_default will try to use the service
        # account associated with the instance we're running on, if one exists.
        self.credentials = oauth2client.client.GoogleCredentials.get_application_default()
        self.compute = discovery.build("compute", "beta", credentials=self.credentials)
        # If we have specified a GCE_PROJECT, use that, else, try the instance metadata, else fail.
        self.project = os.environ.get("GCE_PROJECT") or GcpMetadata.project()
        if self.project is None:
            raise YBOpsRuntimeError(
                "Could not determine GCP project from either env or server metadata!")

        self.metadata = metadata
        self.waiter = Waiter(self.project, self.compute)

    def network(self, dest_vpc_id=None, host_vpc_id=None, per_region_meta={}):
        return NetworkManager(
            self.project, self.compute, self.metadata, dest_vpc_id, host_vpc_id, per_region_meta)

    @staticmethod
    def get_current_host_info():
        network = GcpMetadata.network()
        host_project = GcpMetadata.host_project()
        project = GcpMetadata.project()
        if network is None or project is None:
            raise YBOpsRuntimeError("Host not in GCP.")
        return {
            "network": network.split("/")[-1],
            "host_project": host_project,
            "project": project
        }

    def get_image(self, image, project=None):
        return self.compute.images().get(
            project=project if project else self.project,
            image=image).execute()

    def get_full_image_name(self, name, preemptible=False):
        return IMAGE_NAME_PREFIX + name.upper() + \
            (IMAGE_NAME_PREEMPTIBLE_SUFFIX if preemptible else "")

    def create_disk(self, zone, body):
        operation = self.compute.disks().insert(project=self.project,
                                                zone=zone,
                                                body=body).execute()
        self.waiter.wait(operatio, zonen)

    def mount_disk(self, zone, instance, body):
        operation = self.compute.instances().attachDisk(project=self.project,
                                                        zone=zone,
                                                        instance=instance,
                                                        body=body).execute()
        self.waiter.wait(operation, zone)

    def update_disk(self, args, instance):
        zone = args.zone
        instance_info = self.compute.instances().get(project=self.project, zone=zone,
                                                     instance=instance).execute()
        body = {
            "sizeGb": args.volume_size
        }
        print("Got instance info: " + str(instance_info))
        for disk in instance_info['disks']:
            # Bootdisk should be ignored.
            if disk['index'] != 0:
                # The source is the complete URL of the disk, with the last
                # component being the name.
                disk_name = disk['source'].split('/')[-1]
                print("Updating disk " + disk_name)
                operation = self.compute.disks().resize(project=self.project,
                                                        zone=zone,
                                                        disk=disk_name,
                                                        body=body).execute()
                self.waiter.wait(operation, zone=zone)

    def delete_instance(self, zone, instance_name):
        operation = self.compute.instances().delete(project=self.project,
                                                    zone=zone,
                                                    instance=instance_name).execute()
        self.waiter.wait(operation, zone=zone)

    def query_vpc(self, region):
        """
        TODO: implement this once we get GCP custom region support...
        """
        return {}

    def get_regions(self):
        return list(self.metadata['regions'].keys())
        """
        # TODO: revert back to this if we decide to go back to the full list of regions from GCP.
        filter = "(status eq UP)"
        fields = "items(name)"
        regions = self.compute.regions().list(project=self.project,
                                              filter=filter,
                                              fields=fields).execute()

        return [r["name"] for r in regions.get("items", [])]
        """

    def get_zones(self, region, max_results=LIST_MAX_RESULTS):
        return ["{}-{}".format(region, zone)
                for zone in self.metadata['regions'].get(region, {}).get('zones', [])]

    def get_instance_types_by_zone(self, zone):
        fields = "items(name,description,guestCpus,memoryMb,isSharedCpu)"
        instance_type_items = self.compute.machineTypes().list(project=self.project,
                                                               fields=fields,
                                                               zone=zone).execute()
        return instance_type_items["items"]

    def get_pricing_map(self, ):
        # Requests encounters an SSL bug when run on portal, so verify is set to false
        pricing_url_response = requests.get(PRICING_JSON_URL, verify=False)
        pricing_map_all = pricing_url_response.json()["gcp_price_list"]
        pricing_map = {}
        for key in pricing_map_all:
            if key.startswith(IMAGE_NAME_PREFIX):
                pricing_map[key] = pricing_map_all[key]
        return pricing_map

    @gcp_request_limit_retry
    def get_instances(self, zone, instance_name, get_all=False):
        # TODO: filter should work to do (zone eq args.zone), but it doesn't right now...
        filter = "(status eq RUNNING)"
        if instance_name is not None:
            filter += " (name eq {})".format(instance_name)
        instances = self.compute.instances().aggregatedList(
            project=self.project,
            filter=filter,
            maxResults=(LIST_MAX_RESULTS if get_all else 1)).execute()
        instances = instances.get("items", [])
        if zone is not None:
            instances = instances.get("zones/{}".format(zone), None)
            if instances is None:
                raise YBOpsRuntimeError("Invalid zone: {}".format(zone))
            else:
                instances = instances.get("instances", [])
        else:
            instances = [i for sublist in instances.values() for i in sublist.get("instances", [])]
        if len(instances) == 0:
            return None
        results = []
        for data in instances:
            metadata = data.get("metadata", {}).get("items", {})
            server_types = [i["value"] for i in metadata if i["key"] == "server_type"]
            interface = data.get("networkInterfaces", [None])[0]
            access_config = interface.get("accessConfigs", [None])[0]
            zone = data["zone"].split("/")[-1]
            region = zone[:-2]
            machine_type = data["machineType"].split("/")[-1]
            public_ip = access_config.get("natIP") if access_config else None
            result = dict(
                id=data.get("name"),
                name=data.get("name"),
                public_ip=public_ip,
                private_ip=interface.get("networkIP"),
                region=region,
                zone=zone,
                instance_type=machine_type,
                server_type=server_types[0] if server_types else None,
                launched_by=None,
                launch_time=data.get("creationTimestamp")
                )
            if not get_all:
                return result
            results.append(result)
        return results

    def create_instance(self, region, zone, cloud_subnet, instance_name, instance_type, server_type,
                        use_preemptible, can_ip_forward, machine_image, num_volumes, volume_type,
                        volume_size, boot_disk_size_gb=None, assign_public_ip=True, ssh_keys=None):
        network_name = os.environ.get("CUSTOM_GCE_NETWORK", YB_NETWORK_NAME)
        # Name of the project that target VPC network belongs to.
        host_project = os.environ.get("GCE_HOST_PROJECT", self.project)

        boot_disk_json = {
            "autoDelete": True,
            "boot": True,
            "index": 0,
        }
        boot_disk_init_params = {}
        boot_disk_init_params["sourceImage"] = machine_image
        if boot_disk_size_gb is not None:
            # Default: 10GB
            boot_disk_init_params["diskSizeGb"] = boot_disk_size_gb
        boot_disk_json["initializeParams"] = boot_disk_init_params

        accessConfigs = [{"natIP": None}] if assign_public_ip else None

        body = {
            "canIpForward": can_ip_forward,
            "disks": [boot_disk_json],
            "machineType": "zones/{}/machineTypes/{}".format(
                zone, instance_type),
            "metadata": {
                "items": [
                    {
                        "key": "server_type",
                        "value": server_type
                    },
                    {
                        "key": "ssh-keys",
                        "value": ssh_keys
                    }
                ]
            },
            "name": instance_name,
            "networkInterfaces": [{
                "accessConfigs": accessConfigs,
                "subnetwork": "projects/{}/regions/{}/subnetworks/{}".format(
                    host_project, region, cloud_subnet)
            }],
            "tags": {
                "items": get_firewall_tags()
            },
            "scheduling": {
              "preemptible": use_preemptible
            }
        }

        initial_params = {}
        if volume_type == GCP_SCRATCH:
            initial_params.update({
                "diskType": "zones/{}/diskTypes/local-ssd".format(zone)
            })
            # Default size: 375GB
        elif volume_type == GCP_PERSISTENT:
            initial_params.update({
                "diskType": "zones/{}/diskTypes/pd-ssd".format(zone),
                "sourceImage": None,
                "diskSizeGb": volume_size
            })
        disk_config = {
            "autoDelete": True,
            "type": volume_type,
            "initializeParams": initial_params
        }

        for i in range(num_volumes):
            body["disks"].append(disk_config)

        logging.info("[app] About to create GCP VM {} in region {}.".format(
            instance_name, region))
        self.waiter.wait(self.compute.instances().insert(
            project=self.project,
            zone=zone,
            body=body).execute(), zone=zone)
        logging.info("[app] Created GCP VM {}".format(instance_name))
