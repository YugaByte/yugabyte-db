"""
Copyright (c) YugaByte, Inc.

This module provides utility and helper functions to work with a remote server through SSH.
"""

import json
import os
import shlex
import subprocess
import time
import logging

REMOTE_BUILD_HOST_ENV_VAR = 'YB_REMOTE_BUILD_HOST'
DEFAULT_BASE_BRANCH = 'origin/master'
CONFIG_FILE_PATH = '~/.yb_remote_build.json'


def check_output(args):
    bytes = subprocess.check_output(args)
    return bytes.decode('utf-8')


def check_output_line(args):
    return check_output(args).strip()


def check_output_lines(args):
    return [file.strip() for file in check_output(args).split('\n')]


def parse_git_diff_name_status(lines):
    files = ([], [])
    for line in lines:
        tokens = [x.strip() for x in line.split('\t')]
        if len(tokens) == 0 or tokens[0] == '':
            continue
        if tokens[0] == 'D':
            files[1].append(tokens[1])
            continue
        if tokens[0].startswith('R'):
            name = tokens[2]
        else:
            name = tokens[1]
        files[0].append(name)
    return files


def remote_communicate(host, remote_command, error_ok=False):
    if isinstance(remote_command, list):
        remote_command_args = remote_command
    else:
        remote_command_args = [remote_command]

    args = ['ssh', host] + remote_command_args

    logging.info("Running command: %s", args)
    proc = subprocess.Popen(args, shell=False)
    proc.communicate()
    if proc.returncode != 0:
        if error_ok:
            return False
        raise RuntimeError('ssh terminated with code {}'.format(proc.returncode))
    return True


def check_remote_files(escaped_remote_path, host, remote_path, files):
    remote_command = 'cd {0} && git diff --name-status'.format(escaped_remote_path)
    remote_changed, remote_deleted = parse_git_diff_name_status(
        check_output_lines(['ssh', host, remote_command]))
    unexpected = []
    for changed in remote_changed:
        if changed not in files:
            unexpected.append(changed)
    if unexpected:
        command = 'cd {0}'.format(remote_path)
        message = 'Reverting:\n'
        for file_path in unexpected:
            message += '  {0}\n'.format(file_path)
            command += ' && git checkout -- {0}'.format(shlex.quote(file_path))
        print(message)
        remote_communicate(host, command)


def remote_output_line(host, remote_path, command):
    return check_output_line(['ssh', host, 'cd {0} && {1}'.format(remote_path, command)])


def fetch_remote_commit(host, remote_path):
    return remote_output_line(host, remote_path, 'git rev-parse HEAD')


def read_config_file(file_path=CONFIG_FILE_PATH):
    conf_file_path = os.path.expanduser(file_path)
    if not os.path.exists(conf_file_path):
        return None
    with open(conf_file_path) as conf_file:
        return json.load(conf_file)


def apply_default_host_value(host):
    """
    Process the host argument if it's not defined, setting it to env var
    and raising a RuntimeError if it's undefined too.
    :param host: host input argument
    :return: host to use
    """
    if host is None and REMOTE_BUILD_HOST_ENV_VAR in os.environ:
        host = os.environ[REMOTE_BUILD_HOST_ENV_VAR]

    if host is None:
        raise RuntimeError("Please specify host with --host option or {} variable\n".format(
            REMOTE_BUILD_HOST_ENV_VAR))

    return host


def load_profile(args, profile_name="default_profile"):
    """
    Loads the profile from config file if it's defined, initializing given arguments
    in the CLI args map with the ones from profile - if they were omitted in CLI call.
    Also appends 'extra_args' from profile to 'build_args' in args map.
    :param args: parsed CLI arguments map to init missing values with the loaded args
    :param profile_name: name of the profile to load
    :return:
    """
    conf = read_config_file()

    if conf and not profile_name:
        profile_name = conf.get("default_profile", profile_name)

    if profile_name is None:
        return

    profiles = conf['profiles']
    profile = profiles.get(profile_name)
    if profile:
        logging.info("Using profile %s", profile_name)
    else:
        # Match profile using the remote host.
        for profile_name_to_try in profiles:
            if profiles[profile_name_to_try].get('host') == profile_name:
                logging.info("Using profile %s based on the host name", profile_name_to_try)
                profile = profiles[profile_name_to_try]
                break
    if profile is None:
        raise RuntimeError("Unknown profile '%s'" % profile_name)

    if 'remote_path' not in profile and args.remote_path is None:
        # Automatically figure out the remote path based on directory substitutions specified in the
        # profile. E.g. one could specify
        # "code_directory_substitutions": [{
        #   "local": "~/code",
        #   "remote": "/home/centos/code"
        # }]
        # and then run remote_build.py in ~/code/yugabyte-db and the build will run in
        # /home/centos/code/yugabyte-db on the remote host.
        cur_dir = os.getcwd()
        cur_dir_variants = [os.path.abspath(cur_dir), os.path.realpath(cur_dir)]
        for substitution in profile.get('code_directory_substitutions', []):
            local_code_dir = os.path.expanduser(substitution['local'])
            remote_code_dir = os.path.expanduser(substitution['remote'])

            for cur_dir_variant in cur_dir_variants:
                for local_code_dir_variant in [
                        os.path.abspath(local_code_dir), os.path.realpath(local_code_dir)]:
                    if cur_dir_variant == local_code_dir_variant:
                        args.remote_path = remote_code_dir
                    elif cur_dir_variant.startswith(local_code_dir_variant + '/'):
                        args.remote_path = os.path.join(
                            remote_code_dir,
                            os.path.relpath(cur_dir_variant, local_code_dir_variant))

                if args.remote_path is not None:
                    break

            if args.remote_path is not None:
                logging.info(
                    "Auto-detected remote path as %s based on current directory %s using the "
                    "substitution %s in the profile",
                    args.remote_path, cur_dir, json.dumps(substitution))
                break

    for arg_name in ['host', 'remote_path', 'branch']:
        if getattr(args, arg_name) is None:
            setattr(args, arg_name, profile.get(arg_name))
    if args.build_args is None:
        args.build_args = []
    args.build_args += profile.get('extra_args', [])


