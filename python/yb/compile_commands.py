# Copyright (c) Yugabyte, Inc.

import os
import logging
import json
import re


from yb.common_util import (
    YB_SRC_ROOT,
    read_json_file,
    write_json_file,
    get_absolute_path_aliases,
)


# We build PostgreSQL code in a separate directory (postgres_build) rsynced from the source tree to
# support out-of-source builds. Then, after generating the compilation commands, we rewrite them
# to work with original files (in src/postgres) so that clangd can use them.

# The "combined compilation commands" file contains all compilation commands for C++ code and
# PostgreSQL C code. This is the file that is symlinked in the YugabyteDB source root directory and
# used with Clangd. These commands are post-processed.
COMBINED_POSTPROCESSED_COMPILE_COMMANDS_FILE_NAME = 'combined_compile_commands.json'

# The same as above but without postprocessing, just concatenated from C++ and Postgres C
# compile_commands.json files.
COMBINED_RAW_COMPILE_COMMANDS_FILE_NAME = 'combined_raw_compile_commands.json'


def create_compile_commands_symlink(combined_compile_commands_path, build_type):
    dest_link_path = os.path.join(YB_SRC_ROOT, 'compile_commands.json')
    if build_type != 'compilecmds':
        logging.info("Not creating a symlink at %s for build type %s",
                     dest_link_path, build_type)
        return

    if (not os.path.exists(dest_link_path) or
            os.path.realpath(dest_link_path) != os.path.realpath(combined_compile_commands_path)):
        if os.path.exists(dest_link_path):
            logging.info("Removing the old file/link at %s", dest_link_path)
            os.remove(dest_link_path)

        os.symlink(
            os.path.relpath(
                os.path.realpath(combined_compile_commands_path),
                os.path.realpath(YB_SRC_ROOT)),
            dest_link_path)
        logging.info("Created symlink at %s", dest_link_path)


def get_include_path_arg(include_path):
    return '-I%s' % include_path


class IncludePathRewriter:
    """
    Rewrites include paths in a compilation command line so that they refer to sources in
    src/postgres, not to rsynced sources in build/.../postgres_build.
    """
    def __init__(
            self,
            original_working_directory,
            new_working_directory,
            build_root):
        self.original_working_directory = original_working_directory
        self.new_working_directory = new_working_directory
        self.already_added_include_paths = set()
        self.original_include_paths = []
        self.additional_postgres_include_paths = []
        self.build_root = build_root
        self.postgres_install_dir_include_realpath = os.path.realpath(
            os.path.join(self.build_root, 'postgres', 'include'))
        self.new_args = []

    def remember_original_include_path(self, original_include_path):
        if (original_include_path not in self.already_added_include_paths and
                original_include_path not in self.original_include_paths):
            self.original_include_paths.append(original_include_path)

    def check_for_postgres_installed_include_path(self, include_path):
        assert os.path.isabs(include_path)
        if (not self.additional_postgres_include_paths and
                os.path.realpath(include_path) == self.postgres_install_dir_include_realpath):
            # If we encounter the include directory build/.../postgres/include, we add these
            # additional include directories at the end of the command line. This will help with
            # finding include files even when the PostgreSQL build is not fully done.
            self.additional_postgres_include_paths.extend([
                os.path.join(YB_SRC_ROOT, 'src', 'postgres', 'src', 'interfaces', 'libpq'),
                os.path.join(
                    self.build_root, 'postgres_build', 'src', 'include'),
                os.path.join(
                    self.build_root, 'postgres_build', 'interfaces', 'libpq')
            ])

    def append_include_path_arg(self, include_path):
        self.new_args.append(get_include_path_arg(include_path))

    def append_new_absolute_include_path(self, new_absolute_include_path):
        assert os.path.isabs(new_absolute_include_path)
        self.append_include_path_arg(new_absolute_include_path)

        # Prevent adding multiple aliases for the same include path.
        self.already_added_include_paths.add(new_absolute_include_path)
        self.already_added_include_paths.add(os.path.abspath(new_absolute_include_path))
        self.already_added_include_paths.add(os.path.realpath(new_absolute_include_path))

    def handle_include_path(self, include_path):
        if os.path.isabs(include_path):
            # This is already an absolute path, append it as is.
            self.check_for_postgres_installed_include_path(include_path)
            self.append_new_absolute_include_path(include_path)
            return

        original_include_path = os.path.realpath(
            os.path.join(self.original_working_directory, include_path))

        # This is a relative path. Rewrite it relative to the new directory where we pretend to
        # be running the compiler (i.e. where Clangd will run the compiler).
        self.remember_original_include_path(original_include_path)
        self.check_for_postgres_installed_include_path(original_include_path)

        new_include_path = os.path.realpath(os.path.join(self.new_working_directory, include_path))
        self.append_new_absolute_include_path(new_include_path)

    def rewrite(self, args):
        for arg in args:
            if not arg.startswith('-I'):
                self.new_args.append(arg)
                continue
            include_path = arg[2:]
            self.handle_include_path(include_path)
        self.new_args.extend(
            [get_include_path_arg(include_path)
             for include_path in
             self.additional_postgres_include_paths + self.original_include_paths])
        return self.new_args


