#!/usr/bin/env python3

# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

"""
Run YugaByte tests on Spark using PySpark.

Example (mostly useful during testing this script):

cd ~/code/yugabyte

Run all C++ tests:

"$SPARK_INSTALLATION_DIR/bin/spark-submit" \
    build-support/run_tests_on_spark.py \
    --spark-master-url=spark://$SPARK_HOST:$SPARK_PORT \
    --build-root "$PWD/build/release-gcc-dynamic-ninja" \
    --verbose \
    --reports-dir /tmp \
    --write_report \
    --save_report_to_build_dir \
    --cpp \
    --recreate_archive_for_workers

Run Java tests satisfying a particular regex:

"$SPARK_INSTALLATION_DIR/bin/spark-submit" \
    build-support/run_tests_on_spark.py \
    --spark-master-url=spark://$SPARK_HOST:$SPARK_PORT \
    --build-root "$PWD/build/release-gcc-dynamic-ninja" \
    --verbose \
    --reports-dir=/tmp \
    --write_report \
    --save_report_to_build_dir \
    --java \
    --test_filter_re=org[.]yb[.].*Pg.* \
    --send_archive_to_workers \
    --recreate_archive_for_workers
"""

import argparse
import getpass
import glob
import gzip
import json
import logging
import os
import pwd
import random
import re
import socket
import subprocess
import sys
import time
import traceback
import tempfile
import errno
import signal

from collections import defaultdict

from typing import List

BUILD_SUPPORT_DIR = os.path.dirname(os.path.realpath(__file__))

# An upper bound on a single test's running time. In practice there are multiple other timeouts
# that should be triggered earlier.
TEST_TIMEOUT_UPPER_BOUND_SEC = 35 * 60

# We wait for a special "flag file" to appear indicating that the test has started running, which is
# created by the about_to_start_running_test in common-test-env.sh. If this does not happen within
# this amount of time, we terminate the run-test.sh script. This should prevent tests getting stuck
# for a long time in macOS builds.
TIME_SEC_TO_START_RUNNING_TEST = 5 * 60


def wait_for_path_to_exist(target_path):
    if os.path.exists(target_path):
        return
    waited_for_sec = 0
    start_time_sec = time.time()
    printed_msg_at_sec = 0
    MSG_PRINT_INTERVAL_SEC = 5.0
    TIMEOUT_SEC = 120
    while not os.path.exists(target_path):
        current_time_sec = time.time()
        if current_time_sec - printed_msg_at_sec >= MSG_PRINT_INTERVAL_SEC:
            sys.stderr.write("Path '%s' does not exist, waiting\n" % target_path)
            printed_msg_at_sec = current_time_sec
        if current_time_sec - start_time_sec >= TIMEOUT_SEC:
            raise IOError(
                "Timed out after %.1f seconds waiting for path to exist: %s" % (
                    current_time_sec - start_time_sec, target_path
                ))
        time.sleep(0.1)
    elapsed_time = time.time() - start_time_sec
    sys.stderr.write("Waited for %.1f seconds for the path '%s' to appear\n" % (
        elapsed_time, target_path
    ))


sys.path.append(os.path.join(os.path.dirname(__file__), '..', 'python'))
from yb import yb_dist_tests  # noqa
from yb import command_util  # noqa
from yb.common_util import set_to_comma_sep_str, get_bool_env_var, is_macos  # noqa
from yb.yb_dist_tests import TestDescriptor  # noqa

# Special Jenkins environment variables. They are propagated to tasks running in a distributed way
# on Spark.
JENKINS_ENV_VARS = [
    "BUILD_TYPE",
    "BUILD_ID",
    "BUILD_NUMBER",
    "BUILD_TAG",
    "BUILD_URL",
    "CVS_BRANCH",
    "EXECUTOR_NUMBER",
    "GIT_BRANCH",
    "GIT_COMMIT",
    "GIT_URL",
    "JAVA_HOME",
    "JENKINS_URL",
    "JOB_NAME",
    "NODE_NAME",
    "SVN_REVISION",
    "WORKSPACE",
    ]

# In addition, all variables with names starting with the following prefix are propagated.
PROPAGATED_ENV_VAR_PREFIX = 'YB_'

SPARK_URLS = {
    'linux_default': os.getenv(
        'YB_LINUX_PY3_SPARK_URL',
        'spark://spark-for-yugabyte-linux-default.example.com:7077'),
    'linux_asan_tsan': os.getenv(
        'YB_ASAN_TSAN_PY3_SPARK_URL',
        'spark://spark-for-yugabyte-linux-asan-tsan.example.com:7077'),
    'macos': os.getenv(
        'YB_MACOS_PY3_SPARK_URL',
        'spark://spark-for-yugabyte-macos.example.com:7077'),
}

# This has to match what we output in run-test.sh if YB_LIST_CTEST_TESTS_ONLY is set.
CTEST_TEST_PROGRAM_RE = re.compile(r'^.* ctest test: \"(.*)\"$')

# Non-gtest tests and tests with internal dependencies that we should run in one shot. This almost
# duplicates a  from common-test-env.sh, but that is probably OK since we should not be adding new
# such tests.
ONE_SHOT_TESTS = set([
        'merge_test',
        'c_test',
        'compact_on_deletion_collector_test',
        'db_sanity_test',
        'tests-rocksdb/thread_local_test'])

HASH_COMMENT_RE = re.compile('#.*$')

# Number of failures of any particular task before giving up on the job. The total number of
# failures spread across different tasks will not cause the job to fail; a particular task has to
# fail this number of attempts. Should be greater than or equal to 1. Number of allowed retries =
# this value - 1.
SPARK_TASK_MAX_FAILURES = 100

# Global variables. Some of these are used on the remote worker side.
verbose = False
g_spark_master_url_override = None
propagated_env_vars = {}
global_conf_dict = None
spark_context = None
archive_sha256sum = None


def configure_logging():
    log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="[%(filename)s:%(lineno)d] %(asctime)s %(levelname)s: %(message)s")


def is_pid_running(pid):
    import psutil
    try:
        process = psutil.Process(pid)
        return process.status() != psutil.STATUS_ZOMBIE
    except psutil.NoSuchProcess:
        # Not running.
        return False


