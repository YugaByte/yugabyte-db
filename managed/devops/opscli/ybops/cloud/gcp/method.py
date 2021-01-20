#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.cloud.common.method import CreateInstancesMethod, ProvisionInstancesMethod,\
    AbstractMethod, DestroyInstancesMethod, AbstractAccessMethod
from ybops.common.exceptions import YBOpsRuntimeError, get_exception_message
from ybops.utils import validated_key_file, format_rsa_key
from ybops.cloud.gcp.utils import GCP_PERSISTENT, GCP_SCRATCH

import json
import os


class GcpCreateInstancesMethod(CreateInstancesMethod):
    """Subclass for creating instances in GCP. This is responsible for taking in the GCP specific
    flags, such as instance types, subnets, etc.
    """
    SSH_USER = "centos"

    def __init__(self, base_command):
        super(GcpCreateInstancesMethod, self).__init__(base_command)

    def add_extra_args(self):
        super(GcpCreateInstancesMethod, self).add_extra_args()
        self.parser.add_argument("--use_preemptible", action="store_true",
                                 help="If to use preemptible instances.")
        self.parser.add_argument("--volume_type", choices=[GCP_SCRATCH, GCP_PERSISTENT],
                                 default="scratch", help="Storage type for GCP instances.")

    def run_ansible_create(self, args):
        server_type = args.type

        can_ip_forward = (
            server_type.startswith('openvpn-server') or
            server_type.startswith('ipsec-gateway'))

        machine_image = args.machine_image if args.machine_image else \
            self.cloud.get_image(args.region)["selfLink"]

        ssh_keys = None
        if args.private_key_file is not None:
            rsa_key = validated_key_file(args.private_key_file)
            public_key = format_rsa_key(rsa_key, public_key=True)
            ssh_keys = "{}:{} {}".format(self.SSH_USER, public_key, self.SSH_USER)

        self.cloud.get_admin().create_instance(
            args.region, args.zone, args.cloud_subnet, args.search_pattern, args.instance_type,
            server_type, args.use_preemptible, can_ip_forward, machine_image, args.num_volumes,
            args.volume_type, args.volume_size, args.boot_disk_size_gb, args.assign_public_ip,
            ssh_keys)


class GcpProvisionInstancesMethod(ProvisionInstancesMethod):
    """Subclass for provisioning instances in GCP. Sets up the proper Create method to point to the
    GCP specific one.
    """
    def __init__(self, base_command):
        super(GcpProvisionInstancesMethod, self).__init__(base_command)

    def setup_create_method(self):
        """Override to get the wiring to the proper method.
        """
        self.create_method = GcpCreateInstancesMethod(self.base_command)

    def update_ansible_vars_with_args(self, args):
        super(GcpProvisionInstancesMethod, self).update_ansible_vars_with_args(args)
        self.extra_vars["device_names"] = self.cloud.get_device_names(args)
        self.extra_vars["mount_points"] = self.cloud.get_mount_points_csv(args)


class GcpDestroyInstancesMethod(DestroyInstancesMethod):
    """Subclass for deleting instances in GCP. Uses the API to delete instance bypassing Ansible.
    """
    def __init__(self, base_command):
        super(GcpDestroyInstancesMethod, self).__init__(base_command)

    def callback(self, args):
        self.cloud.delete_instance(args)


class GcpQueryRegionsMethod(AbstractMethod):
    def __init__(self, base_command):
        super(GcpQueryRegionsMethod, self).__init__(base_command, "regions")

    def callback(self, args):
        print(json.dumps(self.cloud.get_regions(args)))


class GcpQueryVpcMethod(AbstractMethod):
    def __init__(self, base_command):
        super(GcpQueryVpcMethod, self).__init__(base_command, "vpc")

    def add_extra_args(self):
        super(GcpQueryVpcMethod, self).add_extra_args()
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        print(json.dumps(self.cloud.query_vpc(args)))


