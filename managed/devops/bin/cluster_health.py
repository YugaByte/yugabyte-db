#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

import argparse
import json
import logging
import os
import smtplib
import subprocess
import sys
import time

from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from exceptions import RuntimeError
from datetime import datetime
from dateutil import tz
from multiprocessing import Pool

# Try to read home dir from environment variable, else assume it's /home/yugabyte.
YB_HOME_DIR = os.environ.get("YB_HOME_DIR", "/home/yugabyte")
YB_TSERVER_DIR = os.path.join(YB_HOME_DIR, "tserver")
YB_CORES_DIR = os.path.join(YB_HOME_DIR, "cores/")
YB_PROCESS_LOG_PATH_FORMAT = os.path.join(YB_HOME_DIR, "{}/logs/")
VM_CERT_FILE_PATH = os.path.join(YB_HOME_DIR, "yugabyte-tls-config/ca.crt")
K8S_CERT_FILE_PATH = "/opt/certs/yugabyte/ca.crt"

RECENT_FAILURE_THRESHOLD_SEC = 8 * 60
FATAL_TIME_THRESHOLD_MINUTES = 12
DISK_UTILIZATION_THRESHOLD_PCT = 80
FD_THRESHOLD_PCT = 50
SSH_TIMEOUT_SEC = 10
MAX_CONCURRENT_PROCESSES = 10
MAX_TRIES = 2

# This hardcoded as the ses:FromAddress in the IAM policy.
EMAIL_SERVER = os.environ.get("SMTP_SERVER", "email-smtp.us-west-2.amazonaws.com")
EMAIL_PORT = os.environ.get("SMTP_PORT", None)
# These need to be setup as env vars from YW or in the env if running manually. If not specified
# then sending the email will fail, but reporting the status will still work just fine.
EMAIL_FROM = os.environ.get("YB_ALERTS_EMAIL")
EMAIL_USERNAME = os.environ.get("YB_ALERTS_USERNAME", None)
EMAIL_PASSWORD = os.environ.get("YB_ALERTS_PASSWORD", None)
SMTP_USE_SSL = os.environ.get("SMTP_USE_SSL", "true")
SMTP_USE_TLS = os.environ.get("SMTP_USE_TLS", "false")

###################################################################################################
# Reporting
###################################################################################################


def generate_ts():
    return local_time().strftime('%Y-%m-%d %H:%M:%S')


class EntryType:
    NODE = "node"
    NODE_NAME = "node_name"
    TIMESTAMP = "timestamp"
    MESSAGE = "message"
    DETAILS = "details"
    HAS_ERROR = "has_error"
    PROCESS = "process"


class Entry:
    def __init__(self, message, node, process=None, node_name=None):
        self.timestamp = generate_ts()
        self.message = message
        self.node = node
        self.node_name = node_name
        self.process = process
        # To be filled in.
        self.details = None
        self.has_error = None

    def fill_and_return_entry(self, details, has_error=False):
        self.details = details
        self.has_error = has_error
        return self

    def as_json(self):
        j = {
            EntryType.NODE: self.node,
            EntryType.NODE_NAME: self.node_name or "",
            EntryType.TIMESTAMP: self.timestamp,
            EntryType.MESSAGE: self.message,
            EntryType.DETAILS: self.details,
            EntryType.HAS_ERROR: self.has_error
        }
        if self.process:
            j[EntryType.PROCESS] = self.process
        return j


class Report:
    def __init__(self, yb_version):
        self.mail_error = None
        self.entries = []
        self.start_ts = generate_ts()
        self.yb_version = yb_version

        logging.info("Script started at {}".format(self.start_ts))

    def add_entry(self, entry):
        self.entries.append(entry)

    def has_errors(self):
        return True in [e.has_error for e in self.entries]

    def write_to_log(self, log_file):
        if not self.has_errors() or log_file is None:
            logging.info("Not logging anything to a file.")
            return

        # Write to stderr.
        logging.info(str(self))
        # Write to the log file.
        file = open(log_file, 'a')
        file.write(str(self))
        file.close()

    def as_json(self, only_errors=False):
        j = {
            "timestamp": self.start_ts,
            "yb_version": self.yb_version,
            "data": [e.as_json() for e in self.entries if not only_errors or e.has_error],
            "has_error": True in [e.has_error for e in self.entries]
        }
        if self.mail_error is not None:
            j["mail_error"] = self.mail_error
        return json.dumps(j, indent=2)

    def __str__(self):
        return self.as_json()


