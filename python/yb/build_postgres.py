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

"""
A wrapper script around PostgreSQL build that allows building debug and release versions in separate
directories.
"""

import os
import logging
import argparse
import re
import sys
import multiprocessing
import subprocess
import json
import hashlib
import time

from subprocess import check_call

from yugabyte_pycommon import (
    init_logging,
    run_program,
    WorkDirContext,
    mkdir_p,
    quote_for_bash,
    is_verbose_mode
)

from yb import common_util
from yb.tool_base import YbBuildToolBase
from yb.common_util import (
    YB_SRC_ROOT,
    get_build_type_from_build_root,
    get_bool_env_var,
    write_json_file,
    read_json_file,
    get_absolute_path_aliases,
    EnvVarContext,
)
from yb.compile_commands import (
    COMBINED_POSTPROCESSED_COMPILE_COMMANDS_FILE_NAME,
    COMBINED_RAW_COMPILE_COMMANDS_FILE_NAME,
    create_compile_commands_symlink,
    CompileCommandProcessor,
)
from overrides import overrides


ALLOW_REMOTE_COMPILATION = True
BUILD_STEPS = (
    'configure',
    'make',
)
CONFIG_ENV_VARS = [
    'CFLAGS',
    'CXXFLAGS',
    'LDFLAGS',
    'PATH',
    'YB_BUILD_ROOT',
    'YB_BUILD_TYPE',
    'YB_SRC_ROOT',
    'YB_THIRDPARTY_DIR'
]
REMOVE_CONFIG_CACHE_MSG_RE = re.compile(r'error: run.*\brm config[.]cache\b.*and start over')


def sha256(s):
    return hashlib.sha256(s).hexdigest()


def adjust_error_on_warning_flag(flag, step, language):
    """
    Adjust a given compiler flag according to the build step and language.
    """
    assert language in ('c', 'c++')
    assert step in BUILD_STEPS
    if language == 'c' and flag in ('-Wreorder', '-Wnon-virtual-dtor'):
        # Skip C++-only flags.
        return None

    if step == 'configure':
        if flag == '-Werror':
            # Skip this flag altogether during the configure step.
            return None

        if flag == '-fsanitize=thread':
            # Don't actually enable TSAN in the configure step, otherwise configure will think that
            # our pthread library is not working properly.
            # https://gist.githubusercontent.com/mbautin/366970ac55c9d3579816d5e8563e70b4/raw
            return None

    if step == 'make':
        # No changes.
        return flag

    if flag.startswith('-Werror='):
        # Convert e.g. -Werror=implicit-function-declaration to -Wimplicit-function-declaration.
        return '-W' + flag[8:]

    return flag


def filter_compiler_flags(compiler_flags, step, language):
    """
    This function optionaly removes flags that turn warnings into errors.
    """
    assert language in ('c', 'c++')
    assert step in BUILD_STEPS
    adjusted_flags = [
        adjust_error_on_warning_flag(flag, step, language)
        for flag in compiler_flags.split()
    ]
    return ' '.join([
        flag for flag in adjusted_flags if flag is not None
    ])


