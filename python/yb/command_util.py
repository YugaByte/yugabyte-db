#
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
Copyright (c) YugaByte, Inc.

This module provides utilities for running commands.
"""

import os
import platform
import shutil
import subprocess
import logging
import shlex

from collections import namedtuple


class ProgramResult:
    def __init__(
            self,
            cmd_line,
            returncode,
            stdout,
            stderr,
            error_msg,
            program_path):
        self.cmd_line = cmd_line
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.error_msg = error_msg
        self.program_path = program_path


def trim_output(output, max_lines):
    if isinstance(output, bytes):
        output = output.decode('utf-8')
    lines = output.split("\n")
    if len(lines) <= max_lines:
        return output
    return "\n".join(lines[:max_lines] + ['({} lines skipped)'.format(len(lines) - max_lines)])


def run_program(args, error_ok=False, max_error_lines=100, cwd=None, log_command=False):
    """
    Run the given program identified by its argument list, and return a ProgramResult object.

    @param error_ok False to raise an exception on errors.
    """
    if not isinstance(args, list):
        args = [args]
    if log_command:
        logging.info("Running command: %s", args)
    try:
        program_subprocess = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd)
    except OSError:
        logging.error("Failed to run program {}".format(args))
        raise

    program_stdout, program_stderr = program_subprocess.communicate()
    error_msg = None
    if program_subprocess.returncode != 0:
        from pipes import quote
        error_msg = "Non-zero exit code {} from: {} ; stdout: '{}' stderr: '{}'".format(
                program_subprocess.returncode, ' '.join([quote(arg) for arg in args]),
                trim_output(program_stdout.strip(), max_error_lines),
                trim_output(program_stderr.strip(), max_error_lines))
        if not error_ok:
            raise RuntimeError(error_msg)
    return ProgramResult(
        cmd_line=args,
        returncode=program_subprocess.returncode,
        stdout=program_stdout.strip().decode('utf-8'),
        stderr=program_stderr.strip().decode('utf-8'),
        error_msg=error_msg,
        program_path=os.path.realpath(args[0])
    )


def mkdir_p(d):
    """
    Similar to the "mkdir -p ..." shell command. Creates the given directory and all enclosing
    directories. No-op if the directory already exists.
    """
    if os.path.isdir(d):
        return
    try:
        os.makedirs(d)
    except OSError as e:
        if os.path.isdir(d):
            return
        raise e


def copy_deep(src, dst, create_dst_dir=False):
    """
    Does recursive copy of src path to dst path. Copies symlinks as symlinks. Doesn't overwrite
    existing files and symlinks (even if they are broken) in Linux.
    In Darwin, it overwrite the files if the source mtime is newer than the destination mtime.
    """
    system_is_darwin = platform.system().lower() == "darwin"
    if create_dst_dir:
        mkdir_p(os.path.dirname(dst))
    src_is_link = os.path.islink(src)
    dst_exists = os.path.lexists(dst)
    if os.path.isdir(src) and not src_is_link:
        logging.debug("Copying directory {} to {}".format(src, dst))
        mkdir_p(dst)
        for name in os.listdir(src):
            copy_deep(os.path.join(src, name), os.path.join(dst, name))
    elif src_is_link:
        if dst_exists:
            return
        target = os.readlink(src)
        logging.debug("Creating symlink {} -> {}".format(dst, target))
        os.symlink(target, dst)
    else:
        if dst_exists:
            if not system_is_darwin:
                return
            # Only overwrite the file if the source is newer than the destination.
            if os.path.getmtime(src) <= os.path.getmtime(dst):
                return
        logging.debug("Copying file {} to {}".format(src, dst))
        # Preserve the file attributes.
        shutil.copy2(src, dst)
