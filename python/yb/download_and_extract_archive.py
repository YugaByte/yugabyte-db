#!/usr/bin/env python

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

"""
Downloads and extracts an archive with pre-built third-party dependencies.
"""

# This script should not use any non-standard modules and should run with Python 2 and Python 3.
# It could be run before the main Python interpreter we'll be using for most of our scripts is
# even installed.

import os
import sys
import re
import logging
import socket
import random
import atexit
import subprocess
import argparse
import tempfile
import time
import getpass
import platform
import fcntl
import errno


g_verbose = False
EXPECTED_ARCHIVE_EXTENSION = '.tar.gz'
CHECKSUM_EXTENSION = '.sha256'


def remove_ignore_errors(file_path):
    file_path = os.path.abspath(file_path)
    if os.path.isfile(file_path):
        try:
            os.remove(file_path)
        except Exception as e:
            logging.warning("Error removing %s: %s, ignoring", file_path, e)


def run_cmd(args):
    if g_verbose:
        logging.info("Running command: %s", args)
    try:
        subprocess.check_call(args)
    except:  # noqa
        logging.error("Error trying to run command: %s", args)
        raise


def validate_sha256sum(checksum_str):
    if not re.match(r'^[0-9a-f]{64}$', checksum_str):
        raise ValueError("Invalid SHA256 checksum: '%s', expected 64 hex characters", checksum_str)


def read_file_and_strip(file_path):
    with open(file_path) as f:
        return f.read().strip()


def compute_sha256sum(file_path):
    cmd_line = None
    if sys.platform.startswith('linux'):
        cmd_line = ['sha256sum', file_path]
    elif sys.platform.startswith('darwin'):
        cmd_line = ['shasum', '--algorithm', '256', file_path]
    else:
        raise ValueError("Don't know how to compute SHA256 checksum on platform %s" % sys.platform)

    checksum_str = subprocess.check_output(cmd_line).strip().split()[0].decode('utf-8')
    validate_sha256sum(checksum_str)
    return checksum_str


def verify_sha256sum(checksum_file_path, data_file_path):
    if not os.path.exists(checksum_file_path):
        raise IOError("Checksum file does not exist: %s" % checksum_file_path)

    if not os.path.exists(data_file_path):
        raise IOError("Data file does not exist: %s", data_file_path)

    if not checksum_file_path.endswith(CHECKSUM_EXTENSION):
        raise ValueError("Checksum file path must end with '%s', got: %s" % (
            CHECKSUM_EXTENSION, checksum_file_path))

    # Guard against someone passing in the actual data file instead of the checksum file.
    checksum_file_size = os.stat(checksum_file_path).st_size
    if checksum_file_size > 4096:
        raise IOError("Checksum file size is too big: %d bytes (file path: %s)" % (
            checksum_file_size, checksum_file_path))

    expected_checksum = read_file_and_strip(checksum_file_path).split()[0]

    actual_checksum = compute_sha256sum(data_file_path)
    if actual_checksum == expected_checksum:
        return True

    err_msg = "Invalid checksum for file %s: got %s, expected %s" % (
        data_file_path, actual_checksum, expected_checksum)
    logging.warning(err_msg)
    return False


def download_url(url, dest_path):
    start_time_sec = time.time()
    logging.info("Downloading %s to %s", url, dest_path)
    dest_dir = os.path.dirname(dest_path)
    if not os.path.isdir(dest_dir):
        raise IOError("Destination directory %s does not exist" % dest_dir)
    run_cmd(['curl', '-LsS', url, '-o', dest_path])
    if not os.path.exists(dest_path):
        raise IOError("Failed to download %s: file %s does not exist" % (url, dest_path))
    elapsed_sec = time.time() - start_time_sec
    logging.info("Downloaded %s to %s in %.1fs" % (url, dest_path, elapsed_sec))


def move_file(src_path, dest_path):
    if g_verbose:
        logging.info("Trying to move file %s to %s", src_path, dest_path)
    if not os.path.exists(src_path):
        raise IOError("Does not exist: %s" % src_path)
    if not os.path.isfile(src_path):
        raise IOError("Not a file: %s" % src_path)
    if os.path.isdir(dest_path):
        raise IOError("Destination path can't be a directory: %s" % dest_path)
    if os.path.exists(dest_path):
        logging.warning("Destination path already exists: %s, moving %s there anyway" % (
            dest_path, src_path))
    dest_parent_dir = os.path.dirname(dest_path)
    if not os.path.isdir(dest_parent_dir):
        raise IOError("Destination directory %s does not exist" % dest_parent_dir)
    os.rename(src_path, dest_path)


def check_dir_exists_and_is_writable(dir_path, description):
    if not os.path.isdir(dir_path):
        raise IOError("%s directory %s does not exist" % (description, dir_path))
    if not os.access(dir_path, os.W_OK):
        raise IOError("%s directory %s is not writable by current user (%s)" % (
            description, dir_path, getpass.getuser()))