class PostgresBuilder(YbBuildToolBase):
    def __init__(self):
        super().__init__()
        self.build_root = None
        self.pg_build_root = None
        self.pg_prefix = None
        self.build_type = None
        self.postgres_src_dir = None
        self.compiler_type = None
        self.env_vars_for_build_stamp = set()

        # Check if the outer build is using runs the compiler on build workers.
        self.build_uses_remote_compilation = os.environ.get('YB_REMOTE_COMPILATION') == '1'
        if self.build_uses_remote_compilation == 'auto':
            raise RuntimeError(
                "No 'auto' value is allowed for YB_REMOTE_COMPILATION at this point")

    def get_yb_version(self):
        with open(os.path.join(YB_SRC_ROOT, 'version.txt'), "r") as version_file:
            return version_file.read().strip()

    def set_env_var(self, name, value):
        if value is None:
            if name in os.environ:
                del os.environ[name]
        else:
            os.environ[name] = value
        self.env_vars_for_build_stamp.add(name)

    def append_to_env_var(self, name, to_append, separator=' '):
        if name in os.environ and os.environ[name]:
            self.set_env_var(name, os.environ[name] + separator + to_append)
        else:
            self.set_env_var(name, to_append)

    @overrides
    def get_description(self):
        return __doc__

    @overrides
    def add_command_line_args(self):
        parser = self.arg_parser
        parser.add_argument('--cflags', help='C compiler flags')
        parser.add_argument('--clean',
                            action='store_true',
                            help='Clean PostgreSQL build and installation directories.')
        parser.add_argument('--cxxflags', help='C++ compiler flags')
        parser.add_argument('--ldflags', help='Linker flags for all binaries')
        parser.add_argument('--ldflags_ex', help='Linker flags for executables')
        parser.add_argument('--openssl_include_dir', help='OpenSSL include dir')
        parser.add_argument('--openssl_lib_dir', help='OpenSSL lib dir')
        parser.add_argument('--run_tests',
                            action='store_true',
                            help='Run PostgreSQL tests after building it.')
        parser.add_argument('--step',
                            choices=BUILD_STEPS,
                            help='Run a specific step of the build process')

    @overrides
    def validate_and_process_args(self):
        if not self.args.build_root:
            raise RuntimeError("Neither BUILD_ROOT or --build-root specified")

        self.build_root = os.path.abspath(self.args.build_root)
        self.build_root_realpath = os.path.realpath(self.build_root)
        self.build_type = get_build_type_from_build_root(self.build_root)
        self.pg_build_root = os.path.join(self.build_root, 'postgres_build')
        self.build_stamp_path = os.path.join(self.pg_build_root, 'build_stamp')
        self.pg_prefix = os.path.join(self.build_root, 'postgres')
        self.postgres_src_dir = os.path.join(YB_SRC_ROOT, 'src', 'postgres')
        self.compiler_type = self.args.compiler_type
        self.openssl_include_dir = self.args.openssl_include_dir
        self.openssl_lib_dir = self.args.openssl_lib_dir

        if not self.compiler_type:
            raise RuntimeError(
                "Compiler type not specified using either --compiler_type or YB_COMPILER_TYPE")

        self.export_compile_commands = os.environ.get('YB_EXPORT_COMPILE_COMMANDS') == '1'
        self.should_configure = self.args.step is None or self.args.step == 'configure'
        self.should_make = self.args.step is None or self.args.step == 'make'
        self.thirdparty_dir = self.args.thirdparty_dir
        self.original_path = os.getenv('PATH').split(':')

    def adjust_cflags_in_makefile(self):
        makefile_global_path = os.path.join(self.pg_build_root, 'src/Makefile.global')
        new_makefile_lines = []
        new_cflags = os.environ['CFLAGS'].strip()
        found_cflags = False
        with open(makefile_global_path) as makefile_global_input_f:
            for line in makefile_global_input_f:
                line = line.rstrip("\n")
                if line.startswith('CFLAGS = '):
                    existing_cflags = line[9:].strip()
                    found_cflags = True
                    if existing_cflags != new_cflags:
                        line = 'CFLAGS = $(YB_PREPEND_CFLAGS) ' + \
                            new_cflags + ' $(YB_APPEND_CFLAGS)'
                        replaced_cflags = True
                new_makefile_lines.append(line)

        if not found_cflags:
            raise RuntimeError("Could not find a CFLAGS line in %s" % makefile_global_path)

        if replaced_cflags:
            logging.info("Replaced cflags in %s", makefile_global_path)
            with open(makefile_global_path, 'w') as makefile_global_out_f:
                makefile_global_out_f.write("\n".join(new_makefile_lines) + "\n")

    def set_env_vars(self, step):
        if step not in BUILD_STEPS:
            raise RuntimeError(
                ("Invalid step specified for setting env vars: must be in {}")
                .format(BUILD_STEPS))
        is_make_step = step == 'make'

        self.set_env_var('YB_PG_BUILD_STEP', step)
        self.set_env_var('YB_BUILD_ROOT', self.build_root)
        self.set_env_var('YB_THIRDPARTY_DIR', self.thirdparty_dir)
        self.set_env_var('YB_SRC_ROOT', YB_SRC_ROOT)

        for var_name in ['CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LDFLAGS_EX']:
            arg_value = getattr(self.args, var_name.lower())
            self.set_env_var(var_name, arg_value)

        additional_c_cxx_flags = [
            '-Wimplicit-function-declaration',
            '-Wno-error=unused-function',
            '-DHAVE__BUILTIN_CONSTANT_P=1',
            '-DUSE_SSE42_CRC32C=1',
            '-std=c11',
            '-Werror=implicit-function-declaration',
            '-Werror=int-conversion',
        ]

        if self.compiler_type == 'clang':
            additional_c_cxx_flags += [
                '-Wno-builtin-requires-header'
            ]

        if is_make_step:
            additional_c_cxx_flags += [
                '-Wall',
                '-Werror',
                '-Wno-error=unused-function'
            ]

            if self.build_type == 'release':
                if self.compiler_type == 'clang':
                    additional_c_cxx_flags += [
                        '-Wno-error=array-bounds',
                        '-Wno-error=gnu-designator'
                    ]
                if self.compiler_type == 'gcc':
                    additional_c_cxx_flags += ['-Wno-error=strict-overflow']
            if self.build_type == 'asan':
                additional_c_cxx_flags += [
                    '-fsanitize-recover=signed-integer-overflow',
                    '-fsanitize-recover=shift-base',
                    '-fsanitize-recover=shift-exponent'
                ]

        # Tell gdb to pretend that we're compiling the code in the $YB_SRC_ROOT/src/postgres
        # directory.
        additional_c_cxx_flags += [
            '-fdebug-prefix-map=%s=%s' % (build_path, source_path)
            for build_path in get_absolute_path_aliases(self.pg_build_root)
            for source_path in get_absolute_path_aliases(self.postgres_src_dir)
        ]

        if self.compiler_type == 'gcc':
            additional_c_cxx_flags.append('-Wno-error=maybe-uninitialized')

        for var_name in ['CFLAGS', 'CXXFLAGS']:
            os.environ[var_name] = filter_compiler_flags(
                    os.environ.get(var_name, '') + ' ' + ' '.join(additional_c_cxx_flags),
                    step,
                    language='c' if var_name == 'CFLAGS' else 'c++')
        if step == 'make':
            self.adjust_cflags_in_makefile()

        for env_var_name in ['MAKEFLAGS']:
            if env_var_name in os.environ:
                del os.environ[env_var_name]

        compiler_wrappers_dir = os.path.join(YB_SRC_ROOT, 'build-support', 'compiler-wrappers')
        self.set_env_var('CC', os.path.join(compiler_wrappers_dir, 'cc'))
        self.set_env_var('CXX', os.path.join(compiler_wrappers_dir, 'c++'))

        self.set_env_var('LDFLAGS', re.sub(r'-Wl,--no-undefined', ' ', os.environ['LDFLAGS']))
        self.set_env_var(
            'LDFLAGS',
            re.sub(r'-Wl,--no-allow-shlib-undefined', ' ', os.environ['LDFLAGS']))

        self.append_to_env_var(
            'LDFLAGS',
            '-Wl,-rpath,' + os.path.join(self.build_root, 'lib'))

        for ldflags_var_name in ['LDFLAGS', 'LDFLAGS_EX']:
            self.append_to_env_var(ldflags_var_name, '-lm')

        if is_verbose_mode():
            # CPPFLAGS are C preprocessor flags, CXXFLAGS are C++ flags.
            for env_var_name in ['CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LDFLAGS_EX', 'LIBS']:
                if env_var_name in os.environ:
                    logging.info("%s: %s", env_var_name, os.environ[env_var_name])

        self.remote_compilation_allowed = ALLOW_REMOTE_COMPILATION and is_make_step

        self.set_env_var(
            'YB_REMOTE_COMPILATION',
            '1' if (self.remote_compilation_allowed and
                    self.build_uses_remote_compilation and
                    step == 'make') else '0'
        )

        self.set_env_var('YB_BUILD_TYPE', self.build_type)

        # Do not try to make rpaths relative during the configure step, as that may slow it down.
        self.set_env_var('YB_DISABLE_RELATIVE_RPATH', '1' if step == 'configure' else '0')
        if self.build_type == 'compilecmds':
            self.set_env_var('YB_SKIP_LINKING', '1')

        # We need to add this directory to PATH so Postgres build could find Bison.
        thirdparty_installed_common_bin_path = os.path.join(
            self.thirdparty_dir, 'installed', 'common', 'bin')
        new_path_str = ':'.join([thirdparty_installed_common_bin_path] + self.original_path)
        self.set_env_var('PATH', new_path_str)

    def sync_postgres_source(self):
        logging.info("Syncing postgres source code")
        # Remove source code files from the build directory that have been removed from the
        # source directory.
        # TODO: extend this to the complete list of source file types.
        run_program([
            'rsync', '-avz',
            '--include', '*.c',
            '--include', '*.h',
            '--include', 'Makefile',
            '--include', '*.am',
            '--include', '*.in',
            '--include', '*.mk',
            '--exclude', '*',
            '--delete',
            self.postgres_src_dir + '/', self.pg_build_root],
            capture_output=False)
        run_program([
            'rsync',
            '-az',
            '--exclude', '*.sw?',
            '--exclude', '*.bak',
            '--exclude', '*.orig',
            '--exclude', '*.rej',
            self.postgres_src_dir + '/', self.pg_build_root],
            capture_output=False)
        if is_verbose_mode():
            logging.info("Successfully synced postgres source code")

    def clean_postgres(self):
        logging.info("Removing the postgres build and installation directories")
        for dir_to_delete in [self.pg_build_root, self.pg_prefix]:
            logging.info("Deleting the directory '%s'", dir_to_delete)
            # rm -rf is much faster than Python's rmtree.
            run_program(['rm', '-rf', dir_to_delete], capture_output=False)

    def configure_postgres(self):
        if is_verbose_mode():
            logging.info("Running configure in the postgres build directory")
        # Don't enable -Werror when running configure -- that can affect the resulting
        # configuration.
        configure_cmd_line = [
                './configure',
                '--prefix', self.pg_prefix,
                '--with-extra-version=-YB-' + self.get_yb_version(),
                '--enable-depend',
                '--with-ldap',
                '--with-openssl',
                # Options are ossp (original/old implementation), bsd (BSD) and e2fs
                # (libuuid-based for Unix/Mac).
                '--with-uuid=e2fs',
                '--with-libedit-preferred',
                '--with-includes=' + self.openssl_include_dir,
                '--with-libraries=' + self.openssl_lib_dir,
                # We're enabling debug symbols for all types of builds.
                '--enable-debug']
        if not get_bool_env_var('YB_NO_PG_CONFIG_CACHE'):
            configure_cmd_line.append('--config-cache')

        # We get readline-related errors in ASAN/TSAN, so let's disable readline there.
        if self.build_type in ['asan', 'tsan']:
            # TODO: do we still need this limitation?
            configure_cmd_line += ['--without-readline']

        if self.build_type != 'release':
            configure_cmd_line += ['--enable-cassert']
        # Unset YB_SHOW_COMPILER_COMMAND_LINE when configuring postgres to avoid unintended side
        # effects from additional compiler output.
        with EnvVarContext(YB_SHOW_COMPILER_COMMAND_LINE=None):
            configure_result = run_program(configure_cmd_line, error_ok=True)
        if configure_result.failure():
            rerun_configure = False
            for line in configure_result.stderr.splitlines():
                if REMOVE_CONFIG_CACHE_MSG_RE.search(line.strip()):
                    logging.info("Configure failed because of stale config.cache, re-running.")
                    run_program('rm -f config.cache')
                    rerun_configure = True
                    break

            if not rerun_configure:
                logging.error("Standard error from configure:\n" + configure_result.stderr)
                config_log_path = os.path.join(self.pg_build_root, "config.log")
                if os.path.exists(config_log_path):
                    with open(config_log_path) as config_log_file:
                        config_log_str = config_log_file.read()
                    logging.info(f"Contents of {config_log_path}:")
                    sys.stderr.write(config_log_str + "\n")
                else:
                    logging.warning(f"File not found: {config_log_path}")
                raise RuntimeError("configure failed")

            configure_result = run_program(
                configure_cmd_line, shell=True, stdout_stderr_prefix='configure', error_ok=True)

        if is_verbose_mode() and configure_result.success():
            configure_result.print_output_to_stdout()

        configure_result.print_output_and_raise_error_if_failed()

        logging.info("Successfully ran configure in the postgres build directory")

    def get_env_vars_str(self, env_var_names):
        return "\n".join(
            "%s=%s" % (k, os.environ[k])
            for k in sorted(env_var_names)
            if k in os.environ
        )

    def get_build_stamp(self, include_env_vars):
        """
        Creates a "build stamp" that tries to capture all inputs that might affect the PostgreSQL
        code. This is needed to avoid needlessly rebuilding PostgreSQL, as it takes ~10 seconds
        even if there are no code changes.
        """

        with WorkDirContext(YB_SRC_ROOT):
            code_subset = [
                'src/postgres',
                'src/yb/yql/pggate',
                'python/yb/build_postgres.py',
                'build-support/build_postgres',
                'CMakeLists.txt'
            ]
            git_hash = subprocess.check_output(
                ['git', '--no-pager', 'log', '-n', '1', '--pretty=%H'] + code_subset).strip()
            git_diff = subprocess.check_output(['git', 'diff'] + code_subset)
            git_diff_cached = subprocess.check_output(['git', 'diff', '--cached'] + code_subset)

        env_vars_str = self.get_env_vars_str(self.env_vars_for_build_stamp)
        build_stamp = "\n".join([
            "git_commit_sha1=%s" % git_hash,
            "git_diff_sha256=%s" % sha256(git_diff),
            "git_diff_cached_sha256=%s" % sha256(git_diff_cached)
            ])

        if include_env_vars:
            build_stamp += "\nenv_vars_sha256=%s" % hashlib.sha256(
                env_vars_str.encode('utf-8')).hexdigest()

        return build_stamp.strip()

    def get_saved_build_stamp(self):
        if os.path.exists(self.build_stamp_path):
            with open(self.build_stamp_path) as build_stamp_file:
                return build_stamp_file.read().strip()

    def make_postgres(self):
        self.set_env_vars('make')
        # Postgresql requires MAKELEVEL to be 0 or non-set when calling its make.
        # But in case YB project is built with make, MAKELEVEL is not 0 at this point.
        make_cmd = ['make', 'MAKELEVEL=0']

        make_parallelism = os.environ.get('YB_MAKE_PARALLELISM')
        if make_parallelism:
            make_parallelism = int(make_parallelism)
        if self.build_uses_remote_compilation and not self.remote_compilation_allowed:
            # Since we're building everything locally in this case, and YB_MAKE_PARALLELISM is
            # likely specified for distributed compilation, cap it at some factor times the number
            # of CPU cores.
            parallelism_cap = multiprocessing.cpu_count() * 2
            if make_parallelism:
                make_parallelism = min(parallelism_cap, make_parallelism)
            else:
                make_parallelism = parallelism_cap

        if make_parallelism:
            make_cmd += ['-j', str(int(make_parallelism))]

        self.set_env_var('YB_COMPILER_TYPE', self.compiler_type)

        # Create a script allowing to easily run "make" from the build directory with the right
        # environment.
        env_script_content = ''
        for env_var_name in CONFIG_ENV_VARS:
            env_var_value = os.environ.get(env_var_name)
            if env_var_value is None:
                raise RuntimeError("Expected env var %s to be set" % env_var_name)
            env_script_content += "export %s=%s\n" % (env_var_name, quote_for_bash(env_var_value))

        compile_commands_files = []

        work_dirs = [self.pg_build_root, os.path.join(self.pg_build_root, 'contrib')]

        for work_dir in work_dirs:
            with WorkDirContext(work_dir):
                # Create a script to run Make easily with the right environment.
                make_script_path = 'make.sh'
                with open(make_script_path, 'w') as out_f:
                    out_f.write(
                        '#!/usr/bin/env bash\n'
                        '. "${BASH_SOURCE%/*}"/env.sh\n'
                        'make "$@"\n')
                with open('env.sh', 'w') as out_f:
                    out_f.write(env_script_content)

                run_program(['chmod', 'u+x', make_script_path])

                # Actually run Make.
                if is_verbose_mode():
                    logging.info("Running make in the %s directory", work_dir)

                make_result = run_program(
                    make_cmd, stdout_stderr_prefix='make', cwd=work_dir, shell=True, error_ok=True
                )
                if make_result.failure():
                    make_result.print_output_to_stdout()
                    raise RuntimeError("PostgreSQL compilation failed")

                if self.build_type == 'compilecmds':
                    logging.info(
                            "Not running make install in the %s directory since we are only "
                            "generating the compilation database", work_dir)
                else:
                    run_program(
                        'make install', stdout_stderr_prefix='make_install',
                        cwd=work_dir, shell=True, error_ok=True
                    ).print_output_and_raise_error_if_failed()
                    logging.info("Successfully ran make in the %s directory", work_dir)

                if self.export_compile_commands:
                    logging.info("Generating the compilation database in directory '%s'", work_dir)

                    compile_commands_path = os.path.join(work_dir, 'compile_commands.json')
                    self.set_env_var('YB_PG_SKIP_CONFIG_STATUS', '1')
                    if not os.path.exists(compile_commands_path):
                        run_program(['compiledb', 'make', '-n'], capture_output=False)
                    del os.environ['YB_PG_SKIP_CONFIG_STATUS']

                    if not os.path.exists(compile_commands_path):
                        raise RuntimeError("Failed to generate compilation database at: %s" %
                                           compile_commands_path)
                    compile_commands_files.append(compile_commands_path)

        if self.export_compile_commands:
            self.combine_compile_commands(compile_commands_files)

    def combine_compile_commands(self, compile_commands_files):
        """
        Combine compilation commands files from main and contrib subtrees of PostgreSQL, patch them
        so that they point to the original source directory, and concatenate with the main
        compilation commands file.
        """
        all_compile_commands_paths = compile_commands_files + [
            os.path.join(self.build_root, 'compile_commands.json')
        ]

        # -----------------------------------------------------------------------------------------
        # Combine raw compilation commands in a single file.
        # -----------------------------------------------------------------------------------------

        combined_raw_compile_commands = []
        for compile_commands_path in all_compile_commands_paths:
            combined_raw_compile_commands += read_json_file(compile_commands_path)

        write_json_file(
            combined_raw_compile_commands,
            os.path.join(self.build_root, COMBINED_RAW_COMPILE_COMMANDS_FILE_NAME),
            description_for_log='combined raw compilation commands file')

        # -----------------------------------------------------------------------------------------
        # Combine post-processed compilation commands in a single file.
        # -----------------------------------------------------------------------------------------

        compile_command_processor = CompileCommandProcessor(self.build_root)
        combined_postprocessed_compile_commands = [
            compile_command_processor.postprocess_compile_command(item)
            for item in combined_raw_compile_commands
        ]

        combined_postprocessed_compile_commands_path = os.path.join(
            self.build_root, COMBINED_POSTPROCESSED_COMPILE_COMMANDS_FILE_NAME)

        write_json_file(
            combined_postprocessed_compile_commands,
            combined_postprocessed_compile_commands_path,
            description_for_log='combined postprocessed compilation commands file')

        create_compile_commands_symlink(
            combined_postprocessed_compile_commands_path, self.build_type)

    @overrides
    def run_impl(self):
        self.build_postgres()

    def steps_description(self):
        if self.args.step is None:
            return "all steps in {}".format(BUILD_STEPS)
        return "the '%s' step" % (self.args.step)

    def build_postgres(self):
        start_time_sec = time.time()
        if self.args.clean:
            self.clean_postgres()

        mkdir_p(self.pg_build_root)

        self.set_env_vars('configure')
        saved_build_stamp = self.get_saved_build_stamp()
        initial_build_stamp = self.get_build_stamp(include_env_vars=True)
        initial_build_stamp_no_env = self.get_build_stamp(include_env_vars=False)
        logging.info("PostgreSQL build stamp:\n%s", initial_build_stamp)

        if initial_build_stamp == saved_build_stamp:
            if self.export_compile_commands:
                logging.info(
                    "Even though PostgreSQL is already up-to-date in directory %s, we still need "
                    "to create compile_commands.json, so proceeding with %s",
                    self.pg_build_root, self.steps_description())
            else:
                logging.info(
                    "PostgreSQL is already up-to-date in directory %s, skipping %s.",
                    self.pg_build_root, self.steps_description())
                return

        with WorkDirContext(self.pg_build_root):
            if self.should_configure:
                self.sync_postgres_source()
                configure_start_time_sec = time.time()
                self.configure_postgres()
                logging.info("The configure step of building PostgreSQL took %.1f sec",
                             time.time() - configure_start_time_sec)
            if self.should_make:
                make_start_time_sec = time.time()
                self.make_postgres()
                logging.info("The make step of building PostgreSQL took %.1f sec",
                             time.time() - make_start_time_sec)

        if self.should_make:
            # Guard against the code having changed while we were building it.
            final_build_stamp_no_env = self.get_build_stamp(include_env_vars=False)
            if final_build_stamp_no_env == initial_build_stamp_no_env:
                logging.info("Updating build stamp file at %s", self.build_stamp_path)
                with open(self.build_stamp_path, 'w') as build_stamp_file:
                    build_stamp_file.write(initial_build_stamp)
            else:
                logging.warning("PostgreSQL build stamp changed during the build! Not updating.")

        logging.info(
            "PostgreSQL build (%s) took %.1f sec",
            self.steps_description(), time.time() - start_time_sec)


def main():
    init_logging()
    if get_bool_env_var('YB_SKIP_POSTGRES_BUILD'):
        logging.info("Skipping PostgreSQL build (YB_SKIP_POSTGRES_BUILD is set)")
        return
    PostgresBuilder().run()


if __name__ == '__main__':
    main()