class GcpQueryZonesMethod(AbstractMethod):
    def __init__(self, base_command):
        super(GcpQueryZonesMethod, self).__init__(base_command, "zones")

    def add_extra_args(self):
        super(GcpQueryZonesMethod, self).add_extra_args()
        self.parser.add_argument(
            "--dest_vpc_id", default=None,
            help="Custom VPC to get zone and subnet info for.")
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        print(json.dumps(self.cloud.get_zones(args)))


class GcpQueryInstanceTypesMethod(AbstractMethod):
    def __init__(self, base_command):
        super(GcpQueryInstanceTypesMethod, self).__init__(base_command, "instance_types")

    def add_extra_args(self):
        super(GcpQueryInstanceTypesMethod, self).add_extra_args()
        self.parser.add_argument("--regions", nargs='+')
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        print(json.dumps(self.cloud.get_instance_types(args)))


class GcpQueryCurrentHostMethod(AbstractMethod):
    def __init__(self, base_command):
        super(GcpQueryCurrentHostMethod, self).__init__(base_command, "current-host")
        # We do not need cloud credentials to query metadata.
        self.need_validation = False

    def callback(self, args):
        print(json.dumps(self.cloud.get_current_host_info()))


class GcpQueryPreemptibleInstanceMethod(AbstractMethod):
    def __init__(self, base_command):
        super(GcpQueryPreemptibleInstanceMethod, self).__init__(base_command, "spot-pricing")

    def add_extra_args(self):
        super(GcpQueryPreemptibleInstanceMethod, self).add_extra_args()
        self.parser.add_argument("--instance_type", required=True,
                                 help="The instance type to get pricing info for")

    def callback(self, args):
        try:
            if args.region is None:
                raise YBOpsRuntimeError("Must specify a region to query spot price")
            print(json.dumps({'SpotPrice': self.cloud.get_spot_pricing(args)}))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class GcpAccessAddKeyMethod(AbstractAccessMethod):
    def __init__(self, base_command):
        super(GcpAccessAddKeyMethod, self).__init__(base_command, "add-key")

    def callback(self, args):
        (private_key_file, public_key_file) = self.validate_key_files(args)
        print(json.dumps({"private_key": private_key_file, "public_key": public_key_file}))


class GcpAbstractNetworkMethod(AbstractMethod):
    def __init__(self, base_command, method_name):
        super(GcpAbstractNetworkMethod, self).__init__(base_command, method_name)

    def add_extra_args(self):
        super(GcpAbstractNetworkMethod, self).add_extra_args()
        self.parser.add_argument("--metadata_override", required=False,
                                 help="A custom YML metadata override file.")

    def preprocess_args(self, args):
        super(GcpAbstractNetworkMethod, self).preprocess_args(args)
        if args.metadata_override:
            self.cloud.update_metadata(args.metadata_override)


class GcpNetworkBootstrapMethod(GcpAbstractNetworkMethod):
    def __init__(self, base_command):
        super(GcpNetworkBootstrapMethod, self).__init__(base_command, "bootstrap")

    def add_extra_args(self):
        """Setup the CLI options network bootstrap."""
        super(GcpNetworkBootstrapMethod, self).add_extra_args()
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.network_bootstrap(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class GcpNetworkCleanupMethod(GcpAbstractNetworkMethod):
    def __init__(self, base_command):
        super(GcpNetworkCleanupMethod, self).__init__(base_command, "cleanup")

    def add_extra_args(self):
        """Setup the CLI options network cleanup."""
        super(GcpNetworkCleanupMethod, self).add_extra_args()
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.network_cleanup(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class GcpNetworkQueryMethod(GcpAbstractNetworkMethod):
    def __init__(self, base_command):
        super(GcpNetworkQueryMethod, self).__init__(base_command, "query")

    def add_extra_args(self):
        """Setup the CLI options network queries."""
        super(GcpNetworkQueryMethod, self).add_extra_args()
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.query_vpc(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))