def delete_if_exists_log_errors(file_path):
    if os.path.exists(file_path):
        try:
            os.remove(file_path)
        except OSError as os_error:
            logging.error("Error deleting file %s: %s", file_path, os_error)


def log_heading(msg):
    logging.info('\n%s\n%s\n%s' % ('-' * 80, msg, '-' * 80))


# Initializes the spark context. The details list will be incorporated in the Spark application
# name visible in the Spark web UI.
def init_spark_context(details=[]):
    global spark_context
    if spark_context:
        return
    log_heading("Initializing Spark context")
    global_conf = yb_dist_tests.global_conf
    build_type = yb_dist_tests.global_conf.build_type
    from pyspark import SparkContext
    SparkContext.setSystemProperty('spark.task.maxFailures', str(SPARK_TASK_MAX_FAILURES))

    spark_master_url = g_spark_master_url_override
    if spark_master_url is None:
        if is_macos():
            logging.info("This is macOS, using the macOS Spark cluster")
            spark_master_url = SPARK_URLS['macos']
        elif yb_dist_tests.global_conf.build_type in ['asan', 'tsan']:
            logging.info("Using a separate Spark cluster for ASAN and TSAN tests")
            spark_master_url = SPARK_URLS['linux_asan_tsan']
        else:
            logging.info("Using the regular Spark cluster for non-ASAN/TSAN tests")
            spark_master_url = SPARK_URLS['linux_default']

    logging.info("Spark master URL: %s", spark_master_url)
    spark_master_url = os.environ.get('YB_SPARK_MASTER_URL', spark_master_url)
    details += [
        'user: {}'.format(getpass.getuser()),
        'build type: {}'.format(build_type)
        ]

    if 'BUILD_URL' in os.environ:
        details.append('URL: {}'.format(os.environ['BUILD_URL']))

    SparkContext.setSystemProperty("spark.pyspark.python", "/usr/local/bin/python3")
    spark_context = SparkContext(spark_master_url, "YB tests: {}".format(' '.join(details)))
    yb_python_zip_path = yb_dist_tests.get_tmp_filename(
            prefix='yb_python_module_for_spark_workers_', suffix='.zip', auto_remove=True)
    logging.info("Creating a zip archive with the yb python module at %s", yb_python_zip_path)
    subprocess.check_call(
            ['zip', '--recurse-paths', '--quiet', yb_python_zip_path, 'yb',
             '-x', '*.sw?', '-x', '*.pyc'],
            cwd=os.path.join(global_conf.yb_src_root, 'python'))
    spark_context.addPyFile(yb_python_zip_path)
    if global_conf.archive_for_workers is not None:
        logging.info("Will send the archive %s to all Spark workers",
                     global_conf.archive_for_workers)
        spark_context.addFile(global_conf.archive_for_workers)

    log_heading("Initialized Spark context")


def set_global_conf_for_spark_jobs():
    global global_conf_dict
    global_conf_dict = vars(yb_dist_tests.global_conf)


def get_bash_path():
    if sys.platform == 'darwin':
        return '/usr/local/bin/bash'
    return '/bin/bash'


