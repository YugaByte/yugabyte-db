#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

import glob
import logging
import os
import subprocess
import tarfile
import json

import ybops.utils as ybutils
from ybops.common.exceptions import YBOpsRuntimeError


class ReleaseManager(object):
    """ReleaseManager class is used to package yugabyte specific builds.
    """

    RELEASE_MANIFEST_NAME = "yb_release_manifest.json"

    def __init__(self, options):
        """Initializer method takes an options parameter, which has
        repository path and name of the release.
        Args:
            options (dict): dictionary of options with repository path and
                            release name
        """
        assert options["repository"] is not None, 'repository is required option'
        assert options["name"] is not None, 'name is required option'

        self.repository = options["repository"]
        self.release_name = options["name"]
        self.build_type = options.get("type", None)
        self.release_manifest = self.fetch_release_manifest()
        self.force_yes = options.get("force_yes", False)

    def fetch_release_manifest(self):
        """This method fetches the manifest json, from the repository file path

        Returns:
            (json): returns the manifest json
        """
        release_manifest_file = os.path.join(self.repository, self.RELEASE_MANIFEST_NAME)

        with open(release_manifest_file) as f:
            release_manifest = json.load(f)

        assert release_manifest is not None, 'Unable to read {0} file'.format(release_manifest_file)

        return release_manifest

    def check_for_uncommitted_changes(self):
        """This method checks if the git repository isn't dirty.
        """
        local_changes = subprocess.check_output(
            ["git", "diff", "origin/master"]).decode("utf-8").strip()

        if not local_changes:
            return

        ybutils.log_message(logging.ERROR,
                            "Repository {} appears to have uncommitted changes. "
                            "The source release will not include your local changes."
                            .format(self.repository))
        if not self.force_yes:
            ybutils.confirm_prompt("Continue?")

    def check_for_local_commits(self):
        """This method checks if there is local commits which haven't been pushed upstream.
        """
        subprocess.call(["git", "fetch", "origin"])
        local_commits = subprocess.check_output(
            ["git", "log", "origin/master..HEAD", "--oneline"]).decode("utf-8")
        if not local_commits:
            return

        ybutils.log_message(logging.ERROR,
                            "Repository {} appears to have local commits:\n {} "
                            "This should not be an official release!"
                            .format(self.repository, local_commits))

        if not self.force_yes:
            ybutils.confirm_prompt("Continue?")

    def create_tarball(self):
        """This method creates a tar file based on the release manifest.
        Returns:
            (str): returns the tar file
        """
        tarball_path = ybutils.get_release_file(self.repository,
                                                self.release_name,
                                                self.build_type)
        ybutils.log_message(logging.INFO, "Exporting release tarball")

        # TODO(bogdan,ram): hack around yugabyte untaring into a top-level dir named by repo.
        # This should only be the repo name passed in from the respective yb_release.py in either
        # yugabyte or devops.
        prefix_dir = ""
        if self.release_name == "yugabyte":
            prefix_dir = "{}-{}".format(
                self.release_name, ybutils.get_default_release_version(self.repository))

        with tarfile.open(tarball_path, "w:gz") as tar:
            for folder_key in self.release_manifest:
                if folder_key.startswith("/"):
                    raise YBOpsRuntimeError("Manifest file keys should not start with /")
                for file_pattern in self.release_manifest[folder_key]:
                    # If the file pattern starts with /, we assume that the pattern includes
                    # absolute path, if not it is relative to the repository path.
                    if not file_pattern.startswith("/"):
                        file_pattern = os.path.join(self.repository, file_pattern)

                    for file_path in glob.glob(file_pattern):
                        path_within_tarball = os.path.join(
                            prefix_dir, folder_key, os.path.basename(file_path))
                        tar.add(file_path, arcname=path_within_tarball)

        ybutils.log_message(logging.INFO, "Generated tarball: {}".format(tarball_path))
        return tarball_path

    def generate_release(self):
        """This method check if we have local uncommitted changes, or local commits
        and confirms with the user, if accepted, we proceed to create a tar based on the
        release manifest and write the checksum files as well.

        Returns:
            (str): returns the generated tar file name
        """
        tar_file = None
        current_dir = os.getcwd()
        try:
            os.chdir(self.repository)
            self.check_for_uncommitted_changes()
            self.check_for_local_commits()
            tar_file = self.create_tarball()
            ybutils.log_message(logging.INFO, "Release generation succeeded!")
        finally:
            os.chdir(current_dir)

        return tar_file
