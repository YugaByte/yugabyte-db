#!/usr/bin/env python
#
# Copyright 2019 YugaByte, Inc. and Contributors
#
# Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
# may not use this file except in compliance with the License. You
# may obtain a copy of the License at
#
# https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt

from ybops.common.exceptions import YBOpsRuntimeError


class AbstractCommandParser(object):
    """Generic class for wrapping around command line parsing and subcommand formation.

    This provides the mechanics to register subcommands, as well as add top-level options for
    the main command.
    """
    def __init__(self, name):
        self.name = name
        self.parser = None
        self.subparsers = None
        self.subcommands = []
        self.methods = []

    def register(self, parser):
        """Register a certain instance of an ArgParser with this class.

        Args:
            parser: The ArgParser to attach subcommands to.
        """
        self.parser = parser
        self.subparsers = self.parser.add_subparsers()

        self.add_subcommands()
        for c in self.subcommands:
            subparser = self.subparsers.add_parser(c.name)
            c.register(subparser)

        self.add_methods()
        for m in self.methods:
            m.prepare()

        self.add_extra_args()

    def add_extra_args(self):
        """Override this in subclasses to add command-level arguments or flags.
        """
        pass

    def add_subcommands(self):
        """Override this in subclasses to add extra subcommands.
        """
        pass

    def add_subcommand(self, command):
        """Convenience method to register an explicit subcommand.
        """
        self.subcommands.append(command)

    def add_methods(self):
        pass

    def add_method(self, method):
        self.methods.append(method)

    def unimplemented(self, message=""):
        """Convenience method to raise an error on unimplemented codepaths.
        """
        raise YBOpsRuntimeError("Unimplemented method. Details: {}".format(message))


class AbstractPerCloudCommand(AbstractCommandParser):
    """Class that encapsulates a lower layer of subcommands, that can be called on various clouds.
    """
    def __init__(self, name):
        super(AbstractPerCloudCommand, self).__init__(name)
        self.cloud = None

    def add_subcommand(self, command):
        """Subclass override to set a reference to the cloud into the subcommands we add.
        """
        command.cloud = self.cloud
        super(AbstractPerCloudCommand, self).add_subcommand(command)