def parallel_run_test(test_descriptor_str):
    """
    This is invoked in parallel to actually run tests.
    """
    global_conf = initialize_remote_task()
    from yb import yb_dist_tests, command_util

    wait_for_path_to_exist(global_conf.build_root)

    # Created files/directories will be writable by the group.
    old_umask = os.umask(2)

    test_descriptor = yb_dist_tests.TestDescriptor(test_descriptor_str)

    # This is saved in the test result file by process_test_result.py.
    os.environ['YB_TEST_DESCRIPTOR_STR'] = test_descriptor_str

    os.environ['YB_TEST_ATTEMPT_INDEX'] = str(test_descriptor.attempt_index)
    os.environ['build_type'] = global_conf.build_type
    os.environ['YB_RUNNING_TEST_ON_SPARK'] = '1'
    os.environ['BUILD_ROOT'] = global_conf.build_root

    test_started_running_flag_file = yb_dist_tests.get_tmp_filename(
            prefix='yb_test_started_running_flag_file')

    os.environ['YB_TEST_STARTED_RUNNING_FLAG_FILE'] = test_started_running_flag_file
    os.environ['YB_TEST_EXTRA_ERROR_LOG_PATH'] = test_descriptor.error_output_path

    artifact_list_path = yb_dist_tests.get_tmp_filename(
            prefix='yb_test_artifact_list', suffix='.txt')
    os.environ['YB_TEST_ARTIFACT_LIST_PATH'] = artifact_list_path
    logging.info("Setting YB_TEST_ARTIFACT_LIST_PATH to %s", artifact_list_path)

    # We could use "run_program" here, but it collects all the output in memory, which is not
    # ideal for a large amount of test log output. The "tee" part also makes the output visible in
    # the standard error of the Spark task as well, which is sometimes helpful for debugging.
    def run_test():
        start_time_sec = time.time()
        error_log_dir_path = os.path.dirname(os.path.abspath(test_descriptor.error_output_path))
        if not os.path.isdir(error_log_dir_path):
            subprocess.check_call(['mkdir', '-p', error_log_dir_path])
        runner_oneline = 'set -o pipefail; "%s" %s 2>&1 | tee "%s"; exit ${PIPESTATUS[0]}' % (
            global_conf.get_run_test_script_path(),
            test_descriptor.args_for_run_test,
            test_descriptor.error_output_path
        )
        process = subprocess.Popen(
            [get_bash_path(), '-c', runner_oneline]
        )

        found_flag_file = False
        while is_pid_running(process.pid):
            elapsed_time_sec = time.time() - start_time_sec
            termination_reason = None
            if elapsed_time_sec > TEST_TIMEOUT_UPPER_BOUND_SEC:
                termination_reason = 'ran longer than %d seconds' % TEST_TIMEOUT_UPPER_BOUND_SEC

            failed_to_launch = False
            if not found_flag_file:
                if os.path.exists(test_started_running_flag_file):
                    found_flag_file = True
                elif elapsed_time_sec > TIME_SEC_TO_START_RUNNING_TEST:
                    termination_reason = (
                        'could not start running the test in %d seconds (file %s not created). '
                        'Ran command: {{ %s }}.'
                    ) % (
                        TIME_SEC_TO_START_RUNNING_TEST,
                        test_started_running_flag_file,
                        runner_oneline
                    )
                    failed_to_launch = True

            if termination_reason:
                error_msg = "Test %s is being terminated (ran for %.1f seconds), reason: %s" % (
                    test_descriptor, elapsed_time_sec, termination_reason
                )
                logging.info(error_msg)
                try:
                    os.kill(process.pid, signal.SIGKILL)
                except OSError as os_error:
                    if os_error.errno == errno.ESRCH:
                        logging.info(
                            "Process with pid %d disappeared suddenly, that's OK",
                            process.pid)
                        pass
                    raise os_error

                if failed_to_launch:
                    # This exception should bubble up to Spark and cause it to hopefully re-run the
                    # test one some other node.
                    raise RuntimeError(error_msg)
                break

            time.sleep(0.5)

        exit_code = process.wait()

        elapsed_time_sec = time.time() - start_time_sec
        logging.info("Test %s ran on %s in %.1f seconds, rc=%d",
                     test_descriptor, socket.gethostname(), elapsed_time_sec, exit_code)
        return exit_code, elapsed_time_sec

    # End of the local run_test() function.

    try:
        exit_code, elapsed_time_sec = run_test()
        error_output_path = test_descriptor.error_output_path

        failed_without_output = False
        if os.path.isfile(error_output_path) and os.path.getsize(error_output_path) == 0:
            # Empty error output file (<something>__error.log).
            if exit_code == 0:
                # Test succeeded, no error output.
                os.remove(error_output_path)
            else:
                failed_without_output = True

        artifact_paths = []
        num_errors_copying_artifacts = 0

        rel_artifact_paths = None
        if global_conf.archive_for_workers:
            artifact_paths = [error_output_path]
            if os.path.exists(artifact_list_path):
                with open(artifact_list_path) as artifact_list_file:
                    for artifact_path_pattern in artifact_list_file:
                        artifact_path_pattern = artifact_path_pattern.strip()
                        if not artifact_path_pattern:
                            continue
                        logging.info("Artifact pattern to copy to main build host: '%s'",
                                     artifact_path_pattern)
                        glob_result = glob.glob(os.path.abspath(artifact_path_pattern))
                        artifact_paths.extend(glob_result)
                        if not glob_result:
                            logging.warning("No artifacts found for pattern: '%s'",
                                            artifact_path_pattern)
            else:
                logging.warning("Artifact list does not exist: '%s'", artifact_list_path)

            if is_macos() and socket.gethostname() == os.environ.get('YB_BUILD_HOST'):
                logging.info("Files already local to build host. Skipping artifact copy.")
            else:
                num_artifacts_copied = 0
                for artifact_path in artifact_paths:
                    if not os.path.exists(artifact_path):
                        logging.warning("Build artifact file does not exist: '%s'", artifact_path)
                        continue
                    if is_macos():
                        dest_path = get_mac_shared_nfs(artifact_path)
                    else:
                        dest_path = yb_dist_tests.to_real_nfs_path(artifact_path)
                    dest_dir = os.path.dirname(dest_path)
                    if not os.path.exists(dest_dir):
                        logging.info("Creating directory %s", dest_dir)
                        subprocess.check_call(['mkdir', '-p', dest_dir])
                    logging.info("Copying %s to %s", artifact_path, dest_path)
                    try:
                        subprocess.check_call(['cp', '-f', artifact_path, dest_path])
                    except subprocess.CalledProcessError as ex:
                        logging.error("Error copying %s to %s: %s", artifact_path, dest_path, ex)
                        num_errors_copying_artifacts = 1

                    num_artifacts_copied += 1
                logging.info("Number of build artifact files copied: %d", num_artifacts_copied)

            rel_artifact_paths = [
                    os.path.relpath(os.path.abspath(artifact_path), global_conf.yb_src_root)
                    for artifact_path in artifact_paths
            ]
        return yb_dist_tests.TestResult(
                exit_code=exit_code,
                test_descriptor=test_descriptor,
                elapsed_time_sec=elapsed_time_sec,
                failed_without_output=failed_without_output,
                artifact_paths=rel_artifact_paths,
                num_errors_copying_artifacts=num_errors_copying_artifacts)
    finally:
        delete_if_exists_log_errors(test_started_running_flag_file)
        delete_if_exists_log_errors(artifact_list_path)
        os.umask(old_umask)


def get_bash_shebang():
    # Prefer /usr/local/bin/bash as we install Bash 4+ there on macOS.
    if os.path.exists('/usr/local/bin/bash'):
        return '/usr/local/bin/bash'
    return '/usr/bin/env bash'