###################################################################################################
# Checking
###################################################################################################
def check_output(cmd, env):
    try:
        bytes = subprocess.check_output(cmd, stderr=subprocess.STDOUT, env=env)
        return bytes.decode('utf-8')
    except subprocess.CalledProcessError as e:
        return 'Error executing command {}: {}'.format(cmd, e.output)


def safe_pipe(command_str):
    return "set -o pipefail; " + command_str


def has_errors(str):
    return str.startswith('Error')


class KubernetesDetails():

    def __init__(self, node_fqdn, config_map):
        self.namespace = node_fqdn.split('.')[2]
        self.pod_name = node_fqdn.split('.')[0]
        # The pod names are yb-master-n/yb-tserver-n where n is the pod number
        # and yb-master/yb-tserver are the container names.
        self.container = self.pod_name.rsplit('-', 1)[0]
        self.config = config_map[self.namespace]


class NodeChecker():

    def __init__(self, node, node_name, identity_file, ssh_port, start_time_ms,
                 namespace_to_config, ysql_port, ycql_port, redis_port, enable_tls_client):
        self.node = node
        self.node_name = node_name
        self.identity_file = identity_file
        self.ssh_port = ssh_port
        self.start_time_ms = start_time_ms
        self.enable_tls_client = enable_tls_client
        # TODO: best way to do mark that this is a k8s deployment?
        self.is_k8s = ssh_port == 0 and not self.identity_file
        self.k8s_details = None
        if self.is_k8s:
            self.k8s_details = KubernetesDetails(node, namespace_to_config)

        # Some validation.
        if not self.is_k8s and not os.path.isfile(self.identity_file):
            raise RuntimeError('Error: cannot find identity file {}'.format(self.identity_file))
        self.ysql_port = ysql_port
        self.ycql_port = ycql_port
        self.redis_port = redis_port

    def _new_entry(self, message, process=None):
        return Entry(message, self.node, process, self.node_name)

    def _remote_check_output(self, command):
        cmd_to_run = []
        command = safe_pipe(command) if isinstance(command, basestring) else command
        env_conf = os.environ.copy()
        if self.is_k8s:
            env_conf["KUBECONFIG"] = self.k8s_details.config
            cmd_to_run.extend([
                'kubectl',
                'exec',
                '-t',
                '-n={}'.format(self.k8s_details.namespace),
                self.k8s_details.pod_name,
                '-c',
                self.k8s_details.container,
                '--',
                'bash',
                '-c',
                command
            ])
        else:
            cmd_to_run.extend(
                ['ssh', 'yugabyte@{}'.format(self.node), '-p', str(self.ssh_port),
                 '-o', 'StrictHostKeyChecking no',
                 '-o', 'ConnectTimeout={}'.format(SSH_TIMEOUT_SEC),
                 '-o', 'UserKnownHostsFile /dev/null',
                 '-o', 'LogLevel ERROR',
                 '-i', self.identity_file])
            cmd_to_run.append(command)

        output = check_output(cmd_to_run, env_conf).strip()

        # TODO: this is only relevant for SSH, so non-k8s.
        if output.endswith('No route to host'):
            return 'Error: Node {} is unreachable'.format(self.node)
        return output

    def get_disk_utilization(self):
        remote_cmd = 'df -h'
        return self._remote_check_output(remote_cmd)

    def check_disk_utilization(self):
        logging.info("Checking disk utilization on node {}".format(self.node))
        e = self._new_entry("Disk utilization")
        found_error = False
        output = self.get_disk_utilization()
        msgs = []
        if has_errors(output):
            return e.fill_and_return_entry([output], True)

        # Do not process the headers.
        lines = output.split('\n')
        msgs.append(lines[0])
        for line in lines[1:]:
            msgs.append(line)
            if not line:
                continue
            percentage = line.split()[4][:-1]
            if int(percentage) > DISK_UTILIZATION_THRESHOLD_PCT:
                found_error = True
        return e.fill_and_return_entry(msgs, found_error)

    def check_for_fatal_logs(self, process):
        logging.info("Checking for fatals on node {}".format(self.node))
        e = self._new_entry("Fatal log files")
        logs = []
        process_dir = process.strip("yb-")
        search_dir = YB_PROCESS_LOG_PATH_FORMAT.format(process_dir)
        remote_cmd = (
            'find {} {} -name "*FATAL*" -type f -printf "%T@ %p\\n" | sort -rn'.format(
                search_dir,
                '-mmin -{}'.format(FATAL_TIME_THRESHOLD_MINUTES)))
        output = self._remote_check_output(remote_cmd)
        if has_errors(output):
            return e.fill_and_return_entry([output], True)

        if output:
            for line in output.strip().split('\n'):
                splits = line.strip().split()
                # In case of errors, we might get other outputs from the find command, such as
                # the stderr output. In that case, best we can do is validate it has 2
                # components and first one is an epoch with fraction...
                if len(splits) != 2:
                    continue
                epoch, filename = splits
                epoch = epoch.split('.')[0]
                if not epoch.isdigit():
                    continue
                logs.append('{} ({} old)'.format(
                    filename,
                    ''.join(seconds_to_human_readable_time(int(time.time() - int(epoch))))))
        return e.fill_and_return_entry(logs, len(logs) > 0)

    def check_for_core_files(self):
        logging.info("Checking for core files on node {}".format(self.node))
        e = self._new_entry("Core files")
        remote_cmd = 'if [ -d {} ]; then find {} {} -name "core_*"; fi'.format(
            YB_CORES_DIR, YB_CORES_DIR,
            '-mmin -{}'.format(FATAL_TIME_THRESHOLD_MINUTES))
        output = self._remote_check_output(remote_cmd)
        if has_errors(output):
            return e.fill_and_return_entry([output], True)

        files = []
        if output.strip():
            files = output.strip().split('\n')
        return e.fill_and_return_entry(files, len(files) > 0)

    def get_uptime_for_process(self, process):
        remote_cmd = "ps -C {} -o etimes=".format(process)
        return self._remote_check_output(remote_cmd).strip()

    def check_uptime_for_process(self, process):
        logging.info("Checking uptime for {} process {}".format(self.node, process))
        e = self._new_entry("Uptime", process)
        uptime = self.get_uptime_for_process(process)
        if has_errors(uptime):
            return e.fill_and_return_entry([uptime], True)

        if uptime.isdigit():
            # To prevent emails right after the universe creation or restart, we pass in a
            # potential start time. If the reported uptime is in our threshold, but so is the
            # time since the last operation, then we do not report this as an error.
            recent_operation = self.start_time_ms is not None and \
                (int(time.time()) - self.start_time_ms / 1000 <= RECENT_FAILURE_THRESHOLD_SEC)
            # Server went down recently.
            if int(uptime) <= RECENT_FAILURE_THRESHOLD_SEC and not recent_operation:
                return e.fill_and_return_entry(['Uptime: {} seconds'.format(uptime)], True)
            else:
                return e.fill_and_return_entry(['Uptime: {} seconds'.format(uptime)], False)
        elif not uptime:
            return e.fill_and_return_entry(['Process is not running'], True)
        else:
            return e.fill_and_return_entry(['Invalid uptime {}'.format(uptime)], True)

    def check_file_descriptors(self):
        logging.info("Checking for open file descriptors on node {}".format(self.node))
        e = self._new_entry("Opened file descriptors")

        remote_cmd = 'ulimit -n; cat /proc/sys/fs/file-max; cat /proc/sys/fs/file-nr | cut -f1'
        output = self._remote_check_output(remote_cmd)

        if has_errors(output):
            return e.fill_and_return_entry([output], True)

        counts = output.split()

        if len(counts) != 3:
            return e.fill_and_return_entry(
                ['Error checking file descriptors: {}'.format(counts)], True)

        if not all(count.isdigit() for count in counts):
            return e.fill_and_return_entry(
                ['Received invalid counts: {}'.format(counts)], True)

        ulimit = int(counts[0])
        file_max = int(counts[1])
        open_fd = int(counts[2])
        max_fd = min(ulimit, file_max)
        if open_fd > max_fd * FD_THRESHOLD_PCT / 100.0:
            return e.fill_and_return_entry(
                ['Open file descriptors: {}. Max file descriptors: {}'.format(open_fd, max_fd)],
                True)

        return e.fill_and_return_entry([])

    def check_cqlsh(self):
        logging.info("Checking cqlsh works for node {}".format(self.node))
        e = self._new_entry("Connectivity with cqlsh")

        cqlsh = '{}/bin/cqlsh'.format(YB_TSERVER_DIR)
        remote_cmd = '{} {} {} -e "SHOW HOST"'.format(cqlsh, self.node, self.ycql_port)
        if self.enable_tls_client:
            cert_file = K8S_CERT_FILE_PATH if self.is_k8s else VM_CERT_FILE_PATH

            remote_cmd = 'SSL_CERTFILE={} {} {}'.format(cert_file, remote_cmd, '--ssl')

        output = self._remote_check_output(remote_cmd).strip()

        errors = []
        if not (output.startswith('Connected to local cluster at {}:{}'
                                  .format(self.node, self.ycql_port)) or
                "AuthenticationFailed('Remote end requires authentication.'" in output):
            errors = [output]
        return e.fill_and_return_entry(errors, len(errors) > 0)

    def check_redis_cli(self):
        logging.info("Checking redis cli works for node {}".format(self.node))
        e = self._new_entry("Connectivity with redis-cli")
        redis_cli = '{}/bin/redis-cli'.format(YB_TSERVER_DIR)
        remote_cmd = 'echo "ping" | {} -h {} -p {}'.format(redis_cli, self.node, self.redis_port)

        output = self._remote_check_output(remote_cmd).strip()

        errors = []
        if output not in ("PONG", "NOAUTH ping: Authentication required."):
            errors = [output]

        return e.fill_and_return_entry(errors, len(errors) > 0)

    def check_ysqlsh(self):
        logging.info("Checking ysqlsh works for node {}".format(self.node))
        e = self._new_entry("Connectivity with ysqlsh")

        ysqlsh = '{}/bin/ysqlsh'.format(YB_TSERVER_DIR)
        if not self.enable_tls_client:
            user = "postgres"
            remote_cmd = r'echo "\conninfo" | {} -h {} -p {} -U {}'.format(
                ysqlsh, self.node, self.ysql_port, user)
        else:
            user = "yugabyte"
            remote_cmd = r'echo "\conninfo" | {} -h {} -p {} -U {} {}'.format(
                ysqlsh, self.node, self.ysql_port, user, '"sslmode=require"')

        errors = []
        output = self._remote_check_output(remote_cmd).strip()
        if not (output.startswith('You are connected to database "{}"'.format(user)) or
                "Password for user {}:".format(user)):
            errors = [output]
        return e.fill_and_return_entry(errors, len(errors) > 0)