def sync_changes(host, branch, remote_path, wait_for_ssh):
    """
    Push local changes to a remote server.

    :param host: remote host
    :param branch: branch used as a base for local changes
    :param remote_path: path to yugabyte directory on a remote server
    :param wait_for_ssh: whether script should wait for host to become accessible via SSH
    :return: escaped remote path made absolute
    """
    commit = check_output_line(['git', 'merge-base', branch, 'HEAD'])
    print("Base commit: {0}".format(commit))

    if wait_for_ssh:
        while not remote_communicate(host, 'true', error_ok=True):
            print("Remote host is unavailabe, re-trying")
            time.sleep(1)

    can_clone_and_retry = True
    remote_commit = None
    while True:
        try:
            remote_commit = fetch_remote_commit(host, remote_path)
            break
        except subprocess.CalledProcessError as called_process_error:
            if not can_clone_and_retry:
                raise called_process_error
            logging.exception(called_process_error)

        can_clone_and_retry = False
        logging.info("Trying to clone the remote repository at %s", remote_path)
        remote_communicate(
            host,
            """
                repo_dir={0};
                echo "Attempting to clone the code on $(hostname) at $repo_dir"
                if [[ ! -e $repo_dir ]]; then
                    ( set -x; git clone git@github.com:yugabyte/yugabyte-db.git "$repo_dir" )
                fi
            """.format(shlex.quote(remote_path)).strip())

    if remote_path.startswith('~/'):
        escaped_remote_path = '$HOME/' + shlex.quote(remote_path[2:])
    else:
        escaped_remote_path = shlex.quote(remote_path)

    if remote_commit != commit:
        print("Remote commit mismatch, syncing")
        remote_command = ' && '.join([
            'cd {0}'.format(escaped_remote_path),
            'git checkout -- .',
            'git clean -f .',
            'git checkout master',
            'git pull',
            'git checkout {0}'.format(commit)
        ])
        remote_communicate(host, remote_command)
        remote_commit = fetch_remote_commit(host, remote_path)
        if remote_commit != commit:
            raise RuntimeError("Failed to sync remote commit to: {0}, it is still: {1}".format(
                commit, remote_commit))

    files, del_files = parse_git_diff_name_status(
        check_output_lines(['git', 'diff', commit, '--name-status']))
    print("Total files: {0}, deleted files: {1}".format(len(files), len(del_files)))

    if files:
        # From this StackOverflow thread: https://goo.gl/xzhBUC
        # The -a option is equivalent to -rlptgoD. You need to remove the -t. -t tells rsync to
        # transfer modification times along with the files and update them on the remote system.
        #
        # Another relevant one -- how to make rsync preserve timestamps of unchanged files:
        # https://goo.gl/czD96F
        #
        # We are using "rlpcgoD" instead of "rlptgoD" (with "t" replaced with "c").
        # The goal is to use checksums for deciding what files to skip.
        rsync_args = ['rsync', '-rlpcgoDvR']
        rsync_args += files
        rsync_args += ['--exclude', '.git']
        rsync_args += ["{0}:{1}".format(host, remote_path)]
        proc = subprocess.Popen(rsync_args, shell=False)
        proc.communicate()
        if proc.returncode != 0:
            raise RuntimeError('rsync terminated with code {}'.format(proc.returncode))

    if del_files:
        remote_command = 'cd {0} && rm -f '.format(escaped_remote_path)
        for file in del_files:
            remote_command += shlex.quote(file)
            remote_command += ' '
        remote_communicate(host, remote_command)

    check_remote_files(escaped_remote_path, host, remote_path, files)

    return escaped_remote_path


def exec_command(host, escaped_remote_path, script_name, script_args, do_quote_args):
    remote_command = "cd {0} && ./{1}".format(escaped_remote_path, script_name)
    for arg in script_args:
        remote_command += " {0}".format(shlex.quote(arg) if do_quote_args else arg)
    print("Remote command: {0}".format(remote_command))
    # Let's not use subprocess if the output is potentially large:
    # https://thraxil.org/users/anders/posts/2008/03/13/Subprocess-Hanging-PIPE-is-your-enemy/
    ssh_path = subprocess.check_output(['which', 'ssh']).strip()
    ssh_args = [ssh_path, host, remote_command]
    os.execv(ssh_path, ssh_args)