def initialize_remote_task():
    configure_logging()

    global_conf = yb_dist_tests.set_global_conf_from_dict(global_conf_dict)
    global_conf.set_env_on_spark_worker(propagated_env_vars)
    if not global_conf.archive_for_workers:
        return global_conf

    from pyspark import SparkFiles
    archive_name = os.path.basename(SparkFiles.get(global_conf.archive_for_workers))
    expected_archive_sha256sum = global_conf.archive_sha256sum
    assert expected_archive_sha256sum is not None

    worker_tmp_dir = os.path.abspath(SparkFiles.getRootDirectory())
    archive_path = os.path.join(worker_tmp_dir, archive_name)
    if not os.path.exists(archive_path):
        raise IOError("Archive not found: %s" % archive_path)
    # We install the code into the same path where it was installed on the main build node (Jenkins
    # slave or dev server), but put it in as separate variable to have flexibility to change it
    # later.
    remote_yb_src_root = global_conf.yb_src_root

    subprocess.check_call([
        'mkdir',
        '-p',
        os.path.dirname(remote_yb_src_root)])
    try:
        untar_script_path = os.path.join(
                worker_tmp_dir, 'untar_archive_once_%d.sh' % random.randint(0, 2**64))
        # We also copy the temporary script here for later reference.
        untar_script_path_for_reference = os.path.join(
                worker_tmp_dir, 'untar_archive_once.sh')
        lock_path = '/tmp/yb_dist_tests_update_archive%s.lock' % (
                global_conf.yb_src_root.replace('/', '__'))
        bash_shebang = get_bash_shebang()
        with open(untar_script_path, 'w') as untar_script_file:
            # Do the locking using the flock command in Bash -- file locking in Python is painful.
            # Some curly braces in the script template are escaped as "{{" and }}".

            # TODO: rewrite this shell script in Python, except for the flock part.
            untar_script_file.write("""#!{bash_shebang}
set -euo pipefail
(
    PATH=/usr/local/bin:$PATH
    flock -w 60 200
    if [[ -d '{remote_yb_src_root}' ]]; then
        previous_sha256_file_path='{remote_yb_src_root}/extracted_from_archive.sha256'
        if [[ ! -f $previous_sha256_file_path ]]; then
            # Prevent accidental deletion of directories that were not installed by untarring
            # an archive.
            echo "File $previous_sha256_file_path does not exist!" >&2
            exit 1
        fi
        previous_sha256sum=$(<"$previous_sha256_file_path")
        if [[ $previous_sha256sum == '{expected_archive_sha256sum}' ]]; then
            echo "Found existing archive installation at '{remote_yb_src_root}' with correct" \
                 "expected checksum '$previous_sha256sum'."
        else
            echo "Removing '{remote_yb_src_root}': it was installed from archive with checksum" \
                 "'$previous_sha256sum' but we are installing one with checksum" \
                 "'{expected_archive_sha256sum}'."
            rm -rf '{remote_yb_src_root}'
        fi
    fi
    if [[ ! -d '{remote_yb_src_root}' ]]; then
        if [[ ! -f '{untar_script_path_for_reference}' ]]; then
            cp '{untar_script_path}' '{untar_script_path_for_reference}'
        fi
        actual_archive_sha256sum=$( (
            [[ $OSTYPE == linux* ]] && sha256sum '{archive_path}' ||
            shasum --portable --algorithm 256 '{archive_path}'
        ) | awk '{{ print $1 }}' )
        if [[ $actual_archive_sha256sum != '{expected_archive_sha256sum}' ]]; then
          echo "Archive SHA256 sum of '{archive_path}' is $actual_archive_sha256sum, which" \
               "does not match expected value: {expected_archive_sha256sum}." >&2
          exit 1
        fi
        chmod 0755 '{untar_script_path_for_reference}'
        yb_src_root_extract_tmp_dir='{remote_yb_src_root}'.$RANDOM.$RANDOM.$RANDOM.$RANDOM
        mkdir "$yb_src_root_extract_tmp_dir"
        if [[ -x /bin/pigz ]]; then
            # Decompress faster with pigz
            /bin/pigz -dc '{archive_path}' | tar xf - -C "$yb_src_root_extract_tmp_dir"
        else
            tar xzf '{archive_path}' -C "$yb_src_root_extract_tmp_dir"
        fi
        echo '{expected_archive_sha256sum}' \
                >"$yb_src_root_extract_tmp_dir/extracted_from_archive.sha256"
        mv "$yb_src_root_extract_tmp_dir" '{remote_yb_src_root}'
    fi
)  200>'{lock_path}'
""".format(**locals()))
        os.chmod(untar_script_path, 0o755)
        subprocess.check_call(untar_script_path)
    finally:
        if os.path.exists(untar_script_path):
            os.remove(untar_script_path)

    return global_conf


def parallel_list_test_descriptors(rel_test_path):
    """
    This is invoked in parallel to list all individual tests within our C++ test programs. Without
    this, listing all gtest tests across 330 test programs might take about 5 minutes on TSAN and 2
    minutes in debug.
    """

    from yb import yb_dist_tests, command_util
    global_conf = initialize_remote_task()

    find_or_download_thirdparty_script_path = os.path.join(
        global_conf.yb_src_root, 'build-support', 'find_or_download_thirdparty.sh')
    subprocess.check_call(find_or_download_thirdparty_script_path)

    wait_for_path_to_exist(global_conf.build_root)
    list_tests_cmd_line = [
            os.path.join(global_conf.build_root, rel_test_path), '--gtest_list_tests']

    try:
        prog_result = command_util.run_program(list_tests_cmd_line)
    except OSError as ex:
        logging.error("Failed running the command: %s", list_tests_cmd_line)
        raise

    # --gtest_list_tests gives us the following output format:
    #  TestSplitArgs.
    #    Simple
    #    SimpleWithSpaces
    #    SimpleWithQuotes
    #    BadWithQuotes
    #    Empty
    #    Error
    #    BloomFilterReverseCompatibility
    #    BloomFilterWrapper
    #    PrefixExtractorFullFilter
    #    PrefixExtractorBlockFilter
    #    PrefixScan
    #    OptimizeFiltersForHits
    #  BloomStatsTestWithParam/BloomStatsTestWithParam.
    #    BloomStatsTest/0  # GetParam() = (true, true)
    #    BloomStatsTest/1  # GetParam() = (true, false)
    #    BloomStatsTest/2  # GetParam() = (false, false)
    #    BloomStatsTestWithIter/0  # GetParam() = (true, true)
    #    BloomStatsTestWithIter/1  # GetParam() = (true, false)
    #    BloomStatsTestWithIter/2  # GetParam() = (false, false)

    current_test = None
    test_descriptors = []
    test_descriptor_prefix = rel_test_path + yb_dist_tests.TEST_DESCRIPTOR_SEPARATOR
    for line in prog_result.stdout.split("\n"):
        if ('Starting tracking the heap' in line or 'Dumping heap profile to' in line):
            continue
        line = line.rstrip()
        trimmed_line = HASH_COMMENT_RE.sub('', line.strip()).strip()
        if line.startswith('  '):
            test_descriptors.append(test_descriptor_prefix + current_test + trimmed_line)
        else:
            current_test = trimmed_line

    return test_descriptors


