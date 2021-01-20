#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.cloud.common.method import ListInstancesMethod, CreateInstancesMethod, \
    ProvisionInstancesMethod, DestroyInstancesMethod, AbstractMethod, \
    AbstractAccessMethod, AbstractNetworkMethod, AbstractInstancesMethod
from ybops.common.exceptions import YBOpsRuntimeError, get_exception_message
from ybops.cloud.aws.utils import get_yb_sg_name, create_dns_record_set, edit_dns_record_set, \
    delete_dns_record_set, list_dns_record_set

import json
import os
import logging
import glob
import subprocess


class AwsListInstancesMethod(ListInstancesMethod):
    """Subclass for listing instances in AWS. Currently doesn't provide any extra functionality.
    """
    def __init__(self, base_command):
        super(AwsListInstancesMethod, self).__init__(base_command)


class AwsCreateInstancesMethod(CreateInstancesMethod):
    """Subclass for creating instances in AWS. This is responsible for taking in the AWS specific
    flags, such as VPCs, AMIs and more.
    """
    def __init__(self, base_command):
        super(AwsCreateInstancesMethod, self).__init__(base_command)

    def add_extra_args(self):
        """Setup the CLI options for creating instances.
        """
        super(AwsCreateInstancesMethod, self).add_extra_args()
        self.parser.add_argument("--key_pair_name", default=os.environ.get("YB_EC2_KEY_PAIR_NAME"),
                                 help="AWS Key Pair name")
        self.parser.add_argument("--security_group_id", default=None,
                                 help="AWS comma delimited security group IDs.")
        self.parser.add_argument("--volume_type", choices=["gp2", "io1"], default="gp2",
                                 help="Volume type for volumes on EBS-backed instances.")
        self.parser.add_argument("--spot_price", default=None,
                                 help="Spot price for each instance (if desired)")
        self.parser.add_argument("--cmk_res_name", help="CMK arn to enable encrypted EBS volumes.")
        self.parser.add_argument("--iam_profile_arn", help="ARN string for IAM instance profile")

    def preprocess_args(self, args):
        super(AwsCreateInstancesMethod, self).preprocess_args(args)
        if args.region is None:
            raise YBOpsRuntimeError("Must specify a region!")
        # TODO: better handling of this...
        if args.machine_image is None:
            # Update the machine_image with the ami_id for this version.
            args.machine_image = self.cloud.get_image(region=args.region).get(args.region)

    def callback(self, args):
        # These are to be used in the provision part for now.
        self.extra_vars.update({
            "aws_key_pair_name": args.key_pair_name,
        })
        if args.security_group_id is not None:
            self.extra_vars.update({
                "aws_security_group_id": args.security_group_id
            })
        else:
            self.extra_vars.update({
                "aws_security_group": get_yb_sg_name(args.region)
            })
        if args.spot_price is not None:
            self.extra_vars.update({
                "aws_spot_price": args.spot_price
            })
        if args.instance_tags is not None:
            self.extra_vars.update({
                "instance_tags": args.instance_tags
            })

        super(AwsCreateInstancesMethod, self).callback(args)

    def run_ansible_create(self, args):
        # TODO: do we need this?
        self.update_ansible_vars(args)
        self.cloud.create_instance(args)


class AwsProvisionInstancesMethod(ProvisionInstancesMethod):
    """Subclass for provisioning instances in AWS. Setups the proper Create method to point to the
    AWS specific one.
    """
    def __init__(self, base_command):
        super(AwsProvisionInstancesMethod, self).__init__(base_command)

    def setup_create_method(self):
        """Override to get the wiring to the proper method.
        """
        self.create_method = AwsCreateInstancesMethod(self.base_command)

    def add_extra_args(self):
        super(AwsProvisionInstancesMethod, self).add_extra_args()
        self.parser.add_argument("--use_chrony", action="store_true",
                                 help="Whether to use chrony instead of NTP.")

    def update_ansible_vars_with_args(self, args):
        super(AwsProvisionInstancesMethod, self).update_ansible_vars_with_args(args)
        self.extra_vars["use_chrony"] = args.use_chrony
        self.extra_vars["device_names"] = self.cloud.get_device_names(args)
        self.extra_vars["mount_points"] = self.cloud.get_mount_points_csv(args)
        self.extra_vars["cmk_res_name"] = args.cmk_res_name