###################################################################################################
# Utility functions
###################################################################################################
def seconds_to_human_readable_time(seconds):
    m, s = divmod(seconds, 60)
    h, m = divmod(m, 60)
    d, h = divmod(h, 24)
    return ['{} days {:d}:{:02d}:{:02d}'.format(d, h, m, s)]


def assemble_mail_row(data, is_first_check, timestamp):
    ts_span = '''<div style="
        color:#ffffff;
        font-size:5px;">{}</div>'''.format(timestamp) if is_first_check else ''
    process = ''
    if 'process' in data:
        process = data['process']

    border_color = 'transparent' if is_first_check else '#e5e5e9'

    def td_element(padding, weight, content): return '''<td style="
        border-top: 1px solid {};
        padding:{};
        {}
        vertical-align:top;">{}</td>'''.format(border_color, padding, weight, content)
    badge = ''
    if data['has_error'] is True:
        badge = '''<span style="
            font-size:0.8em;
            background-color:#E8473F;
            color:#ffffff;
            border-radius:2px;
            margin-right:8px;
            padding:2px 4px;
            font-weight:400;">Failed</span>'''

    def pre_element(content): return '''<pre style="
        background-color:#f7f7f7;
        border: 1px solid #e5e5e9;
        padding:10px;
        white-space: pre-wrap;
        border-radius:5px;">{}</pre>'''.format(content)

    details_content_ok = '<div style="padding:11px 0;">Ok</div>'
    dat_det = data['details']
    details_content = details_content_ok if dat_det == [] else pre_element(
        '\n'.join(dat_det) if data['message'] == 'Disk utilization' else ' '.join(dat_det))
    return '<tr>{}{}{}</tr>'.format(
        td_element(
            '10px 20px 10px 0',
            'font-weight:700;font-size:1.2em;',
            badge+data['message']+ts_span),
        td_element('10px 20px 10px 0', '', process),
        td_element('0', '', details_content))