def get_username():
    try:
        return os.getlogin()
    except OSError as ex:
        logging.warning(("Got an OSError trying to get the current user name, " +
                         "trying a workaround: {}").format(ex))
        # https://github.com/gitpython-developers/gitpython/issues/39
        try:
            return pwd.getpwuid(os.getuid()).pw_name
        except KeyError as ex:
            user_from_env = os.getenv('USER')
            if user_from_env:
                return user_from_env
            id_output = subprocess.check_output('id').strip()
            ID_OUTPUT_RE = re.compile(r'^uid=\d+[(]([^)]+)[)]\s.*')
            match = ID_OUTPUT_RE.match(id_output)
            if match:
                return match.group(1)
            logging.warning(
                "Could not get user name from the environment, and could not parse 'id' output: %s",
                id_output)
            raise ex


def get_mac_shared_nfs(path):
    LOCAL_PATH = "/Volumes/share"
    if not path.startswith(LOCAL_PATH):
        raise ValueError("Local path %s does not start with expected prefix '%s'.\n" %
                         (path, LOCAL_PATH))
    relpath = path[len(LOCAL_PATH):]
    return "/Volumes/net/v1/" + os.environ.get('YB_BUILD_HOST') + relpath


def get_jenkins_job_name():
    return os.environ.get('JOB_NAME', None)


def get_jenkins_job_name_path_component():
    jenkins_job_name = get_jenkins_job_name()
    if jenkins_job_name:
        return "job_" + jenkins_job_name
    else:
        return "unknown_jenkins_job"


def get_report_parent_dir(report_base_dir):
    """
    @return a directory to store build report, relative to the given base directory. Path components
            are based on build type, Jenkins job name, etc.
    """
    return os.path.join(report_base_dir,
                        yb_dist_tests.global_conf.build_type,
                        get_jenkins_job_name_path_component())


def save_json_to_paths(short_description, json_data, output_paths, should_gzip=False):
    """
    Saves the given JSON-friendly data structure to the list of paths (exact copy at each path),
    optionally gzipping the output.
    """
    json_data_str = json.dumps(json_data, sort_keys=True, indent=2) + "\n"

    for output_path in output_paths:
        if output_path is None:
            continue

        assert output_path.endswith('.json'), \
            "Expected output path to end with .json: {}".format(output_path)
        final_output_path = output_path + ('.gz' if should_gzip else '')
        logging.info("Saving {} to {}".format(short_description, final_output_path))
        if should_gzip:
            with gzip.open(final_output_path, 'wb') as output_file:
                output_file.write(json_data_str.encode('utf-8'))
        else:
            with open(final_output_path, 'w') as output_file:
                output_file.write(json_data_str)


def save_report(report_base_dir, results, total_elapsed_time_sec, spark_succeeded,
                save_to_build_dir=False):
    historical_report_path = None
    global_conf = yb_dist_tests.global_conf

    if report_base_dir:
        historical_report_parent_dir = get_report_parent_dir(report_base_dir)

        if not os.path.isdir(historical_report_parent_dir):
            try:
                os.makedirs(historical_report_parent_dir)
            except OSError as exc:
                if exc.errno == errno.EEXIST and os.path.isdir(historical_report_parent_dir):
                    pass
                raise

        try:
            username = get_username()
        except:  # noqa
            logging.error("Could not get username, using 'unknown_user':\n%s",
                          traceback.format_exc())
            username = "unknown_user"

        historical_report_path = os.path.join(
                historical_report_parent_dir,
                '{}_{}__user_{}__build_{}.json'.format(
                    global_conf.build_type,
                    time.strftime('%Y-%m-%dT%H_%M_%S'),
                    username,
                    get_jenkins_job_name_path_component(),
                    os.environ.get('BUILD_ID', 'unknown')))

    test_reports_by_descriptor = {}
    for result in results:
        test_descriptor = result.test_descriptor
        test_report_dict = dict(
            elapsed_time_sec=result.elapsed_time_sec,
            exit_code=result.exit_code,
            language=test_descriptor.language,
            rtifact_paths=result.artifact_paths
        )
        test_reports_by_descriptor[test_descriptor.descriptor_str] = test_report_dict
        if test_descriptor.error_output_path and os.path.isfile(test_descriptor.error_output_path):
            test_report_dict['error_output_path'] = test_descriptor.error_output_path

    jenkins_env_var_values = {}
    for jenkins_env_var_name in JENKINS_ENV_VARS:
        if jenkins_env_var_name in os.environ:
            jenkins_env_var_values[jenkins_env_var_name] = os.environ[jenkins_env_var_name]

    report = dict(
        conf=vars(yb_dist_tests.global_conf),
        total_elapsed_time_sec=total_elapsed_time_sec,
        spark_succeeded=spark_succeeded,
        jenkins_env_vars=jenkins_env_var_values,
        tests=test_reports_by_descriptor
        )

    full_report_paths = []
    if historical_report_path:
        full_report_paths.append(historical_report_path)
    if save_to_build_dir:
        full_report_paths.append(os.path.join(global_conf.build_root, 'full_build_report.json'))

    save_json_to_paths('full build report', report, full_report_paths, should_gzip=True)

    if save_to_build_dir:
        del report['tests']
        short_report_path = os.path.join(global_conf.build_root, 'short_build_report.json')
        save_json_to_paths('short build report', report, [short_report_path], should_gzip=False)


def is_one_shot_test(rel_binary_path):
    if rel_binary_path in ONE_SHOT_TESTS:
        return True
    for non_gtest_test in ONE_SHOT_TESTS:
        if rel_binary_path.endswith('/' + non_gtest_test):
            return True
    return False


