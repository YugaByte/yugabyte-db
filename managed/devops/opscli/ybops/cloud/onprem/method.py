#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from jinja2 import Environment, FileSystemLoader
from ybops.common.exceptions import YBOpsRuntimeError, get_exception_message
from ybops.cloud.common.method import AbstractMethod
from ybops.cloud.common.method import AbstractInstancesMethod
from ybops.cloud.common.method import CreateInstancesMethod
from ybops.cloud.common.method import DestroyInstancesMethod
from ybops.cloud.common.method import ProvisionInstancesMethod, ListInstancesMethod
from ybops.utils import get_ssh_host_port, validate_instance, get_datafile_path, YB_HOME_DIR, \
                        get_mount_roots, remote_exec_command, wait_for_ssh, scp_to_tmp


import json
import logging
import os
import subprocess
import stat
import ybops.utils as ybutils

from six import iteritems


class OnPremCreateInstancesMethod(CreateInstancesMethod):
    """Subclass for creating instances in an on premise deployment. This is responsible for setting
    up custom metadata required for properly provisioning the machine as part of a certain cluster.
    """

    def __init__(self, base_command):
        super(OnPremCreateInstancesMethod, self).__init__(base_command)

    def callback(self, args):
        # Since in an on premise deployment, the machine should already be created, we use this
        # step purely to validate that we can access the host.
        #
        # TODO: do we still want/need to change to the custom ssh port?
        self.wait_for_host(args)


class OnPremProvisionInstancesMethod(ProvisionInstancesMethod):
    """Subclass for provisioning instances in an on premise deployment. Sets up the proper Create
    method.
    """

    def __init__(self, base_command):
        super(OnPremProvisionInstancesMethod, self).__init__(base_command)

    def setup_create_method(self):
        """Override to get the wiring to the proper method.
        """
        self.create_method = OnPremCreateInstancesMethod(self.base_command)

    def callback(self, args):
        # For onprem, we are always using pre-existing hosts!
        args.reuse_host = True
        super(OnPremProvisionInstancesMethod, self).callback(args)


class OnPremValidateMethod(AbstractInstancesMethod):
    """Subclass for verifying the instances in an on premise deployment can be accessed over SSH
    and have all the proper properties. E.g. OS is centos-7, mount path for drives is valid, etc.
    """

    def __init__(self, base_command):
        super(OnPremValidateMethod, self).__init__(base_command, "validate")

    def callback(self, args):
        """args.search_pattern should be a private ip address for the device for OnPrem.
        """
        self.extra_vars.update(
            get_ssh_host_port({"private_ip": args.search_pattern}, args.custom_ssh_port))
        print(validate_instance(self.extra_vars["ssh_host"],
                                self.extra_vars["ssh_port"],
                                self.SSH_USER,
                                args.private_key_file,
                                self.mount_points.split(',')))


class OnPremListInstancesMethod(ListInstancesMethod):
    """Subclass for listing instances in onprem.
    """
    def __init__(self, base_command):
        super(OnPremListInstancesMethod, self).__init__(base_command)

    def callback(self, args):
        logging.debug("Received args {}".format(args))

        host_infos = self.cloud.get_host_info(args, get_all=args.as_json)
        if not host_infos:
            return None

        if 'server_type' in host_infos and host_infos['server_type'] is None:
            del host_infos['server_type']

        if args.mount_points:
            for host_info in host_infos:
                try:
                    ssh_options = {
                        "ssh_user": host_info['ssh_user'],
                        "private_key_file": args.private_key_file
                    }
                    ssh_options.update(get_ssh_host_port(
                                        self.cloud.get_host_info(args),
                                        args.custom_ssh_port))
                    host_info['mount_roots'] = get_mount_roots(ssh_options, args.mount_points)

                except Exception as e:
                    logging.info("Error {} locating mount root for host '{}', ignoring.".format(
                        str(e), host_info
                    ))
                    continue

        if args.as_json:
            print(json.dumps(host_infos))
        else:
            print('\n'.join(["{}={}".format(k, v) for k, v in iteritems(host_infos)]))