def send_mail(report, subject, destination, nodes, universe_name, report_only_errors):
    logging.info("Sending email: '{}' to '{}'".format(subject, destination))
    style_font = "font-family: SF Pro Display, SF Pro, Helvetica Neue, Helvetica, sans-serif;"
    json_object = json.loads(str(report))

    def make_subtitle(content):
        return '<span style="color:#8D8F9D;font-weight:400;">{}:</span>'.format(content)

    email_content_data = []

    for node in nodes:
        node_has_error = False
        is_first_check = True
        node_content_data = []
        node_name = ''
        for node_check_data_row in json_object["data"]:
            if node == node_check_data_row["node"]:
                if node_check_data_row["has_error"]:
                    node_has_error = True
                elif report_only_errors:
                    continue
                node_name = node_check_data_row["node_name"]
                node_content_data.append(assemble_mail_row(
                    node_check_data_row,
                    is_first_check,
                    json_object["timestamp"])
                )
                is_first_check = False

        if report_only_errors and not node_has_error:
            continue

        node_header_style_colors_color = '#ffffff;' if node_has_error else '#289b42;'
        node_header_style_colors_back = '#E8473F;' if node_has_error else '#ffffff'
        node_header_style_colors = 'background-color:{};color:{}'.format(
            node_header_style_colors_back, node_header_style_colors_color)
        badge_caption = 'Error' if node_has_error else 'Running fine'

        def th_element(style, content): return '<th style="{}">{}</th>'.format(style, content)

        def tr_element(content): return '''<tr style="
            text-transform:uppercase;
            font-weight:500;">{}</tr>'''.format(content)

        def table_element(content): return '''<table
            cellspacing="0"
            style="width:auto;text-align:left;font-size:12px;">{}</table>'''.format(content)

        badge_style = '''style="font-weight:400;
            margin-left:10px;
            font-size:0.6em;
            vertical-align:middle;
            {}
            border-radius:4px;
            padding:2px 6px;"'''.format(node_header_style_colors)
        node_header_title = str(node_name) + '<br>' + str(node) + \
            '<span {}>{}</span>'.format(badge_style, badge_caption)
        h2_style = '''font-weight:700;
            line-height:1em;
            color:#202951;
            font-size:2.5em;
            margin:0;{}'''.format(style_font)
        node_header = '{}<h2 style="{}">{}</h2>'.format(
            make_subtitle('Cluster'), h2_style, node_header_title)
        table_container_style = '''background-color:#ffffff;
            padding:15px 20px 7px;
            border-radius:10px;
            margin-top:30px;
            margin-bottom:50px;'''

        def table_container(content): return table_element(
            tr_element(
                th_element('width:15%;padding:0 30px 10px 0;', 'Check type') +
                th_element('width:15%;white-space:nowrap;padding:0 30px 10px 0;', 'Details') +
                th_element('', '')) +
            content)
        email_content_data.append('{}<div style="{}">{}</div>'.format(
            node_header,
            table_container_style,
            table_container(''.join(node_content_data))))

    if not email_content_data:
        email_content_data = "<b>No errors to report.</b>"
    sender = EMAIL_FROM
    msg = MIMEMultipart('alternative')
    msg['Subject'] = subject
    msg['From'] = sender
    # TODO(bogdan): AWS recommends doing one sendmail per address:
    # https://docs.aws.amazon.com/ses/latest/DeveloperGuide/manage-sending-limits.html
    msg['To'] = destination
    style = "{}font-size: 14px;background-color:#f7f7f7;padding:25px 30px 5px;".format(style_font)
    # add timestamp to avoid gmail collapsing
    timestamp = '<span style="color:black;font-size:10px;">{}</span>'.format(
        json_object["timestamp"])

    def make_header_left(title, content):
        return '''{}<h1 style="
            {}
            line-height:1em;
            color:#202951;
            font-weight:700;
            font-size:1.85em;
            padding-bottom:15px;
            margin:0;">{}</h1>'''.format(make_subtitle(title), style_font, content)
    header = '''<table width="100%">
        <tr>
            <td style="text-align:left">{name}</td>
            <td style="text-align:right">{ts}</td>
        </tr>
        <tr>
            <td style="text-align:left">{version}</td>
        </tr>
    </table>'''.format(
        name=make_header_left("Universe name", universe_name),
        ts=timestamp,
        version=make_header_left("Universe version", report.yb_version))
    body = '<html><body><pre style="{}">{}\n{} {}</pre></body></html>\n'.format(
        style, header, ''.join(email_content_data), timestamp)

    msg.attach(MIMEText(report.as_json(report_only_errors), 'plain'))
    # the last part of a multipart MIME message is the preferred part
    msg.attach(MIMEText(body, 'html'))

    if SMTP_USE_SSL.lower() == 'true':
        if EMAIL_PORT is not None:
            s = smtplib.SMTP_SSL(EMAIL_SERVER, int(EMAIL_PORT))
        else:
            # Port defaults to 465
            s = smtplib.SMTP_SSL(EMAIL_SERVER)
    else:
        if EMAIL_PORT is not None:
            s = smtplib.SMTP(EMAIL_SERVER, int(EMAIL_PORT))
        else:
            s = smtplib.SMTP(EMAIL_SERVER)
        if SMTP_USE_TLS.lower() == 'true':
            s.starttls()
    s.ehlo()
    if EMAIL_USERNAME is not None and EMAIL_PASSWORD is not None:
        s.login(EMAIL_USERNAME, EMAIL_PASSWORD)
    dest_list = destination.split(',')
    s.sendmail(sender, dest_list, msg.as_string())
    s.quit()