def collect_cpp_tests(cpp_test_program_filter: List[str]) -> List[yb_dist_tests.TestDescriptor]:
    """
    Collect C++ test programs to run.
    @param cpp_test_program_filter: a list of C++ test program names to be used as a filter
    """

    global_conf = yb_dist_tests.global_conf
    logging.info("Collecting the list of C++ test programs (locally; not a Spark job)")
    start_time_sec = time.time()
    build_root_realpath = os.path.realpath(global_conf.build_root)
    ctest_cmd_result = command_util.run_program(
            ['/bin/bash',
             '-c',
             'cd "{}" && YB_LIST_CTEST_TESTS_ONLY=1 ctest -j8 --verbose'.format(
                 build_root_realpath)])
    test_programs = []

    for line in ctest_cmd_result.stdout.split("\n"):
        re_match = CTEST_TEST_PROGRAM_RE.match(line)
        if re_match:
            ctest_test_program = re_match.group(1)
            if ctest_test_program.startswith('/'):
                ctest_test_program = os.path.realpath(ctest_test_program)
            rel_ctest_prog_path = os.path.relpath(
                    os.path.realpath(ctest_test_program),
                    build_root_realpath)
            if rel_ctest_prog_path.startswith('../'):
                raise ValueError(
                    "Relative path to a ctest test binary ended up starting with '../', something "
                    "must be wrong: %s" % rel_ctest_prog_path)
            test_programs.append(rel_ctest_prog_path)

    test_programs = sorted(set(test_programs))
    elapsed_time_sec = time.time() - start_time_sec
    logging.info("Collected %d test programs in %.2f sec" % (
        len(test_programs), elapsed_time_sec))

    if cpp_test_program_filter:
        cpp_test_program_filter = set(cpp_test_program_filter)
        unfiltered_test_programs = test_programs

        # test_program contains test paths relative to the root directory (including directory
        # names), and cpp_test_program_filter contains basenames only.
        test_programs = sorted(set([
                test_program for test_program in test_programs
                if os.path.basename(test_program) in cpp_test_program_filter
            ]))

        logging.info("Filtered down to %d test programs using the list from test conf file" %
                     len(test_programs))
        if unfiltered_test_programs and not test_programs:
            # This means we've filtered the list of C++ test programs down to an empty set.
            logging.info(
                    ("NO MATCHING C++ TEST PROGRAMS FOUND! Test programs from conf file: {}, "
                     "collected from ctest before filtering: {}").format(
                         set_to_comma_sep_str(cpp_test_program_filter),
                         set_to_comma_sep_str(unfiltered_test_programs)))

    if not test_programs:
        logging.info("Found no test programs")
        return []

    fine_granularity_gtest_programs = []
    one_shot_test_programs = []
    for test_program in test_programs:
        if is_one_shot_test(test_program):
            one_shot_test_programs.append(test_program)
        else:
            fine_granularity_gtest_programs.append(test_program)

    logging.info(("Found {} gtest test programs where tests will be run separately, "
                  "{} test programs to be run on one shot").format(
                    len(fine_granularity_gtest_programs),
                    len(one_shot_test_programs)))

    test_programs = fine_granularity_gtest_programs
    logging.info(
        "Collecting gtest tests for {} test programs where tests will be run separately".format(
            len(test_programs)))

    start_time_sec = time.time()

    all_test_programs = fine_granularity_gtest_programs + one_shot_test_programs
    if len(all_test_programs) <= 5:
        app_name_details = ['test programs: [{}]'.format(', '.join(all_test_programs))]
    else:
        app_name_details = ['{} test programs'.format(len(all_test_programs))]

    init_spark_context(app_name_details)
    set_global_conf_for_spark_jobs()

    # Use fewer "slices" (tasks) than there are test programs, in hope to get some batching.
    num_slices = (len(test_programs) + 1) / 2
    all_test_descriptor_lists = run_spark_action(
        lambda: spark_context.parallelize(
            test_programs, numSlices=num_slices).map(parallel_list_test_descriptors).collect()
    )
    elapsed_time_sec = time.time() - start_time_sec
    test_descriptor_strs = one_shot_test_programs + [
        test_descriptor_str
        for test_descriptor_str_list in all_test_descriptor_lists
        for test_descriptor_str in test_descriptor_str_list]
    logging.info("Collected the list of %d gtest tests in %.2f sec" % (
        len(test_descriptor_strs), elapsed_time_sec))

    return [yb_dist_tests.TestDescriptor(s) for s in test_descriptor_strs]


def is_writable(dir_path):
    return os.access(dir_path, os.W_OK)


def is_parent_dir_writable(file_path):
    return is_writable(os.path.dirname(file_path))


def fatal_error(msg):
    logging.error("Fatal: " + msg)
    raise RuntimeError(msg)


def get_java_test_descriptors():
    java_test_list_path = os.path.join(yb_dist_tests.global_conf.build_root, 'java_test_list.txt')
    if not os.path.exists(java_test_list_path):
        raise IOError(
            "Java test list not found at '%s'. Please run ./yb_build.sh --collect-java-tests to "
            "generate the test list file." % java_test_list_path)
    with open(java_test_list_path) as java_test_list_file:
        java_test_descriptors = []
        for line in java_test_list_file:
            line = line.strip()
            if not line:
                continue
            java_test_descriptors.append(yb_dist_tests.TestDescriptor(line))
    if not java_test_descriptors:
        raise RuntimeError("Could not find any Java tests in '%s'" % java_test_list_path)

    logging.info("Found %d Java tests", len(java_test_descriptors))
    return java_test_descriptors


def collect_tests(args):
    test_conf = {}
    if args.test_conf:
        with open(args.test_conf) as test_conf_file:
            test_conf = json.load(test_conf_file)
        if args.run_cpp_tests and not test_conf['run_cpp_tests']:
            logging.info("The test configuration file says that C++ tests should be skipped")
            args.run_cpp_tests = False
        if args.run_java_tests and not test_conf['run_java_tests']:
            logging.info(
                "The test configuration file says that Java tests should be skipped")
            args.run_java_tests = False
        if 'test_filter_re' in test_conf:
            args.test_filter_re = test_conf['test_filter_re']

    cpp_test_descriptors = []
    if args.run_cpp_tests:
        cpp_test_programs = test_conf.get('cpp_test_programs')
        cpp_test_descriptors = collect_cpp_tests(cpp_test_programs)

    java_test_descriptors = []
    if args.run_java_tests:
        java_test_descriptors = get_java_test_descriptors()

    test_descriptors = sorted(java_test_descriptors) + sorted(cpp_test_descriptors)

    if args.test_filter_re:
        test_filter_re_compiled = re.compile(args.test_filter_re)
        num_tests_before_filtering = len(test_descriptors)
        test_descriptors = [
            test_descriptor for test_descriptor in test_descriptors
            if test_filter_re_compiled.match(test_descriptor.descriptor_str_without_attempt_index)
        ]
        logging.info(
            "Filtered %d tests using regular expression %s to %d tests",
            num_tests_before_filtering,
            args.test_filter_re,
            len(test_descriptors)
        )

    return test_descriptors