class OnPremDestroyInstancesMethod(DestroyInstancesMethod):
    """Subclass for destroying an onprem instance, which essentially means cleaning up the used
    resources.
    """
    def __init__(self, base_command):
        super(OnPremDestroyInstancesMethod, self).__init__(base_command)

    def add_extra_args(self):
        super(OnPremDestroyInstancesMethod, self).add_extra_args()
        self.parser.add_argument("--install_node_exporter", action="store_true",
                                 help='Check if node exporter should be stopped.')

    def callback(self, args):
        host_info = self.cloud.get_host_info(args)
        if not host_info:
            logging.error("Host {} does not exists.".format(args.search_pattern))
            return

        # Run non-db related tasks.
        self.update_ansible_vars_with_args(args)
        if args.install_node_exporter:
            logging.info(("[app] Running control script stop " +
                          "against thirdparty services at {}").format(host_info['name']))
            self.cloud.run_control_script(
                "thirdparty", "stop-services", args, self.extra_vars, host_info)

        # Force db-related commands to use the "yugabyte" user.
        args.ssh_user = "yugabyte"
        self.update_ansible_vars_with_args(args)
        servers = ["master", "tserver"]
        commands = ["stop", "clean", "clean-logs"]
        logging.info(("[app] Running control script stop+clean+clean-logs " +
                     "against master+tserver at {}").format(host_info['name']))
        for s in servers:
            for c in commands:
                self.cloud.run_control_script(
                    s, c, args, self.extra_vars, host_info)

        # Now clean the instance, we pass "" as the 'process' since this command isn't really
        # specific to any process and needs to be just run on the node.
        self.cloud.run_control_script("", "clean-instance", args, self.extra_vars, host_info)


class OnPremPrecheckInstanceMethod(AbstractInstancesMethod):
    def __init__(self, base_command):
        self.current_ssh_user = None
        super(OnPremPrecheckInstanceMethod, self).__init__(base_command, "precheck")

    def preprocess_args(self, args):
        super(OnPremPrecheckInstanceMethod, self).preprocess_args(args)
        if args.precheck_type == "configure":
            self.current_ssh_user = "yugabyte"

    def wait_for_host(self, args, default_port=True):
        logging.info("Waiting for instance {}".format(args.search_pattern))
        host_lookup_count = 0
        # Cache the result of the cloud call outside of the loop.
        host_info = None
        if not host_info:
            host_info = self.cloud.get_host_info(args)
        if host_info:
            self.extra_vars.update(
                get_ssh_host_port(host_info, args.custom_ssh_port, default_port=default_port))
            if wait_for_ssh(self.extra_vars["ssh_host"],
                            self.extra_vars["ssh_port"],
                            self.extra_vars["ssh_user"],
                            args.private_key_file):
                return host_info
        else:
            raise YBOpsRuntimeError("Unable to find host info.")

    def get_ssh_user(self):
        return self.current_ssh_user

    def add_extra_args(self):
        super(OnPremPrecheckInstanceMethod, self).add_extra_args()
        self.parser.add_argument("--precheck_type", required=True,
                                 choices=['provision', 'configure'],
                                 help="Preflight check to determine if instance is ready.")
        self.parser.add_argument("--air_gap", action="store_true",
                                 help='If instances are air gapped or not.')
        self.parser.add_argument("--install_node_exporter", action="store_true",
                                 help='Check if node exporter can be installed properly.')

    def callback(self, args):
        host_info = self.cloud.get_host_info(args)
        if not host_info:
            raise YBOpsRuntimeError("Instance: {} does not exist, cannot run preflight checks"
                                    .format(args.search_pattern))

        results = {}
        logging.info("Running {} preflight checks for instance: {}".format(
            args.precheck_type, args.search_pattern))

        self.update_ansible_vars_with_args(args)
        self.update_ansible_vars_with_host_info(host_info, args.custom_ssh_port)
        try:
            is_configure = args.precheck_type == "configure"
            self.wait_for_host(args, default_port=is_configure)
        except YBOpsRuntimeError as e:
            logging.info("Failed to connect to node {}: {}".format(args.search_pattern, e))
            # No point continuing test if ssh fails.
            results["SSH Connection"] = False
            print(json.dumps(results, indent=2))
            return

        scp_result = scp_to_tmp(
            get_datafile_path('preflight_checks.sh'), self.extra_vars["private_ip"],
            self.extra_vars["ssh_user"], self.extra_vars["ssh_port"], args.private_key_file)

        results["SSH Connection"] = scp_result == 0

        ansible_status = self.cloud.setup_ansible(args).run("test_connection.yml",
                                                            self.extra_vars, host_info,
                                                            print_output=False)
        results["Try Ansible Command"] = ansible_status == 0

        cmd = "/tmp/preflight_checks.sh --type {} --yb_home_dir {} --mount_points {}".format(
            args.precheck_type, YB_HOME_DIR, self.cloud.get_mount_points_csv(args))
        if args.install_node_exporter:
            cmd += " --install_node_exporter"
        if args.air_gap:
            cmd += " --airgap"

        self.update_ansible_vars_with_args(args)
        self.update_ansible_vars_with_host_info(host_info, args.custom_ssh_port)
        rc, stdout, stderr = remote_exec_command(
            self.extra_vars["private_ip"], self.extra_vars["ssh_port"],
            self.extra_vars["ssh_user"], args.private_key_file, cmd)

        if rc != 0:
            results["Preflight Script Error"] = stderr
        else:
            # stdout will be returned as a list of lines, which should just be one line of json.
            stdout = json.loads(stdout[0])
            stdout = {k: v == "true" for k, v in stdout.iteritems()}
            results.update(stdout)

        output = json.dumps(results, indent=2)
        print(output)