class AwsDestroyInstancesMethod(DestroyInstancesMethod):
    """Subclass for destroying an instance in AWS, we fetch the host info and update the extra_vars
    with necessary parameters
    """
    def __init__(self, base_command):
        super(AwsDestroyInstancesMethod, self).__init__(base_command)

    def callback(self, args):
        host_info = self.cloud.get_host_info(args, private_ip=args.node_ip)
        if not host_info:
            logging.error("Host {} does not exists.".format(args.search_pattern))
            return

        self.extra_vars.update({
            "cloud_subnet": host_info["subnet"],
            "cloud_region": host_info["region"],
            "private_ip": host_info['private_ip']
        })

        super(AwsDestroyInstancesMethod, self).callback(args)


class AwsTagsMethod(AbstractInstancesMethod):
    def __init__(self, base_command):
        super(AwsTagsMethod, self).__init__(base_command, "tags")

    def add_extra_args(self):
        super(AwsTagsMethod, self).add_extra_args()
        self.parser.add_argument("--remove_tags", required=False,
                                 help="Tag keys to remove.")

    def callback(self, args):
        self.cloud.modify_tags(args)


class AwsAccessAddKeyMethod(AbstractAccessMethod):
    def __init__(self, base_command):
        super(AwsAccessAddKeyMethod, self).__init__(base_command, "add-key")

    def callback(self, args):
        (private_key_file, public_key_file) = self.validate_key_files(args)
        self.cloud.add_key_pair(args)
        print(json.dumps({"private_key": private_key_file, "public_key": public_key_file}))


class AwsAccessDeleteKeyMethod(AbstractAccessMethod):
    def __init__(self, base_command):
        super(AwsAccessDeleteKeyMethod, self).__init__(base_command, "delete-key")

    def callback(self, args):
        try:
            self.cloud.delete_key_pair(args)
            for key_file in glob.glob("{}/{}.*".format(args.key_file_path, args.key_pair_name)):
                os.remove(key_file)
            print(json.dumps({"success": "Keypair {} deleted.".format(args.key_pair_name)}))
        except Exception as e:
            logging.error(e)
            print(json.dumps({"error": "Unable to delete Keypair: {}".format(args.key_pair_name)}))


class AwsAccessListKeysMethod(AbstractMethod):
    def __init__(self, base_command):
        super(AwsAccessListKeysMethod, self).__init__(base_command, "list-keys")

    def add_extra_args(self):
        """Setup the CLI options for List Key Pair.
        """
        super(AwsAccessListKeysMethod, self).add_extra_args()
        self.parser.add_argument("--key_pair_name", required=False, default=None,
                                 help="AWS Key Pair name")

    def callback(self, args):
        print(json.dumps(self.cloud.list_key_pair(args)))


class AwsQueryRegionsMethod(AbstractMethod):
    def __init__(self, base_command):
        super(AwsQueryRegionsMethod, self).__init__(base_command, "regions")

    def callback(self, args):
        print(json.dumps(self.cloud.get_regions()))


class AwsQueryZonesMethod(AbstractMethod):
    def __init__(self, base_command):
        super(AwsQueryZonesMethod, self).__init__(base_command, "zones")

    def add_extra_args(self):
        super(AwsQueryZonesMethod, self).add_extra_args()
        self.parser.add_argument("--dest_vpc_id", required=False, help="Destination VPC Id. " +
                                 "Do not specify if you want us to create a new one.")

    def preprocess_args(self, args):
        super(AwsQueryZonesMethod, self).preprocess_args(args)
        if args.dest_vpc_id and not args.region:
            raise YBOpsRuntimeError("Using --dest_vpc_id requires --region")

    def callback(self, args):
        print(json.dumps(self.cloud.get_zones(args)))


class AwsQueryVPCMethod(AbstractMethod):
    def __init__(self, base_command):
        super(AwsQueryVPCMethod, self).__init__(base_command, "vpc")

    def callback(self, args):
        print(json.dumps(self.cloud.query_vpc(args)))