def local_time():
    return datetime.utcnow().replace(tzinfo=tz.tzutc()).astimezone(tz.gettz('America/Los_Angeles'))


###################################################################################################
# Multi-threaded handling and main
###################################################################################################
def multithreaded_caller(instance, func_name,  sleep_interval=0, args=(), kwargs=None):
    if sleep_interval > 0:
        logging.debug("waiting for sleep to run " + func_name)
        time.sleep(sleep_interval)

    if kwargs is None:
        kwargs = {}
    return getattr(instance, func_name)(*args, **kwargs)


class CheckCoordinator:
    class CheckRunInfo:
        def __init__(self, instance, func_name, yb_process):
            self.instance = instance
            self.func_name = func_name
            self.yb_process = yb_process
            self.result = None
            self.entry = None
            self.tries = 0

    def __init__(self, retry_interval_secs):
        self.pool = Pool(MAX_CONCURRENT_PROCESSES)
        self.checks = []
        self.retry_interval_secs = retry_interval_secs

    def add_check(self, instance, func_name, yb_process=None):
        self.checks.append(CheckCoordinator.CheckRunInfo(instance, func_name, yb_process))

    def run(self):

        while True:
            checks_remaining = 0
            for check in self.checks:
                check.entry = check.result.get() if check.result else None

                # Run checks until they succeed, up to max tries. Wait for sleep_interval secs
                # before retrying to let transient errors correct themselves.
                if check.entry is None or (check.entry.has_error and check.tries < MAX_TRIES):
                    checks_remaining += 1
                    sleep_interval = self.retry_interval_secs if check.tries > 0 else 0

                    if check.tries > 0:
                        logging.info("Retry # " + str(check.tries) +
                                     " for check " + check.func_name)

                    if check.yb_process is None:
                        check.result = self.pool.apply_async(
                                            multithreaded_caller,
                                            (check.instance, check.func_name, sleep_interval))
                    else:
                        check.result = self.pool.apply_async(
                                            multithreaded_caller,
                                            (check.instance,
                                                check.func_name,
                                                sleep_interval,
                                                (check.yb_process,)))

                    check.tries += 1

            if checks_remaining == 0:
                break

        entries = []
        for check in self.checks:
            # TODO: we probably do not need to set a timeout, since SSH has one...
            entries.append(check.result.get())
        return entries