class OnPremFillInstanceProvisionTemplateMethod(AbstractMethod):
    def __init__(self, base_command):
        super(OnPremFillInstanceProvisionTemplateMethod, self).__init__(base_command, 'template')

    def add_extra_args(self):
        self.parser.add_argument('--name', default='provision_instance.py', required=False,
                                 help='Desired name for the new provision instance script')
        self.parser.add_argument('--destination', required=True,
                                 help='Directory to put the new provision instance script in.')
        self.parser.add_argument('--ssh_user', required=True,
                                 help='The user who can SSH into the instance.')
        # Allow for ssh_port to not be mandatory so already created template scripts just default
        # to 22, without breaking...
        self.parser.add_argument('--custom_ssh_port', required=False, default=22,
                                 help='The port on which to SSH into the instance.')
        self.parser.add_argument('--vars_file', required=True,
                                 help='The vault file containing needed vars.')
        self.parser.add_argument('--vault_password_file', required=True,
                                 help='The password file to unlock the vault file.')
        self.parser.add_argument('--local_package_path', required=True,
                                 help='Path to the local third party dependency packages.')
        self.parser.add_argument('--private_key_file', required=True,
                                 help='Private key file to ssh into the instance.')
        self.parser.add_argument('--passwordless_sudo', action='store_true',
                                 help='If the ssh_user has passwordless sudo access or not.')
        self.parser.add_argument("--air_gap", action="store_true",
                                 help='If instances are air gapped or not.')
        self.parser.add_argument("--node_exporter_port", type=int, default=9300,
                                 help="The port for node_exporter to bind to")
        self.parser.add_argument("--node_exporter_user", default="prometheus")
        self.parser.add_argument("--install_node_exporter", action="store_true")

    def callback(self, args):
        config = {'devops_home': ybutils.YB_DEVOPS_HOME, 'cloud': self.cloud.name}
        file_name = 'provision_instance.py.j2'
        try:
            config.update(vars(args))
            config["yb_home_dir"] = YB_HOME_DIR
            data_dir = os.path.dirname(get_datafile_path(file_name))
            template = Environment(loader=FileSystemLoader(data_dir)).get_template(file_name)
            with open(os.path.join(args.destination, args.name), 'w') as f:
                f.write(template.render(config))
            os.chmod(f.name, stat.S_IRWXU)
            print(json.dumps({'script_path': f.name}))
        except Exception as e:
            logging.error(e)
            print(json.dumps(
                {"error": "Unable to create script: {}".format(get_exception_message(e))}))