def load_test_list(test_list_path):
    logging.info("Loading the list of tests to run from %s", test_list_path)
    test_descriptors = []
    with open(test_list_path, 'r') as input_file:
        for line in input_file:
            line = line.strip()
            if line:
                test_descriptors.append(yb_dist_tests.TestDescriptor(line))
    return test_descriptors


def propagate_env_vars():
    num_propagated = 0
    for env_var_name in JENKINS_ENV_VARS:
        if env_var_name in os.environ:
            propagated_env_vars[env_var_name] = os.environ[env_var_name]
            num_propagated += 1

    for env_var_name, env_var_value in os.environ.items():
        if env_var_name.startswith(PROPAGATED_ENV_VAR_PREFIX):
            propagated_env_vars[env_var_name] = env_var_value
            logging.info("Propagating env var %s (value: %s) to Spark workers",
                         env_var_name, env_var_value)
            num_propagated += 1
    logging.info("Number of propagated environment variables: %s", num_propagated)


def run_spark_action(action):
    import py4j
    try:
        results = action()
    except py4j.protocol.Py4JJavaError:
        logging.error("Spark job failed to run! Jenkins should probably restart this build.")
        raise

    return results


def main():
    parser = argparse.ArgumentParser(
        description='Run tests on Spark.')
    parser.add_argument('--verbose', action='store_true',
                        help='Enable debug output')
    parser.add_argument('--java', dest='run_java_tests', action='store_true',
                        help='Run Java tests')
    parser.add_argument('--cpp', dest='run_cpp_tests', action='store_true',
                        help='Run C++ tests')
    parser.add_argument('--all', dest='run_all_tests', action='store_true',
                        help='Run tests in all languages')
    parser.add_argument('--test_list',
                        help='A file with a list of tests to run. Useful when e.g. re-running '
                             'failed tests using a file produced with --failed_test_list.')
    parser.add_argument('--build-root', dest='build_root', required=True,
                        help='Build root (e.g. ~/code/yugabyte/build/debug-gcc-dynamic-community)')
    parser.add_argument('--max-tests', type=int, dest='max_tests',
                        help='Maximum number of tests to run. Useful when debugging this script '
                             'for faster iteration. This number of tests will be randomly chosen '
                             'from the test suite.')
    parser.add_argument('--sleep_after_tests', action='store_true',
                        help='Sleep for a while after test are done before destroying '
                             'SparkContext. This allows to examine the Spark app UI.')
    parser.add_argument('--reports-dir', dest='report_base_dir',
                        help='A parent directory for storing build reports (such as per-test '
                             'run times and whether the Spark job succeeded.)')
    parser.add_argument('--write_report', action='store_true',
                        help='Actually enable writing build reports. If this is not '
                             'specified, we will only read previous test reports to sort tests '
                             'better.')
    parser.add_argument('--save_report_to_build_dir', action='store_true',
                        help='Save a test report to the build directory directly, in addition '
                             'to any reports saved in the common reports directory. This should '
                             'work even if neither --reports-dir or --write_report are specified.')
    parser.add_argument('--test_filter_re',
                        help='A regular expression to filter tests')
    parser.add_argument('--test_conf',
                        help='A file with a JSON configuration describing what tests to run, '
                             'produced by dependency_graph.py')
    parser.add_argument('--num_repetitions', type=int, default=1,
                        help='Number of times to run each test.')
    parser.add_argument('--failed_test_list',
                        help='A file path to save the list of failed tests to. The format is '
                             'one test descriptor per line.')
    parser.add_argument('--allow_no_tests', action='store_true',
                        help='Allow running with filters that yield no tests to run. Useful when '
                             'debugging.')
    parser.add_argument('--spark-master-url',
                        default=os.environ.get('YB_SPARK_URL_OVERRIDE'),
                        help='Override Spark master URL to use. Useful for debugging.')
    parser.add_argument('--send_archive_to_workers',
                        action='store_true',
                        default=False,
                        help='Create an archive containing everything required to run tests and '
                             'send it to workers instead of assuming an NFS filesystem.')
    parser.add_argument('--recreate_archive_for_workers',
                        action='store_true',
                        help='When --send_archive_to_workers is specified, use this option to '
                             're-create the archive that we would send to workers even if it '
                             'already exists.')

    args = parser.parse_args()
    global g_spark_master_url_override
    g_spark_master_url_override = args.spark_master_url

    # ---------------------------------------------------------------------------------------------
    # Argument validation.

    if args.run_all_tests:
        args.run_java_tests = True
        args.run_cpp_tests = True

    global verbose
    verbose = args.verbose

    configure_logging()

    if not args.run_cpp_tests and not args.run_java_tests:
        fatal_error("At least one of --java or --cpp has to be specified")

    yb_dist_tests.set_global_conf_from_args(args)

    report_base_dir = args.report_base_dir
    write_report = args.write_report
    if report_base_dir and not os.path.isdir(report_base_dir):
        fatal_error("Report base directory '{}' does not exist".format(report_base_dir))

    if write_report and not report_base_dir:
        fatal_error("--write_report specified but the reports directory (--reports-dir) is not")

    if write_report and not is_writable(report_base_dir):
        fatal_error(
            "--write_report specified but the reports directory ('{}') is not writable".format(
                report_base_dir))

    if args.num_repetitions < 1:
        fatal_error("--num_repetitions must be at least 1, got: {}".format(args.num_repetitions))

    failed_test_list_path = args.failed_test_list
    if failed_test_list_path and not is_parent_dir_writable(failed_test_list_path):
        fatal_error("Parent directory of failed test list destination path ('{}') is not "
                    "writable".format(args.failed_test_list))

    test_list_path = args.test_list
    if test_list_path and not os.path.isfile(test_list_path):
        fatal_error("File specified by --test_list does not exist or is not a file: '{}'".format(
            test_list_path))

    if ('YB_MVN_LOCAL_REPO' not in os.environ and
            args.run_java_tests and
            args.send_archive_to_workers):
        os.environ['YB_MVN_LOCAL_REPO'] = os.path.join(
                yb_dist_tests.global_conf.build_root, 'm2_repository')
        logging.info("Automatically setting YB_MVN_LOCAL_REPO to %s",
                     os.environ['YB_MVN_LOCAL_REPO'])

    if not args.send_archive_to_workers and args.recreate_archive_for_workers:
        fatal_error("Specify --send_archive_to_workers to use --recreate_archive_for_workers")

    # ---------------------------------------------------------------------------------------------
    # End of argument validation.
    # ---------------------------------------------------------------------------------------------

    os.environ['YB_BUILD_HOST'] = socket.gethostname()

    # ---------------------------------------------------------------------------------------------
    # Start the timer.
    global_start_time = time.time()

    # This needs to be done before Spark context initialization, which will happen as we try to
    # collect all gtest tests in all C++ test programs.
    if args.send_archive_to_workers:
        archive_exists = os.path.exists(yb_dist_tests.global_conf.archive_for_workers)
        if args.recreate_archive_for_workers or not archive_exists:
            archive_sha_path = os.path.join(yb_dist_tests.global_conf.yb_src_root,
                                            'extracted_from_archive.sha256')
            if os.path.exists(archive_sha_path):
                os.remove(archive_sha_path)

            yb_dist_tests.create_archive_for_workers()

            yb_dist_tests.compute_archive_sha256sum()

            # Local host may also be worker, so leave expected checksum here after archive created.
            with open(archive_sha_path, 'w') as archive_sha:
                archive_sha.write(yb_dist_tests.global_conf.archive_sha256sum)
        else:
            yb_dist_tests.compute_archive_sha256sum()

    if test_list_path:
        test_descriptors = load_test_list(test_list_path)
    else:
        test_descriptors = collect_tests(args)

    if not test_descriptors and not args.allow_no_tests:
        logging.info("No tests to run")
        return

    propagate_env_vars()

    num_tests = len(test_descriptors)

    if args.max_tests and num_tests > args.max_tests:
        logging.info("Randomly selecting {} tests out of {} possible".format(
                args.max_tests, num_tests))
        random.shuffle(test_descriptors)
        test_descriptors = test_descriptors[:args.max_tests]
        num_tests = len(test_descriptors)

    if args.verbose:
        for test_descriptor in test_descriptors:
            logging.info("Will run test: {}".format(test_descriptor))

    num_repetitions = args.num_repetitions
    total_num_tests = num_tests * num_repetitions
    logging.info("Running {} tests on Spark, {} times each, for a total of {} tests".format(
        num_tests, num_repetitions, total_num_tests))

    if num_repetitions > 1:
        test_descriptors = [
            test_descriptor.with_attempt_index(i)
            for test_descriptor in test_descriptors
            for i in range(1, num_repetitions + 1)
        ]

    app_name_details = ['{} tests total'.format(total_num_tests)]
    if num_repetitions > 1:
        app_name_details += ['{} repetitions of {} tests'.format(num_repetitions, num_tests)]
    init_spark_context(app_name_details)

    set_global_conf_for_spark_jobs()

    # By this point, test_descriptors have been duplicated the necessary number of times, with
    # attempt indexes attached to each test descriptor.
    spark_succeeded = False
    if test_descriptors:
        logging.info("Running {} tasks on Spark".format(total_num_tests))
        assert total_num_tests == len(test_descriptors), \
            "total_num_tests={}, len(test_descriptors)={}".format(
                    total_num_tests, len(test_descriptors))

        # Randomize test order to avoid any kind of skew.
        random.shuffle(test_descriptors)

        test_names_rdd = spark_context.parallelize(
                [test_descriptor.descriptor_str for test_descriptor in test_descriptors],
                numSlices=total_num_tests)

        results = run_spark_action(lambda: test_names_rdd.map(parallel_run_test).collect())
    else:
        # Allow running zero tests, for testing the reporting logic.
        results = []

    test_exit_codes = set([result.exit_code for result in results])

    global_exit_code = 0 if test_exit_codes == set([0]) else 1

    logging.info("Tests are done, set of exit codes: %s, tentative global exit code: %s",
                 sorted(test_exit_codes), global_exit_code)
    num_tests_by_language = defaultdict(int)
    failures_by_language = defaultdict(int)
    failed_test_desc_strs = []
    had_errors_copying_artifacts = False
    for result in results:
        test_language = result.test_descriptor.language
        if result.exit_code != 0:
            how_test_failed = ""
            if result.failed_without_output:
                how_test_failed = " without any output"
            logging.info("Test failed%s: %s", how_test_failed, result.test_descriptor)
            failures_by_language[test_language] += 1
            failed_test_desc_strs.append(result.test_descriptor.descriptor_str)
        if result.num_errors_copying_artifacts > 0:
            logging.info("Test had errors copying artifacts to build host: %s",
                         result.test_descriptors)
        num_tests_by_language[test_language] += 1

    if had_errors_copying_artifacts and global_exit_code == 0:
        logging.info("Will return exit code 1 due to errors copying artifacts to build host")
        global_exit_code = 1

    if failed_test_list_path:
        logging.info("Writing the list of failed tests to '{}'".format(failed_test_list_path))
        with open(failed_test_list_path, 'w') as failed_test_file:
            failed_test_file.write("\n".join(failed_test_desc_strs) + "\n")

    for language, num_tests in sorted(num_tests_by_language.items()):
        logging.info("Total tests we ran in {}: {}".format(language, num_tests))

    for language, num_failures in sorted(failures_by_language.items()):
        logging.info("Failures in {} tests: {}".format(language, num_failures))

    total_elapsed_time_sec = time.time() - global_start_time
    logging.info("Total elapsed time: {} sec".format(total_elapsed_time_sec))

    if report_base_dir and write_report or args.save_report_to_build_dir:
        save_report(report_base_dir, results, total_elapsed_time_sec, spark_succeeded,
                    save_to_build_dir=args.save_report_to_build_dir)

    if args.sleep_after_tests:
        # This can be used as a way to keep the Spark app running during debugging while examining
        # its UI.
        time.sleep(600)

    sys.exit(global_exit_code)


if __name__ == '__main__':
    main()