class CompileCommandProcessor:
    def __init__(self, build_root):
        self.build_root = build_root

        self.pg_build_root = os.path.join(build_root, 'postgres_build')
        self.pg_build_root_aliases = get_absolute_path_aliases(self.pg_build_root)

        self.postgres_src_root = os.path.join(YB_SRC_ROOT, 'src', 'postgres')

    def postprocess_compile_command(self, compile_command_item):
        original_working_directory = compile_command_item['directory']
        if 'command' in compile_command_item:
            if 'arguments' in compile_command_item:
                raise ValueError(
                    "Invalid compile command item: %s (both 'command' and 'arguments' are "
                    "present)" % json.dumps(compile_command_item))
            arguments = compile_command_item['command'].split()
        elif 'arguments' in compile_command_item:
            arguments = compile_command_item['arguments']
        else:
            raise ValueError(
                "Invalid compile command item: %s (neither 'command' nor 'arguments' are present)" %
                json.dumps(compile_command_item))
        file_path = compile_command_item['file']

        new_working_directory = original_working_directory
        for pg_build_root_alias in self.pg_build_root_aliases:
            # Some files only exist in the postgres build directory. We don't switch the work
            # directory of the compiler to the original source directory in those cases.
            if original_working_directory.startswith(pg_build_root_alias + '/'):
                corresponding_src_dir = os.path.join(
                    self.postgres_src_root,
                    os.path.relpath(original_working_directory, pg_build_root_alias))
                if (os.path.isabs(file_path) or
                        os.path.isfile(os.path.join(corresponding_src_dir, file_path))):
                    new_working_directory = corresponding_src_dir
                    break

        if new_working_directory == original_working_directory:
            new_args = arguments
        else:
            new_args = IncludePathRewriter(
                original_working_directory,
                new_working_directory,
                self.build_root,
            ).rewrite(arguments)

        # Replace the compiler path in compile_commands.json according to user preferences.
        compiler_basename = os.path.basename(new_args[0])
        new_compiler_path = None
        if compiler_basename in ['cc', 'gcc', 'clang']:
            new_compiler_path = os.environ.get('YB_CC_FOR_COMPILE_COMMANDS')
        elif compiler_basename in ['c++', 'g++', 'clang++']:
            new_compiler_path = os.environ.get('YB_CXX_FOR_COMPILE_COMMANDS')
        else:
            logging.warning(
                "Unexpected compiler path: %s. Compile command: %s",
                new_args[0], json.dumps(compile_command_item))
        if new_compiler_path:
            new_args[0] = new_compiler_path

        new_file_path = file_path
        if not os.path.isabs(file_path):
            new_file_path = os.path.join(new_working_directory, file_path)
            if not os.path.isfile(new_file_path):
                new_file_path = file_path

        return {
            'directory': new_working_directory,
            'file': new_file_path,
            'arguments': new_args
        }


def filter_compile_commands(input_path, output_path, file_name_regex_str):
    compiled_re = re.compile(file_name_regex_str)
    input_cmds = read_json_file(input_path)
    output_cmds = [
        item for item in input_cmds
        if compiled_re.match(os.path.basename(item['file']))
    ]
    logging.info(
        "Filtered compilation commands from %d to %d entries using the regex %s",
        len(input_cmds), len(output_cmds), file_name_regex_str)
    write_json_file(
        output_cmds, output_path,
        description_for_log="filtered compilation commands file")