class Cluster():
    def __init__(self, data):
        self.identity_file = data["identityFile"]
        self.ssh_port = data["sshPort"]
        self.enable_tls_client = data["enableTlsClient"]
        self.master_nodes = data["masterNodes"]
        self.tserver_nodes = data["tserverNodes"]
        self.yb_version = data["ybSoftwareVersion"]
        self.namespace_to_config = data["namespaceToConfig"]
        self.enable_ysql = data["enableYSQL"]
        self.ysql_port = data["ysqlPort"]
        self.ycql_port = data["ycqlPort"]
        self.redis_port = data["redisPort"]


class UniverseDefinition():
    def __init__(self, json_blob):
        data = json.loads(json_blob)
        self.clusters = [Cluster(c) for c in data]


def main():
    parser = argparse.ArgumentParser(prog=sys.argv[0])
    parser.add_argument('--cluster_payload', type=str, default=None, required=True,
                        help='JSON serialized payload of cluster data: IPs, pem files, etc.')
    parser.add_argument('--universe_name', type=str, default=None, required=True,
                        help='Universe name to use in the email report')
    parser.add_argument('--customer_tag', type=str, default=None,
                        help='Customer name/env info to use in the email report')
    parser.add_argument('--destination', type=str, default=None, required=False,
                        help='CSV of email addresses to send the report to')
    parser.add_argument('--log_file', type=str, default=None, required=False,
                        help='Log file to which the report will be written to')
    parser.add_argument('--send_status', action="store_true",
                        help='Send email even if no errors')
    parser.add_argument('--start_time_ms', type=int, required=False, default=None,
                        help='Potential start time of the universe, to prevent uptime confusion.')
    parser.add_argument('--retry_interval_secs', type=int, required=False, default=30,
                        help='Time to wait between retries of failed checks')
    parser.add_argument('--report_only_errors', action="store_true",
                        help='Only report nodes with errors')
    args = parser.parse_args()
    universe = UniverseDefinition(args.cluster_payload)
    # Technically, each cluster can have its own version, but in practice, we disallow that in YW.
    universe_version = universe.clusters[0].yb_version if universe.clusters else None
    report = Report(universe_version)
    coordinator = CheckCoordinator(args.retry_interval_secs)
    summary_nodes = {}
    for c in universe.clusters:
        master_nodes = c.master_nodes
        tserver_nodes = c.tserver_nodes
        all_nodes = dict(master_nodes)
        all_nodes.update(dict(tserver_nodes))
        summary_nodes.update(dict(all_nodes))
        for (node, node_name) in all_nodes.items():
            checker = NodeChecker(
                    node, node_name, c.identity_file, c.ssh_port,
                    args.start_time_ms, c.namespace_to_config, c.ysql_port,
                    c.ycql_port, c.redis_port, c.enable_tls_client)
            # TODO: use paramiko to establish ssh connection to the nodes.
            if node in master_nodes:
                coordinator.add_check(
                    checker, "check_uptime_for_process", "yb-master")
                coordinator.add_check(checker, "check_for_fatal_logs", "yb-master")
            if node in tserver_nodes:
                coordinator.add_check(
                    checker, "check_uptime_for_process", "yb-tserver")
                coordinator.add_check(checker, "check_for_fatal_logs", "yb-tserver")
                # Only need to check redis-cli/cqlsh for tserver nodes, to be docker/k8s friendly.
                coordinator.add_check(checker, "check_cqlsh")
                coordinator.add_check(checker, "check_redis_cli")
                # TODO: Enable check after addressing issue #1845.
                if c.enable_ysql:
                    coordinator.add_check(checker, "check_ysqlsh")
            coordinator.add_check(checker, "check_disk_utilization")
            coordinator.add_check(checker, "check_for_core_files")
            coordinator.add_check(checker, "check_file_descriptors")

    entries = coordinator.run()
    for e in entries:
        report.add_entry(e)

    state = "ERROR" if report.has_errors() else "OK"
    subject = "{} - <{}> {}".format(state, args.customer_tag, args.universe_name)
    # If we have errors or were asked to send email with status update, then send the email.
    #
    # NOTE: We only send emails if we are provided a destination, which can be a CSV of addresses.
    if args.destination and (args.send_status or report.has_errors()):
        try:
            send_mail(
                report, subject, args.destination, summary_nodes.keys(),
                args.universe_name, args.report_only_errors
            )
        except Exception as e:
            logging.error("Sending email failed with: {}".format(str(e)))
            report.mail_error = str(e)
    report.write_to_log(args.log_file)

    # Write to stdout to be caught by YW subprocess.
    print(report)


if __name__ == '__main__':
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s: %(message)s")
    main()