class AwsQueryCurrentHostMethod(AbstractMethod):
    VALID_METADATA_TYPES = ("vpc-id", "subnet-id", "instance-id", "mac",
                            "region", "privateIp", "security-groups", "role")

    def __init__(self, base_command):
        super(AwsQueryCurrentHostMethod, self).__init__(base_command, "current-host")
        # We do not need cloud credentials to query metadata.
        self.need_validation = False

    def add_extra_args(self):
        super(AwsQueryCurrentHostMethod, self).add_extra_args()
        self.parser.add_argument("--metadata_types", nargs="+", type=str, required=True,
                                 choices=self.VALID_METADATA_TYPES)

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.get_current_host_info(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class AwsQueryPricingMethod(AbstractMethod):
    def __init__(self, base_command):
        super(AwsQueryPricingMethod, self).__init__(base_command, "pricing")

    def callback(self, args):
        raise YBOpsRuntimeError("Not Implemented")


class AwsQuerySpotPricingMethod(AbstractMethod):
    def __init__(self, base_command):
        super(AwsQuerySpotPricingMethod, self).__init__(base_command, "spot-pricing")

    def add_extra_args(self):
        super(AwsQuerySpotPricingMethod, self).add_extra_args()
        self.parser.add_argument("--instance_type", required=True,
                                 help="The instance type to get pricing info for")

    def callback(self, args):
        try:
            if args.region is None or args.zone is None:
                raise YBOpsRuntimeError("Must specify a region & zone to query spot price")
            print(json.dumps({'SpotPrice': self.cloud.get_spot_pricing(args)}))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class AwsNetworkBootstrapMethod(AbstractNetworkMethod):
    def __init__(self, base_command):
        super(AwsNetworkBootstrapMethod, self).__init__(base_command, "bootstrap")

    def add_extra_args(self):
        """Setup the CLI options network bootstrap."""
        super(AwsNetworkBootstrapMethod, self).add_extra_args()
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.network_bootstrap(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class AwsNetworkQueryMethod(AbstractNetworkMethod):
    def __init__(self, base_command):
        super(AwsNetworkQueryMethod, self).__init__(base_command, "query")

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.query_vpc(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class AwsNetworkCleanupMethod(AbstractNetworkMethod):
    def __init__(self, base_command):
        super(AwsNetworkCleanupMethod, self).__init__(base_command, "cleanup")

    def add_extra_args(self):
        """Setup the CLI options network cleanup."""
        super(AwsNetworkCleanupMethod, self).add_extra_args()
        self.parser.add_argument("--custom_payload", required=False,
                                 help="JSON payload of per-region data.")

    def callback(self, args):
        try:
            print(json.dumps(self.cloud.network_cleanup(args)))
        except YBOpsRuntimeError as ye:
            print(json.dumps({"error": get_exception_message(ye)}))


class AbstractDnsMethod(AbstractMethod):
    def __init__(self, base_command, method_name):
        super(AbstractDnsMethod, self).__init__(base_command, method_name)
        self.ip_list = []
        self.naming_info_required = True

    def add_extra_args(self):
        super(AbstractDnsMethod, self).add_extra_args()
        self.parser.add_argument("--hosted_zone_id", required=True,
                                 help="The ID of the Route53 Hosted Zone.")
        self.parser.add_argument("--domain_name_prefix", required=self.naming_info_required,
                                 help="The prefix to create the RecordSet with, in your Zone.")
        self.parser.add_argument("--node_ips", required=self.naming_info_required,
                                 help="The CSV of the node IPs to associate to this DNS entry.")

    def preprocess_args(self, args):
        super(AbstractDnsMethod, self).preprocess_args(args)
        if args.node_ips:
            self.ip_list = args.node_ips.split(',')


class AwsCreateDnsEntryMethod(AbstractDnsMethod):
    def __init__(self, base_command):
        super(AwsCreateDnsEntryMethod, self).__init__(base_command, "create")

    def callback(self, args):
        create_dns_record_set(args.hosted_zone_id, args.domain_name_prefix, self.ip_list)


class AwsEditDnsEntryMethod(AbstractDnsMethod):
    def __init__(self, base_command):
        super(AwsEditDnsEntryMethod, self).__init__(base_command, "edit")

    def callback(self, args):
        edit_dns_record_set(args.hosted_zone_id, args.domain_name_prefix, self.ip_list)


class AwsDeleteDnsEntryMethod(AbstractDnsMethod):
    def __init__(self, base_command):
        super(AwsDeleteDnsEntryMethod, self).__init__(base_command, "delete")

    def callback(self, args):
        delete_dns_record_set(args.hosted_zone_id, args.domain_name_prefix, self.ip_list)


class AwsListDnsEntryMethod(AbstractDnsMethod):
    def __init__(self, base_command):
        super(AwsListDnsEntryMethod, self).__init__(base_command, "list")
        self.naming_info_required = False

    def callback(self, args):
        try:
            result = list_dns_record_set(args.hosted_zone_id)
            print(json.dumps({
                'name': result['HostedZone']['Name']
            }))
        except Exception as e:
            print(json.dumps({'error': repr(e)}))