# From https://github.com/ianlini/mkdir-p/blob/master/mkdir_p/mkdir_p.py
def mkdir_p(path, mode=0o777):
    try:
        os.makedirs(path, mode=mode)
    except OSError as exc:
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else:
            raise


def exists_or_is_link(dest):
    """
    A file could be a link to a non-existent directory, or to a directory owned by a different
    user in a directory with sticky bit set. In such cases os.path.exists might return false, but
    islink will return true.
    """
    return os.path.exists(dest) or os.path.islink(dest)


def download_and_extract(url, dest_dir_parent, local_cache_dir, nfs_cache_dir):
    tar_gz_name = os.path.basename(url)
    checksum_file_name = tar_gz_name + CHECKSUM_EXTENSION
    install_dir_name = tar_gz_name[:-len(EXPECTED_ARCHIVE_EXTENSION)]
    dest_dir = os.path.join(dest_dir_parent, install_dir_name)

    if os.path.isdir(dest_dir):
        logging.info("Directory %s already exists, no need to install." % dest_dir)
        return

    if not os.path.isdir(local_cache_dir):
        logging.info("Directory %s does not exist, trying to create", local_cache_dir)
        try:
            mkdir_p(local_cache_dir)
        except Exception as ex:
            logging.info("Failed creating directory '%s': %s", local_cache_dir, ex)

    check_dir_exists_and_is_writable(local_cache_dir, "Local cache")

    if not url.endswith(EXPECTED_ARCHIVE_EXTENSION):
        raise ValueError("Archive download URL is expected to end with %s, got: %s" % (
            url, EXPECTED_ARCHIVE_EXTENSION))

    if os.path.isdir(dest_dir):
        logging.info("Directory %s already exists, someone must have created it concurrently.",
                     dest_dir)
        return

    start_time_sec = time.time()
    logging.info("Installing %s into directory %s", url, dest_dir)
    tmp_dir_prefix = os.path.abspath(os.path.join(dest_dir_parent, install_dir_name + '.tmp.'))
    mkdir_p(dest_dir_parent)
    tmp_dir = tempfile.mkdtemp(prefix=tmp_dir_prefix)

    def cleanup():
        if os.path.isdir(tmp_dir):
            run_cmd(['rm', '-rf', tmp_dir])

    atexit.register(cleanup)
    for cache_dir in [local_cache_dir, nfs_cache_dir]:
        cached_tar_gz_path = os.path.join(cache_dir, tar_gz_name)
        cached_checksum_path = cached_tar_gz_path + CHECKSUM_EXTENSION
        tar_gz_path = None
        if os.path.exists(cached_tar_gz_path) and os.path.exists(cached_checksum_path):
            logging.info("Verifying the checksum of %s", cached_tar_gz_path)
            if verify_sha256sum(cached_checksum_path, cached_tar_gz_path):
                tar_gz_path = os.path.join(cache_dir, tar_gz_name)
                break
            else:
                remove_ignore_errors(cached_tar_gz_path)
                remove_ignore_errors(cached_checksum_path)

    if tar_gz_path is None:
        tmp_tar_gz_path = os.path.join(tmp_dir, tar_gz_name)
        tmp_checksum_path = os.path.join(tmp_dir, checksum_file_name)
        download_url(url + CHECKSUM_EXTENSION, tmp_checksum_path)
        download_url(url, tmp_tar_gz_path)
        if not verify_sha256sum(tmp_checksum_path, tmp_tar_gz_path):
            raise ValueError("Checksum verification failed for the download of %s" % url)
        file_names = [tar_gz_name, checksum_file_name]
        for file_name in file_names:
            move_file(os.path.join(tmp_dir, file_name),
                      os.path.join(local_cache_dir, file_name))

        tar_gz_path = os.path.join(local_cache_dir, tar_gz_name)

        nfs_tar_gz_path = os.path.join(nfs_cache_dir, tar_gz_name)
        nfs_checksum_file_path = os.path.join(nfs_cache_dir, checksum_file_name)
        if (os.path.isdir(nfs_cache_dir) and 
            os.access(nfs_cache_dir, os.W_OK) and
            (not os.path.exists(nfs_tar_gz_path) or
             not os.path.exists(nfs_checksum_file_path))):
            for file_name in file_names:
                run_cmd(['cp',
                        os.path.join(local_cache_dir, file_name),
                        os.path.join(nfs_cache_dir, file_name)])

    logging.info("Extracting %s in %s", tar_gz_path, tmp_dir)
    run_cmd(['tar', 'xf', tar_gz_path, '-C', tmp_dir])
    tmp_extracted_dir = os.path.join(tmp_dir, install_dir_name)
    if not os.path.exists(tmp_extracted_dir):
        raise IOError(
            "Extracted '%s' in '%s' but a directory named '%s' did not appear" % (
                tar_gz_path, os.getcwd(), tmp_extracted_dir))
    if exists_or_is_link(dest_dir):
        logging.info("Looks like %s was created concurrently", dest_dir)
        return
    if install_dir_name.startswith('linuxbrew'):
        orig_brew_home_file = os.path.join(tmp_extracted_dir, 'ORIG_BREW_HOME')
        if not os.path.exists(orig_brew_home_file):
            raise IOError("File '%s' not found after extracting '%s'" % (
                orig_brew_home_file, tar_gz_name))
        orig_brew_home = read_file_and_strip(orig_brew_home_file)
        if not orig_brew_home.startswith(dest_dir):
            raise ValueError(
                "Original Homebrew/Linuxbrew install home directory is '%s'"
                " but we are trying to install it in '%s', and that is not a prefix of"
                " the former." % (orig_brew_home, dest_dir))

        already_installed_msg = (
            "'%s' already exists, cannot move '%s' to it. Someone else must have "
            "installed it concurrently. This is OK." % (
                orig_brew_home, dest_dir))

        def create_brew_symlink_if_needed():
            brew_link_src = os.path.basename(orig_brew_home)
            # dest_dir will now be a symlink pointing to brew_link_src. We are NOT creating a
            # symlink inside dest_dir.
            if not exists_or_is_link(dest_dir):
                logging.info("Creating a symlink '%s' -> '%s'", dest_dir, brew_link_src)
                try:
                    os.symlink(brew_link_src, dest_dir)
                except OSError as os_error:
                    if os_error.errno == errno.EEXIST:
                        if exists_or_is_link(dest_dir):
                            logging.info(
                                "Symlink '%s' was created concurrently. This is probably OK.",
                                dest_dir)
                        else:
                            err_msg = (
                                "Failed creating symlink '%s' -> '%s' with error: %s, but the "
                                "symlink does not actually exist!" % (
                                    dest_dir, brew_link_src, os_error))
                            logging.error(err_msg)
                            raise IOError(err_msg)
                    else:
                        logging.error("Unexpected error when creating symlink '%s' -> '%s': %s",
                                      dest_dir, brew_link_src, os_error)
                        raise os_error

            assert exists_or_is_link(dest_dir)
            if not os.path.islink(dest_dir):
                # A defensive sanity check.
                err_msg = "%s exists but is not a symbolic link" % dest_dir
                logging.error(err_msg)
                raise IOError(err_msg)
            else:
                actual_link_src = os.readlink(dest_dir)
                if actual_link_src != brew_link_src:
                    err_msg = "Symlink %s is not pointing to %s but instead points to %s" % (
                        dest_dir, brew_link_src, actual_link_src)
                    logging.error(err_msg)
                    raise IOError(err_msg)

        if os.path.exists(orig_brew_home):
            logging.info(already_installed_msg)
            create_brew_symlink_if_needed()
            return
        logging.info("Moving '%s' to '%s'" % (tmp_extracted_dir, orig_brew_home))
        try:
            os.rename(tmp_extracted_dir, orig_brew_home)
        except IOError as io_error:
            # A defensive sanity check in case locking is not working properly.
            if io_error == errno.ENOTEMPTY:
                # For whatever reason, this is what we get when the destination directory
                # already exists.
                logging.info(already_installed_msg)
                create_brew_symlink_if_needed()
                return

        create_brew_symlink_if_needed()
    else:
        if g_verbose:
            logging.info("Moving %s to %s", tmp_extracted_dir, dest_dir)
        os.rename(tmp_extracted_dir, dest_dir)

    logging.info("Installation of %s took %.1f sec", dest_dir, time.time() - start_time_sec)


def main():
    # Created files/directories should be writable by the group.
    os.umask(2)

    logging.basicConfig(
        level=logging.INFO,
        format="%(filename)s:%(lineno)d " + socket.gethostname() + " pid " + str(os.getpid()) +
               " %(asctime)s %(levelname)s: %(message)s")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--url', help='URL to download. Must end with .tar.gz.', required=True)
    parser.add_argument(
        '--dest-dir-parent', help='Parent directory in which to extract the archive',
        required=True)
    parser.add_argument(
        '--local-cache-dir',
        default='/opt/yb-build/download_cache',
        help='Download cache on the local disk')
    parser.add_argument(
        '--nfs-cache-dir',
        default='/n/jenkins/download_cache',
        help='Download cache on NFS')

    parser.add_argument('--verbose', action='store_true', help='Verbose logging')

    args = parser.parse_args()
    if args.verbose or os.getenv('YB_VERBOSE') == '1':
        global g_verbose
        g_verbose = True

    download_and_extract(
        url=args.url,
        dest_dir_parent=args.dest_dir_parent,
        local_cache_dir=args.local_cache_dir,
        nfs_cache_dir=args.nfs_cache_dir)


if __name__ == '__main__':
    main()
