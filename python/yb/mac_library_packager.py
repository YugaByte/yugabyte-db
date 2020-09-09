# Copyright (c) YugaByte, Inc.

import glob
import logging
import os
import shutil
import stat
import subprocess
import sys

from yb.command_util import run_program
from yb.common_util import get_thirdparty_dir


def add_common_arguments(parser):
    """
    Add command-line arguments common between library_packager_old.py invoked as a script, and
    the yb_release.py script.
    """
    parser.add_argument('--verbose',
                        help='Enable verbose output.',
                        action='store_true')


class MacLibraryPackager:
    def __init__(self,
                 build_dir,
                 seed_executable_patterns,
                 dest_dir,
                 verbose_mode=False):
        self.build_dir = os.path.realpath(build_dir)
        if not os.path.exists(self.build_dir):
            raise IOError("Build directory '{}' does not exist".format(self.build_dir))
        self.seed_executable_patterns = seed_executable_patterns
        self.dest_dir = dest_dir
        logging.debug(
            "Traversing the dependency graph of executables/libraries, starting "
            "with seed executable patterns: {}".format(", ".join(seed_executable_patterns)))
        self.nodes_by_digest = {}
        self.nodes_by_path = {}

        self.dest_dir = dest_dir
        self.verbose_mode = verbose_mode
        self.absolute_paths_by_libname = {}

    def package_binaries(self):
        src = self.build_dir
        dst = self.dest_dir

        dst_bin_dir = os.path.join(dst, 'bin')
        dst_lib_dir = os.path.join(dst, 'lib')

        try:
            os.makedirs(dst_bin_dir)
        except OSError as e:
            raise RuntimeError('Unable to create directory %s', dst)

        logging.debug('Created directory %s', dst)

        bin_dir_files = []
        for seed_executable_glob in self.seed_executable_patterns:
            if seed_executable_glob.find('postgres/bin/') >= 0:
                # Skip postgres binaries since they are copied with the postgres root directory
                # which is handled below.
                continue
            if seed_executable_glob.startswith('bin/'):
                bin_dir_files.append(os.path.basename(seed_executable_glob))
                logging.debug("Adding file '%s' to bash_scripts", seed_executable_glob)
            updated_glob = seed_executable_glob.replace('$BUILD_ROOT', self.build_dir)
            if updated_glob != seed_executable_glob:
                logging.info('Substituting: {} -> {}'.format(seed_executable_glob, updated_glob))
                seed_executable_glob = updated_glob
            glob_results = glob.glob(seed_executable_glob)
            if not glob_results:
                raise RuntimeError("No files found matching the pattern '{}'".format(
                    seed_executable_glob))
            for executable in glob_results:
                shutil.copy(executable, dst_bin_dir)

        src_lib_dir = os.path.join(src, 'lib')
        yb_lib_file_for_postgres = os.path.join(src_lib_dir, 'libyb_pggate.dylib')
        libedit_file_for_postgres = os.path.join(get_thirdparty_dir(),
                                                 'installed/common/lib/libedit.dylib')

        processed_libs = []
        for bin_file in os.listdir(dst_bin_dir):
            if bin_file.endswith('.sh') or bin_file in bin_dir_files:
                logging.info("Not modifying rpath for file '%s' because it's not a binary file",
                             bin_file)
                continue

            logging.debug('Processing binary file: %s', bin_file)
            libs = []

            os.makedirs(os.path.join(dst, 'lib', bin_file))
            libs = self.fix_load_paths(os.path.join(dst_bin_dir, bin_file),
                                       os.path.join(dst_lib_dir, bin_file),
                                       os.path.join('@loader_path/../lib/', bin_file))

            # Elements in libs are absolute paths.
            logging.debug('library dependencies for file %s: %s', bin_file, libs)

            # Treat this as a special case for now (10/14/18).
            libs.append(yb_lib_file_for_postgres)
            libs.append(libedit_file_for_postgres)
            for lib in libs:
                if lib in processed_libs:
                    continue

                # For each library dependency, check whether it already has its own directory (if it
                # does, a physical copy of this library must exist there). If it doesn't, create it
                # and copy the physical file there.
                logging.debug('Processing library: %s', lib)
                libname = os.path.basename(lib)
                lib_dir_path = os.path.join(dst, 'lib', libname)
                if os.path.exists(lib_dir_path):
                    continue

                os.mkdir(lib_dir_path)
                shutil.copy(lib, lib_dir_path)

                lib_file_path = os.path.join(lib_dir_path, libname)

                new_libs = self.fix_load_paths(lib_file_path, lib_dir_path, '@loader_path')
                for new_lib in new_libs:
                    if new_lib not in processed_libs and new_lib not in libs:
                        logging.info('Adding dependency %s for library %s', new_lib, lib_file_path)
                        libs.append(new_lib)
                processed_libs.append(lib)

        # Handle postgres as a special case for now (10/14/18).
        postgres_src = os.path.join(src, 'postgres')
        postgres_dst = os.path.join(dst, 'postgres')
        shutil.copytree(postgres_src, postgres_dst, symlinks=True)
        postgres_bin = os.path.join(postgres_dst, 'bin')
        postgres_lib = os.path.join(postgres_dst, 'lib')
        for bin_file in os.listdir(postgres_bin):
            self.fix_postgres_load_paths(os.path.join(postgres_bin, bin_file), dst)
        for lib_file in os.listdir(postgres_lib):
            if os.path.isdir(os.path.join(postgres_lib, lib_file)):
                continue
            logging.debug("Processing postgres library %s", lib_file)
            self.fix_postgres_load_paths(os.path.join(postgres_lib, lib_file), dst)

    # Run otool to extract information from an object file. Returns the command's output to stdout,
    # or an empty string if filename is not a valid object file.
    # parameter must include the dash.
    def run_otool(self, parameter, filename):
        result = run_program(['otool', parameter, filename], error_ok=True)

        if result.stdout.endswith('is not an object file') or \
                result.stderr.endswith('The file was not recognized as a valid object file'):
            logging.info("Unable to run 'otool %s %s'. File '%s' is not an object file",
                         filename, parameter, filename)
            return None

        if result.returncode != 0:
            raise RuntimeError("Unexpected error running 'otool -l %s': '%s'",
                               filename, result.stderr)

        return result.stdout

    def extract_rpaths(self, filename):
        stdout = self.run_otool('-l', filename)

        if not stdout:
            return []

        rpaths = []
        lines = stdout.splitlines()
        for idx, line in enumerate(lines):
            # Extract rpath. Sample output from 'otool -l':
            # Load command 78
            #          cmd LC_RPATH
            #      cmdsize 72
            #         path /Users/hector/code/yugabyte/thirdparty/installed/common/lib (offset 12)
            if line.strip() == 'cmd LC_RPATH':
                path_line = lines[idx + 2]
                if path_line.split()[0] != 'path':
                    raise RuntimeError("Invalid output from 'otool -l %s'. "
                                       "Expecting line to start with 'path'. Got '%s'",
                                       filename, path_line.split()[0])
                rpaths.append(path_line.split()[1])

        return rpaths

    def extract_dependency_paths(self, filename, rpaths):
        stdout = self.run_otool('-L', filename)

        if not stdout:
            return [], []

        dependency_paths = []
        absolute_dependency_paths = []
        lines = stdout.splitlines()
        # Skip the first line that is always the library path.
        for line in lines[1:]:
            path = line.split()[0]

            # The paths extracted by using otool -L can be absolute paths or relative paths starting
            # with @rpath or @loader_path. Example:
            # otool -L ./build/debug-clang-dynamic-enterprise/lib/libmaster.dylib
            # ./build/debug-clang-dynamic-enterprise/lib/libmaster.dylib:
            #    @rpath/libmaster.dylib (compatibility version 0.0.0, current version 0.0.0)
            #    @rpath/libtserver.dylib (compatibility version 0.0.0, current version 0.0.0)
            #    @rpath/libtablet.dylib (compatibility version 0.0.0, current version 0.0.0)
            #    /Users/hector/code/yugabyte/thirdparty/installed/uninstrumented/lib/\
            # libsnappy.1.dylib (compatibility version 3.0.0, current version 3.4.0)
            #    /usr/lib/libbz2.1.0.dylib (compatibility version 1.0.0, current version 1.0.5)
            #    /usr/lib/libz.1.dylib (compatibility version 1.0.0, current version 1.2.8)
            #
            # So we want to find the absolute paths of those paths that start with @rpath by trying
            # all the rpaths extracted by using 'otool -l'

            # If we don't skip system libraries and package it, macOS will complain that the library
            # exists in two different places (in /usr/lib and in our package lib directory).
            if path.startswith('/usr/lib'):
                self.absolute_paths_by_libname[os.path.basename(path)] = path
                continue
            if path.startswith('@rpath'):
                name = os.path.basename(path)
                # Find the absolute path by prepending all the rpaths extracted from the file.
                for rpath in rpaths:
                    candidate_path = os.path.join(rpath, name)
                    if os.path.isfile(candidate_path):
                        absolute_dependency_paths.append(candidate_path)
                        self.absolute_paths_by_libname[name] = candidate_path
                        break
            elif path.startswith('@loader_path'):
                absolute_path = self.absolute_paths_by_libname[os.path.basename(filename)]
                if not os.path.isfile(absolute_path):
                    raise RuntimeError("File %s doesn't exist" % absolute_path)

                # Replace @loader_path with the absolute dir path of filename.
                absolute_dir = os.path.dirname(absolute_path)
                new_lib_path = path.replace('@loader_path', absolute_dir)
                name = os.path.basename(path)
                if not os.path.isfile(new_lib_path):
                    raise RuntimeError("File %s doesn't exist" % new_lib_path)
                absolute_dependency_paths.append(new_lib_path)
                self.absolute_paths_by_libname[name] = new_lib_path
            else:
                # This should be an absolute path.
                if os.path.isfile(path):
                    absolute_dependency_paths.append(path)
                    self.absolute_paths_by_libname[os.path.basename(path)] = path
                else:
                    raise RuntimeError("File %s doesn't exist" % path)
            dependency_paths.append(path)

        return dependency_paths, absolute_dependency_paths

    def remove_rpaths(self, filename, rpaths):
        for rpath in rpaths:
            run_program(['install_name_tool', '-delete_rpath', rpath, filename])
            logging.debug('Successfully removed rpath %s from %s', rpath, filename)

    def set_new_path(self, filename, old_path, new_path):
        # We need to use a different command if the path is pointing to itself. Example:
        # otool - L ./build/debug-clang-dynamic-enterprise/lib/libmaster.dylib
        # ./build/debug-clang-dynamic-enterprise/ lib/libmaster.dylib:
        #      @rpath/libmaster.dylib

        cmd = []
        if os.path.basename(filename) == os.path.basename(old_path):
            run_program(['install_name_tool', '-id', new_path, filename])
            logging.debug('install_name_tool -id %s %s', new_path, filename)
        else:
            run_program(['install_name_tool', '-change', old_path, new_path, filename])
            logging.debug('install_name_tool -change %s %s %s', old_path, new_path, filename)

    def fix_load_paths(self, filename, lib_bin_dir, loader_path):
        logging.debug('Processing file %s', filename)

        original_mode = os.stat(filename).st_mode
        # Make the file writable.
        try:
            os.chmod(filename, os.stat(filename).st_mode | stat.S_IWUSR)
        except OSError as e:
            logging.error('Unable to make file %s writable', filename)
            raise

        # Extract the paths that are used to resolve paths that start with @rpath.
        rpaths = self.extract_rpaths(filename)

        # Remove rpaths since we are only going to use @loader_path and absolute paths.
        self.remove_rpaths(filename, rpaths)

        # Dependency path will have the paths as extracted by 'otool -L'.
        dependency_paths, absolute_dependency_paths = self.extract_dependency_paths(filename,
                                                                                    rpaths)

        logging.debug('Absolute_dependency_paths for file %s: %s',
                      filename, absolute_dependency_paths)

        # Prepend @loader_path to all dependency paths.
        for dependency_path in dependency_paths:
            basename = os.path.basename(dependency_path)
            new_path = os.path.join(loader_path, basename)

            self.set_new_path(filename, dependency_path, new_path)

        logging.debug('Absolute_paths for %s: %s', filename, absolute_dependency_paths)

        # Since we have changed the dependency path, create a symlink so that the dependency path
        # points to a valid file. It's not guaranteed that this symlink will point to a valid
        # physical file, so the caller is responsible to make sure the physical file exists.
        for absolute_path in absolute_dependency_paths:
            lib_file_name = os.path.basename(absolute_path)
            relative_lib_path = os.path.join("..", lib_file_name, lib_file_name)

            # Create symlink in lib_bin_dir.
            symlink_path = os.path.join(lib_bin_dir, lib_file_name)
            if not os.path.exists(symlink_path):
                logging.debug('Creating symlink %s -> %s', symlink_path, relative_lib_path)
                os.symlink(relative_lib_path, symlink_path)

        # Restore the file's mode.
        try:
            os.chmod(filename, original_mode)
        except OSError as e:
            logging.error('Unable to restore file %s mode', filename)
            raise

        return absolute_dependency_paths

    # Special case for now (10/14/18).
    def fix_postgres_load_paths(self, filename, dst):
        if os.path.islink(filename):
            return []

        libs = []

        original_mode = os.stat(filename).st_mode
        # Make the file writable.
        try:
            os.chmod(filename, original_mode | stat.S_IWUSR)
        except OSError as e:
            logging.error('Unable to make file %s writable', filename)
            raise

        # Extract the paths that are used to resolve paths that start with @rpath.
        rpaths = self.extract_rpaths(filename)

        # Remove rpaths since we will only use @loader_path and absolute paths for system libraries.
        self.remove_rpaths(filename, rpaths)

        logging.debug('Processing file %s for rpaths %s', filename, rpaths)
        if len(rpaths) == 0:
            return []

        # Dependency path will have the paths as extracted by 'otool -L'
        dependency_paths, absolute_dependency_paths = \
            self.extract_dependency_paths(filename, rpaths)

        postgres_dst = os.path.join(dst, 'postgres')
        lib_files = os.listdir(os.path.join(postgres_dst, "lib"))
        for dependency_path in dependency_paths:
            basename = os.path.basename(dependency_path)
            new_path = ''

            if basename in lib_files:
                # If the library is in postgres/lib, then add @loader_path/../
                new_path = os.path.join('@loader_path/../lib', basename)
                logging.info('Setting new path to %s for file %s', new_path, filename)
                self.set_new_path(filename, dependency_path, new_path)
            else:
                # Search in dst/lib
                found = False
                dst_lib = os.path.join(dst, 'lib')
                if basename in os.listdir(dst_lib):
                    new_path = os.path.join('@loader_path/../../lib', basename, basename)
                    logging.info('Setting new path to %s for file %s', new_path, filename)
                    self.set_new_path(filename, dependency_path, new_path)
                else:
                    # Search the file in the rpaths directories.
                    for rpath in rpaths:
                        if basename in os.listdir(rpath):
                            # This shouldn't happen.
                            raise RuntimeError("lib %s" % os.path.join(rpath, basename))

        postgres_lib = os.path.join(postgres_dst, 'lib')
        for absolute_dependency in absolute_dependency_paths:
            if os.path.dirname(absolute_dependency) == postgres_lib:
                basename = os.path.basename(absolute_dependency)
                new_path = os.path.join('@loader_path/../lib', basename)
                self.set_new_path(filename, absolute_dependency, new_path)
                libs.append(basename)
            logging.info('Absolute dependency %s', absolute_dependency)

        # Restore the file's mode.
        try:
            os.chmod(filename, original_mode)
        except OSError as e:
            logging.error('Unable to restore file %s mode', filename)
            raise
