#!/usr/bin/env python3

"""
Build a dependency graph of sources, object files, libraries, and binaries.  Compute the set of
tests that might be affected by changes to the given set of source files.
"""

import argparse
import fnmatch
import json
import logging
import os
import re
import subprocess
import sys
import unittest
import pipes
import platform

from datetime import datetime

from typing import Optional, List, Dict

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from yb.common_util import \
        group_by, make_set, get_build_type_from_build_root, get_yb_src_root_from_build_root, \
        convert_to_non_ninja_build_root, get_bool_env_var, is_ninja_build_root  # nopep8
from yb.command_util import mkdir_p  # nopep8
from yugabyte_pycommon import WorkDirContext  # nopep8


def make_extensions(exts_without_dot):
    return ['.' + ext for ext in exts_without_dot]


def ends_with_one_of(path, exts):
    for ext in exts:
        if path.endswith(ext):
            return True
    return False


def get_relative_path_or_none(abs_path, relative_to):
    """
    If the given path starts with another directory path, return the relative path, or else None.
    """
    if not relative_to.endswith('/'):
        relative_to += '/'
    if abs_path.startswith(relative_to):
        return abs_path[len(relative_to):]


SOURCE_FILE_EXTENSIONS = make_extensions(['c', 'cc', 'cpp', 'cxx', 'h', 'hpp', 'hxx', 'proto',
                                          'l', 'y'])
LIBRARY_FILE_EXTENSIONS_NO_DOT = ['so', 'dylib']
LIBRARY_FILE_EXTENSIONS = make_extensions(LIBRARY_FILE_EXTENSIONS_NO_DOT)
TEST_FILE_SUFFIXES = ['_test', '-test', '_itest', '-itest']
LIBRARY_FILE_NAME_RE = re.compile(r'^lib(.*)[.](?:%s)$' % '|'.join(
    LIBRARY_FILE_EXTENSIONS_NO_DOT))
PROTO_LIBRARY_FILE_NAME_RE = re.compile(r'^lib(.*)_proto[.](?:%s)$' % '|'.join(
    LIBRARY_FILE_EXTENSIONS_NO_DOT))
EXECUTABLE_FILE_NAME_RE = re.compile(r'^[a-zA-Z0-9_.-]+$')
PROTO_OUTPUT_FILE_NAME_RE = re.compile(r'^([a-zA-Z_0-9-]+)[.]pb[.](h|cc)$')

# Ignore some special-case CMake targets that do not have a one-to-one match with executables or
# libraries.
IGNORED_CMAKE_TARGETS = ['gen_version_info', 'latest_symlink', 'postgres']

LIST_DEPS_CMD = 'deps'
LIST_REVERSE_DEPS_CMD = 'rev-deps'
LIST_AFFECTED_CMD = 'affected'
SELF_TEST_CMD = 'self-test'
DEBUG_DUMP_CMD = 'debug-dump'

COMMANDS = [LIST_DEPS_CMD,
            LIST_REVERSE_DEPS_CMD,
            LIST_AFFECTED_CMD,
            SELF_TEST_CMD,
            DEBUG_DUMP_CMD]

COMMANDS_NOT_NEEDING_TARGET_SET = [SELF_TEST_CMD, DEBUG_DUMP_CMD]

HOME_DIR = os.path.realpath(os.path.expanduser('~'))

# This will match any node type (node types being sources/libraries/tests/etc.)
NODE_TYPE_ANY = 'any'

# As of August 2019, there is nothing in the "bin", "managed" and "www" directories that
# is being used by tests.
# If that changes, this needs to be updated. Note that the "bin" directory here is the
# yugabyte/bin directory in the source tree, not the "bin" directory under the build
# directory, so it only has scripts and not yb-master / yb-tserver binaries.
DIRECTORIES_THAT_DO_NOT_AFFECT_TESTS = [
    'architecture',
    'bin',
    'cloud',
    'community',
    'docs',
    'managed',
    'sample',
    'www',
]
CATEGORY_DOES_NOT_AFFECT_TESTS = 'does_not_affect_tests'

# File changes in any category other than these will cause all tests to be re-run.  Even though
# changes to Python code affect the test framework, we can consider this Python code to be
# reasonably tested already, by running doctests, this script, the packaging script, etc.  We can
# remove "python" from this whitelist in the future.
CATEGORIES_NOT_CAUSING_RERUN_OF_ALL_TESTS = set(
        ['java', 'c++', 'python', CATEGORY_DOES_NOT_AFFECT_TESTS])

DYLIB_SUFFIX = '.dylib' if platform.system() == 'Darwin' else '.so'


def is_object_file(path):
    return path.endswith('.o')


def append_to_list_in_dict(dest, key, new_item):
    if key in dest:
        dest[key].append(new_item)
    else:
        dest[key] = [new_item]


def get_node_type_by_path(path):
    """
    >>> get_node_type_by_path('my_source_file.cc')
    'source'
    >>> get_node_type_by_path('my_library.so')
    'library'
    >>> get_node_type_by_path('/bin/bash')
    'executable'
    >>> get_node_type_by_path('my_object_file.o')
    'object'
    >>> get_node_type_by_path('tests-integration/some_file')
    'test'
    >>> get_node_type_by_path('tests-integration/some_file.txt')
    'other'
    >>> get_node_type_by_path('something/my-test')
    'test'
    >>> get_node_type_by_path('something/my_test')
    'test'
    >>> get_node_type_by_path('something/my-itest')
    'test'
    >>> get_node_type_by_path('something/my_itest')
    'test'
    >>> get_node_type_by_path('some-dir/some_file')
    'other'
    """
    if ends_with_one_of(path, SOURCE_FILE_EXTENSIONS):
        return 'source'

    if ends_with_one_of(path, LIBRARY_FILE_EXTENSIONS):
        return 'library'

    if (ends_with_one_of(path, TEST_FILE_SUFFIXES) or
            (os.path.basename(os.path.dirname(path)).startswith('tests-') and
             '.' not in os.path.basename(path))):
        return 'test'

    if is_object_file(path):
        return 'object'

    if os.path.exists(path) and os.access(path, os.X_OK):
        # This will only work if the code has been fully built.
        return 'executable'

    return 'other'


class Node:
    """
    A node in the dependency graph. Could be a source file, a header file, an object file, a
    dynamic library, or an executable.
    """
    def __init__(self, path, dep_graph, source_str):
        path = os.path.realpath(path)
        self.path = path

        # Other nodes that this node depends on.
        self.deps = set()

        # Nodes that depend on this node.
        self.reverse_deps = set()

        self.node_type = get_node_type_by_path(path)
        self.dep_graph = dep_graph
        self.conf = dep_graph.conf
        self.source_str = source_str

        self.is_proto_lib = (
            self.node_type == 'library' and
            PROTO_LIBRARY_FILE_NAME_RE.match(os.path.basename(self.path)))

        self._cached_proto_lib_deps = None
        self._cached_containing_binaries = None
        self._cached_cmake_target = None
        self._has_no_cmake_target = False

    def add_dependency(self, dep):
        assert self is not dep
        self.deps.add(dep)
        dep.reverse_deps.add(self)

    def __eq__(self, other):
        if not isinstance(other, Node):
            return False

        return self.path == other.path

    def __hash__(self):
        return hash(self.path)

    def is_source(self):
        return self.node_type == 'source'

    def validate_existence(self):
        if not os.path.exists(self.path) and not self.dep_graph.conf.incomplete_build:
            raise RuntimeError(
                    "Path does not exist: '{}'. This node was found in: {}".format(
                        self.path, self.source_str))

    def get_pretty_path(self):
        for prefix, alias in [(self.conf.build_root, '$BUILD_ROOT'),
                              (self.conf.yb_src_root, '$YB_SRC_ROOT'),
                              (HOME_DIR, '~')]:
            if self.path.startswith(prefix + '/'):
                return alias + '/' + self.path[len(prefix) + 1:]

        return self.path

    def path_rel_to_build_root(self):
        return get_relative_path_or_none(self.path, self.conf.build_root)

    def path_rel_to_src_root(self):
        return get_relative_path_or_none(self.path, self.conf.yb_src_root)

    def __str__(self):
        return "Node(\"{}\", type={}, {} deps, {} rev deps)".format(
                self.get_pretty_path(), self.node_type, len(self.deps), len(self.reverse_deps))

    def __repr__(self):
        return self.__str__()

    def get_cmake_target(self):
        """
        @return the CMake target based on the current node's path. E.g. this would be "master"
                for the "libmaster.so" library, "yb-master" for the "yb-master" executable.
        """
        if self._cached_cmake_target:
            return self._cached_cmake_target
        if self._has_no_cmake_target:
            return None

        if self.path.endswith('.proto'):
            path = self.path
            names = []
            while path != '/' and path != self.conf.yb_src_root:
                names.append(os.path.basename(path))
                path = os.path.dirname(path)

            # This needs to be consistent with the following CMake code snippet:
            #
            #   set(TGT_NAME "gen_${PROTO_REL_TO_YB_SRC_ROOT}")
            #   string(REPLACE "@" "_" TGT_NAME ${TGT_NAME})
            #   string(REPLACE "." "_" TGT_NAME ${TGT_NAME})
            #   string(REPLACE "-" "_" TGT_NAME ${TGT_NAME})
            #   string(REPLACE "/" "_" TGT_NAME ${TGT_NAME})
            #
            # (see FindProtobuf.cmake and FindYRPC.cmake).
            #
            # "/" cannot appear in the resulting string, so no need to replace it with "_".
            target = re.sub('[@.-]', '_', '_'.join(['gen'] + names[::-1]))

            if self.conf.verbose:
                logging.info("Associating protobuf file '{}' with CMake target '{}'".format(
                    self.path, target))
            self._cached_cmake_target = target
            return target

        basename = os.path.basename(self.path)
        m = LIBRARY_FILE_NAME_RE.match(basename)
        if m:
            target = m.group(1)
            self._cached_cmake_target = target
            return target
        m = EXECUTABLE_FILE_NAME_RE.match(basename)
        if m:
            self._cached_cmake_target = basename
            return basename
        self._has_no_cmake_target = True
        return None

    def get_containing_binaries(self):
        """
        Returns nodes (usually one node) corresponding to executables or dynamic libraries that the
        given object file is compiled into.
        """
        if self.path.endswith('.cc'):
            cc_o_rev_deps = [rev_dep for rev_dep in self.reverse_deps
                             if rev_dep.path.endswith('.cc.o')]
            if len(cc_o_rev_deps) != 1:
                raise RuntimeError(
                    "Could not identify exactly one object file reverse dependency of node "
                    "%s. All set of reverse dependencies: %s" % (self, self.reverse_deps))
            return cc_o_rev_deps[0].get_containing_binaries()

        if self.node_type != 'object':
            return None
        if self._cached_containing_binaries:
            return self._cached_containing_binaries

        binaries = []
        for rev_dep in self.reverse_deps:
            if rev_dep.node_type in ['library', 'test', 'executable']:
                binaries.append(rev_dep)
        if len(binaries) > 1:
            logging.warning(
                "Node %s is linked into multiple libraries: %s. Might be worth checking.",
                self, binaries)

        self._cached_containing_binaries = binaries
        return binaries

    def get_recursive_deps(self):
        """
        Returns a set of all dependencies that this node depends on.
        """

        recursive_deps = set()
        visited = set()

        def walk(node, add_self=True):
            if add_self:
                recursive_deps.add(node)
            for dep in node.deps:
                if dep not in recursive_deps:
                    walk(dep)
        walk(self, add_self=False)
        return recursive_deps

    def get_proto_lib_deps(self):
        if self._cached_proto_lib_deps is None:
            self._cached_proto_lib_deps = [
                dep for dep in self.get_recursive_deps() if dep.is_proto_lib
            ]
        return self._cached_proto_lib_deps

    def get_containing_proto_lib(self):
        """
        For a .pb.cc file node, return the node corresponding to the containing protobuf library.
        """
        if not self.path.endswith('.pb.cc.o'):
            return

        proto_libs = [binary for binary in self.get_containing_binaries()]

        if len(proto_libs) != 1:
            logging.info("Reverse deps:\n    %s" % ("\n    ".join(
                [str(dep) for dep in self.reverse_deps])))
            raise RuntimeError("Invalid number of proto libs for %s: %s" % (node, proto_libs))
        return proto_libs[0]

    def get_proto_gen_cmake_target(self):
        """
        For .pb.{h,cc} nodes this will return a CMake target of the form
        gen_..., e.g. gen_src_yb_common_wire_protocol.
        """
        rel_path = self.path_rel_to_build_root()
        if not rel_path:
            return None

        basename = os.path.basename(rel_path)
        match = PROTO_OUTPUT_FILE_NAME_RE.match(basename)
        if not match:
            return None
        return '_'.join(
            ['gen'] +
            os.path.dirname(rel_path).split('/') +
            [match.group(1), 'proto']
        )


def set_to_str(items):
    return ",\n".join(sorted(items))


def is_abs_path(path):
    return path.startswith('/')


class Configuration:

    def __init__(self, args):
        self.args = args
        self.verbose = args.verbose
        self.build_root = os.path.realpath(args.build_root)
        self.is_ninja = is_ninja_build_root(self.build_root)

        self.build_type = get_build_type_from_build_root(self.build_root)
        self.yb_src_root = get_yb_src_root_from_build_root(self.build_root, must_succeed=True)
        self.src_dir_path = os.path.join(self.yb_src_root, 'src')
        self.ent_src_dir_path = os.path.join(self.yb_src_root, 'ent', 'src')
        self.rel_path_base_dirs = set([self.build_root, os.path.join(self.src_dir_path, 'yb')])
        self.incomplete_build = args.incomplete_build

        self.file_regex = args.file_regex
        if not self.file_regex and args.file_name_glob:
            self.file_regex = fnmatch.translate('*/' + args.file_name_glob)

        self.src_dir_paths = [self.src_dir_path, self.ent_src_dir_path]

        for dir_path in self.src_dir_paths:
            if not os.path.isdir(dir_path):
                raise RuntimeError("Directory does not exist, or is not a directory: %s" % dir_path)


class CMakeDepGraph:
    """
    A light-weight class representing the dependency graph of CMake targets imported from the
    yb_cmake_deps.txt file that we generate in our top-level CMakeLists.txt. This dependency graph
    does not have any nodes for source files and object files.
    """
    def __init__(self, build_root):
        self.build_root = build_root
        self.cmake_targets = None
        self.cmake_deps_path = os.path.join(self.build_root, 'yb_cmake_deps.txt')
        self.cmake_deps = None
        self._load()

    def _load(self):
        logging.info("Loading dependencies between CMake targets from '{}'".format(
            self.cmake_deps_path))
        self.cmake_deps = {}
        self.cmake_targets = set()
        with open(self.cmake_deps_path) as cmake_deps_file:
            for line in cmake_deps_file:
                line = line.strip()
                if not line:
                    continue
                items = [item.strip() for item in line.split(':')]
                if len(items) != 2:
                    raise RuntimeError(
                            "Expected to find two items when splitting line on ':', found {}:\n{}",
                            len(items), line)
                lhs, rhs = items
                if lhs in IGNORED_CMAKE_TARGETS:
                    continue
                cmake_dep_set = self._get_cmake_dep_set_of(lhs)

                for cmake_dep in rhs.split(';'):
                    if cmake_dep in IGNORED_CMAKE_TARGETS:
                        continue
                    cmake_dep_set.add(cmake_dep)

        for cmake_target, cmake_target_deps in self.cmake_deps.items():
            adding_targets = [cmake_target] + list(cmake_target_deps)
            self.cmake_targets.update(set(adding_targets))
        logging.info("Found {} CMake targets in '{}'".format(
            len(self.cmake_targets), self.cmake_deps_path))

    def _get_cmake_dep_set_of(self, target):
        """
        Get CMake dependencies of the given target. What is returned is a mutable set. Modifying
        this set modifies this CMake dependency graph.
        """
        deps = self.cmake_deps.get(target)
        if deps is None:
            deps = set()
            self.cmake_deps[target] = deps
            self.cmake_targets.add(target)
        return deps

    def add_dependency(self, from_target, to_target):
        if from_target in IGNORED_CMAKE_TARGETS or to_target in IGNORED_CMAKE_TARGETS:
            return
        self._get_cmake_dep_set_of(from_target).add(to_target)
        self.cmake_targets.add(to_target)

    def get_recursive_cmake_deps(self, cmake_target):
        result = set()
        visited = set()

        def walk(cur_target, add_this_target=True):
            if cur_target in visited:
                return
            visited.add(cur_target)
            if add_this_target:
                result.add(cur_target)
            for dep in self.cmake_deps.get(cur_target, []):
                walk(dep)

        walk(cmake_target, add_this_target=False)
        return result


class DependencyGraphBuilder:
    """
    Builds a dependency graph from the contents of the build directory. Each node of the graph is
    a file (an executable, a dynamic library, or a source file).
    """
    def __init__(self, conf):
        self.conf = conf
        self.compile_dirs = set()
        self.compile_commands = None
        self.useful_base_dirs = set()
        self.unresolvable_rel_paths = set()
        self.resolved_rel_paths = {}
        self.dep_graph = DependencyGraph(conf)
        self.cmake_dep_graph = None

    def load_cmake_deps(self):
        self.cmake_dep_graph = CMakeDepGraph(self.conf.build_root)

    def parse_link_and_depend_files_for_make(self):
        logging.info(
                "Parsing link.txt and depend.make files from the build tree at '{}'".format(
                    self.conf.build_root))
        start_time = datetime.now()

        num_parsed = 0
        for root, dirs, files in os.walk(self.conf.build_root):
            for file_name in files:
                file_path = os.path.join(root, file_name)
                if file_name == 'depend.make':
                    self.parse_depend_file(file_path)
                    num_parsed += 1
                elif file_name == 'link.txt':
                    self.parse_link_txt_file(file_path)
                    num_parsed += 1
            if num_parsed % 10 == 0:
                print('.', end='', file=sys.stderr)
                sys.stderr.flush()
        print('', file=sys.stderr)
        sys.stderr.flush()

        logging.info("Parsed link.txt and depend.make files in %.2f seconds" %
                     (datetime.now() - start_time).total_seconds())

    def find_proto_files(self):
        for src_subtree_root in self.conf.src_dir_paths:
            logging.info("Finding .proto files in the source tree at '{}'".format(src_subtree_root))
            source_str = 'proto files in {}'.format(src_subtree_root)
            for root, dirs, files in os.walk(src_subtree_root):
                for file_name in files:
                    if file_name.endswith('.proto'):
                        self.dep_graph.find_or_create_node(
                                os.path.join(root, file_name),
                                source_str=source_str)

    def find_flex_bison_files(self):
        """
        CMake commands generally include the C file compilation, but misses the case where flex
        or bison generates those files, somewhat similiar to .proto files.
        Only examining src/yb tree.
        """
        src_yb_root = os.path.join(self.conf.src_dir_path, 'yb')
        logging.info("Finding .y and .l files in the source tree at '%s'", src_yb_root)
        source_str = 'flex and bison files in {}'.format(src_yb_root)
        for root, dirs, files in os.walk(src_yb_root):
            for file_name in files:
                if file_name.endswith('.y') or file_name.endswith('.l'):
                    rel_path = os.path.relpath(root, self.conf.yb_src_root)
                    assert not rel_path.startswith('../')
                    dependent_file = os.path.join(self.conf.build_root,
                                                  rel_path,
                                                  file_name[:len(file_name)] + '.cc')
                    self.register_dependency(dependent_file,
                                             os.path.join(root, file_name),
                                             source_str)

    def match_cmake_targets_with_files(self):
        logging.info("Matching CMake targets with the files found")
        self.cmake_target_to_nodes = {}
        for node in self.dep_graph.get_nodes():
            node_cmake_target = node.get_cmake_target()
            if node_cmake_target:
                node_set = self.cmake_target_to_nodes.get(node_cmake_target)
                if not node_set:
                    node_set = set()
                    self.cmake_target_to_nodes[node_cmake_target] = node_set
                node_set.add(node)

        self.cmake_target_to_node = {}
        unmatched_cmake_targets = set()
        cmake_dep_graph = self.dep_graph.get_cmake_dep_graph()
        for cmake_target in cmake_dep_graph.cmake_targets:
            nodes = self.cmake_target_to_nodes.get(cmake_target)
            if not nodes:
                unmatched_cmake_targets.add(cmake_target)
                continue

            if len(nodes) > 1:
                raise RuntimeError("Ambiguous nodes found for CMake target '{}': {}".format(
                    cmake_target, nodes))
            self.cmake_target_to_node[cmake_target] = list(nodes)[0]

        if unmatched_cmake_targets:
            logging.warn(
                    "These CMake targets do not have any associated files: %s",
                    sorted(unmatched_cmake_targets))

        # We're not adding nodes into our graph for CMake targets. Instead, we're finding files
        # that correspond to CMake targets, and add dependencies to those files.
        for cmake_target, cmake_target_deps in cmake_dep_graph.cmake_deps.items():
            if cmake_target in unmatched_cmake_targets:
                continue
            node = self.cmake_target_to_node[cmake_target]
            for cmake_target_dep in cmake_target_deps:
                if cmake_target_dep in unmatched_cmake_targets:
                    continue
                node.add_dependency(self.cmake_target_to_node[cmake_target_dep])

    def resolve_rel_path(self, rel_path):
        if is_abs_path(rel_path):
            return rel_path

        if rel_path in self.unresolvable_rel_paths:
            return None
        existing_resolution = self.resolved_rel_paths.get(rel_path)
        if existing_resolution:
            return existing_resolution

        candidates = set()
        for base_dir in self.conf.rel_path_base_dirs:
            candidate_path = os.path.abspath(os.path.join(base_dir, rel_path))
            if os.path.exists(candidate_path):
                self.useful_base_dirs.add(base_dir)
                candidates.add(candidate_path)
        if not candidates:
            self.unresolvable_rel_paths.add(rel_path)
            return None
        if len(candidates) > 1:
            logging.warning("Ambiguous ways to resolve '{}': '{}'".format(
                rel_path, set_to_str(candidates)))
            self.unresolvable_rel_paths.add(rel_path)
            return None

        resolved = list(candidates)[0]
        self.resolved_rel_paths[rel_path] = resolved
        return resolved

    def resolve_dependent_rel_path(self, rel_path):
        if is_abs_path(rel_path):
            return rel_path
        if is_object_file(rel_path):
            return os.path.join(self.conf.build_root, rel_path)
        raise RuntimeError(
            "Don't know how to resolve relative path of a 'dependent': {}".format(
                rel_path))

    def parse_ninja_metadata(self):
        with WorkDirContext(self.conf.build_root):
            ninja_path = os.environ.get('YB_NINJA_PATH', 'ninja')
            logging.info("Ninja executable path: %s", ninja_path)
            logging.info("Running 'ninja -t commands'")
            subprocess.check_call('{} -t commands >ninja_commands.txt'.format(
                pipes.quote(ninja_path)), shell=True)
            logging.info("Parsing the output of 'ninja -t commands' for linker commands")
            self.parse_link_txt_file('ninja_commands.txt')

            logging.info("Running 'ninja -t deps'")
            subprocess.check_call('{} -t deps >ninja_deps.txt'.format(
                pipes.quote(ninja_path)), shell=True)
            logging.info("Parsing the output of 'ninja -t deps' to infer dependencies")
            self.parse_depend_file('ninja_deps.txt')

    def register_dependency(self, dependent, dependency, dependency_came_from):
        dependent = self.resolve_dependent_rel_path(dependent.strip())
        dependency = dependency.strip()
        dependency = self.resolve_rel_path(dependency)
        if dependency:
            dependent_node = self.dep_graph.find_or_create_node(
                    dependent, source_str=dependency_came_from)
            dependency_node = self.dep_graph.find_or_create_node(
                    dependency, source_str=dependency_came_from)
            dependent_node.add_dependency(dependency_node)

    def parse_depend_file(self, depend_make_path):
        """
        Parse either a depend.make file from a CMake-generated Unix Makefile project (fully built)
        or the output of "ninja -t deps" in a Ninja project.
        """
        with open(depend_make_path) as depend_file:
            dependent = None
            for line in depend_file:
                line_orig = line
                line = line.strip()
                if not line or line.startswith('#'):
                    continue

                if ': #' in line:
                    # This is a top-level line from "ninja -t deps".
                    dependent = line.split(': #')[0]
                elif line_orig.startswith(' ' * 4) and not line_orig.startswith(' ' * 5):
                    # This is a line listing a particular dependency from "ninja -t deps".
                    dependency = line
                    if not dependent:
                        raise ValueError("Error parsing %s: no dependent found for dependency %s" %
                                         (depend_make_path, dependency))
                    self.register_dependency(dependent, dependency, depend_make_path)
                elif ': ' in line:
                    # This is a line from depend.make. It has exactly one dependency on the right
                    # side.
                    dependent, dependency = line.split(':')
                    self.register_dependency(dependent, dependency, depend_make_path)
                else:
                    raise ValueError("Could not parse this line from %s:\n%s" %
                                     (depend_make_path, line_orig))

    def find_node_by_rel_path(self, rel_path):
        if is_abs_path(rel_path):
            return self.find_node(rel_path, must_exist=True)
        candidates = []
        for path, node in self.node_by_path.items():
            if path.endswith('/' + rel_path):
                candidates.append(node)
        if not candidates:
            raise RuntimeError("Could not find node by relative path '{}'".format(rel_path))
        if len(candidates) > 1:
            raise RuntimeError("Ambiguous nodes for relative path '{}'".format(rel_path))
        return candidates[0]

    def parse_link_txt_file(self, link_txt_path):
        with open(link_txt_path) as link_txt_file:
            for line in link_txt_file:
                line = line.strip()
                if line:
                    self.parse_link_command(line, link_txt_path)

    def parse_link_command(self, link_command, link_txt_path):
        link_args = link_command.split()
        output_path = None
        inputs = []
        if self.conf.is_ninja:
            base_dir = self.conf.build_root
        else:
            base_dir = os.path.join(os.path.dirname(link_txt_path), '..', '..')
        i = 0
        compilation = False
        while i < len(link_args):
            arg = link_args[i]
            if arg == '-o':
                new_output_path = link_args[i + 1]
                if output_path and new_output_path and output_path != new_output_path:
                    raise RuntimeError(
                        "Multiple output paths for a link command ('{}' and '{}'): {}".format(
                            output_path, new_output_path, link_command))
                output_path = new_output_path
                if self.conf.is_ninja and is_object_file(output_path):
                    compilation = True
                i += 1
            elif not arg.startswith('@rpath/'):
                if is_object_file(arg):
                    node = self.dep_graph.find_or_create_node(
                            os.path.abspath(os.path.join(base_dir, arg)))
                    inputs.append(node.path)

                if ends_with_one_of(arg, LIBRARY_FILE_EXTENSIONS) and not arg.startswith('-'):
                    node = self.dep_graph.find_or_create_node(
                            os.path.abspath(os.path.join(base_dir, arg)),
                            source_str=link_txt_path)
                    inputs.append(node.path)

            i += 1

        if self.conf.is_ninja and compilation:
            # Ignore compilation commands. We are interested in linking commands only at this point.
            return

        if not output_path:
            if self.conf.is_ninja:
                return
            raise RuntimeError("Could not find output path for link command: %s" % link_command)

        if not is_abs_path(output_path):
            output_path = os.path.abspath(os.path.join(base_dir, output_path))
        output_node = self.dep_graph.find_or_create_node(output_path, source_str=link_txt_path)
        output_node.validate_existence()
        for input_node in inputs:
            dependency_node = self.dep_graph.find_or_create_node(input_node)
            if output_node is dependency_node:
                raise RuntimeError(
                    ("Cannot add a dependency from a node to itself: %s. "
                     "Parsed from command: %s") % (output_node, link_command))
            output_node.add_dependency(dependency_node)

    def build(self):
        compile_commands_path = os.path.join(self.conf.build_root, 'compile_commands.json')
        cmake_deps_path = os.path.join(self.conf.build_root, 'yb_cmake_deps.txt')
        if not os.path.exists(compile_commands_path) or not os.path.exists(cmake_deps_path):

            # This is mostly useful during testing. We don't want to generate the list of compile
            # commands by default because it takes a while, so only generate it on demand.
            os.environ['YB_EXPORT_COMPILE_COMMANDS'] = '1'
            mkdir_p(self.conf.build_root)

            subprocess.check_call(
                    [os.path.join(self.conf.yb_src_root, 'yb_build.sh'),
                     self.conf.build_type,
                     '--cmake-only',
                     '--no-rebuild-thirdparty',
                     '--build-root', self.conf.build_root])

        logging.info("Loading compile commands from '{}'".format(compile_commands_path))
        with open(compile_commands_path) as commands_file:
            self.compile_commands = json.load(commands_file)

        for entry in self.compile_commands:
            self.compile_dirs.add(entry['directory'])

        if self.conf.is_ninja:
            self.parse_ninja_metadata()
        else:
            self.parse_link_and_depend_files_for_make()
        self.find_proto_files()
        self.find_flex_bison_files()
        self.dep_graph.validate_node_existence()

        self.load_cmake_deps()
        self.match_cmake_targets_with_files()
        self.dep_graph._add_proto_generation_deps()

        return self.dep_graph


class DependencyGraph:

    canonicalization_cache = {}

    def __init__(self, conf, json_data=None):
        """
        @param json_data optional results of JSON parsing
        """
        self.conf = conf
        self.node_by_path = {}
        if json_data:
            self.init_from_json(json_data)
        self.nodes_by_basename = None
        self.cmake_dep_graph = None

    def get_cmake_dep_graph(self):
        if self.cmake_dep_graph:
            return self.cmake_dep_graph

        cmake_dep_graph = CMakeDepGraph(self.conf.build_root)
        for node in self.get_nodes():
            proto_lib = node.get_containing_proto_lib()
            if proto_lib and self.conf.verbose:
                logging.info("node: %s, proto lib: %s", node, proto_lib)

        self.cmake_dep_graph = cmake_dep_graph
        return cmake_dep_graph

    def find_node(self, path, must_exist=True, source_str=None):
        assert source_str is None or not must_exist
        path = os.path.abspath(path)
        node = self.node_by_path.get(path)
        if node:
            return node
        if must_exist:
            raise RuntimeError(
                    ("Node not found by path: '{}' (expected to already have this node in our "
                     "graph, not adding).").format(path))
        node = Node(path, self, source_str)
        self.node_by_path[path] = node
        return node

    def find_or_create_node(self, path, source_str=None):
        """
        Finds a node with the given path or creates it if it does not exist.
        @param source_str a string description of how we came up with this node's path
        """

        canonical_path = self.canonicalization_cache.get(path)
        if not canonical_path:
            canonical_path = os.path.realpath(path)
            if canonical_path.startswith(self.conf.build_root + '/'):
                canonical_path = self.conf.build_root + '/' + \
                                 canonical_path[len(self.conf.build_root) + 1:]
            self.canonicalization_cache[path] = canonical_path

        return self.find_node(canonical_path, must_exist=False, source_str=source_str)

    def init_from_json(self, json_nodes):
        id_to_node = {}
        id_to_dep_ids = {}
        for node_json in json_nodes:
            node_id = node_json['id']
            id_to_node[node_id] = self.find_or_create_node(node_json['path'])
            id_to_dep_ids[node_id] = node_json['deps']
        for node_id, dep_ids in id_to_dep_ids.items():
            node = id_to_node[node_id]
            for dep_id in dep_ids:
                node.add_dependency(id_to_node[dep_id])

    def find_nodes_by_regex(self, regex_str):
        filter_re = re.compile(regex_str)
        return [node for node in self.get_nodes() if filter_re.match(node.path)]

    def find_nodes_by_basename(self, basename):
        if not self.nodes_by_basename:
            # We are lazily initializing the basename -> node map, and any changes to the graph
            # after this point will not get reflected in it. This is OK as we're only using this
            # function after the graph has been built.
            self.nodes_by_basename = group_by(
                    self.get_nodes(),
                    lambda node: os.path.basename(node.path))
        return self.nodes_by_basename.get(basename)

    def find_affected_nodes(self, start_nodes, requested_node_type=NODE_TYPE_ANY):
        if self.conf.verbose:
            logging.info("Starting with the following initial nodes:")
            for node in start_nodes:
                logging.info("    {}".format(node))

        results = set()
        visited = set()

        def dfs(node):
            if ((requested_node_type == NODE_TYPE_ANY or node.node_type == requested_node_type) and
                    node not in start_nodes):
                results.add(node)
            if node in visited:
                return
            visited.add(node)
            for dep in node.reverse_deps:
                dfs(dep)

        for node in start_nodes:
            dfs(node)

        return results

    def affected_basenames_by_basename_for_test(self, basename, node_type=NODE_TYPE_ANY):
        nodes_for_basename = self.find_nodes_by_basename(basename)
        if not nodes_for_basename:
            self.dump_debug_info()
            raise RuntimeError(
                    "No nodes found for file name '{}' (total number of nodes: {})".format(
                        basename, len(self.node_by_path)))
        return set([os.path.basename(node.path)
                    for node in self.find_affected_nodes(nodes_for_basename, node_type)])

    def save_as_json(self, output_path):
        """
        Converts the dependency graph into a JSON representation, where every node is given an id,
        so that dependencies are represented concisely.
        """
        with open(output_path, 'w') as output_file:
            next_node_id = [1]  # Use a mutable object so we can modify it from closure.
            path_to_id = {}
            output_file.write("[")

            def get_node_id(node):
                node_id = path_to_id.get(node.path)
                if not node_id:
                    node_id = next_node_id[0]
                    path_to_id[node.path] = node_id
                    next_node_id[0] = node_id + 1
                return node_id

            is_first = True
            for node_path, node in self.node_by_path.items():
                node_json = dict(
                    id=get_node_id(node),
                    path=node_path,
                    deps=[get_node_id(dep) for dep in node.deps]
                    )
                if not is_first:
                    output_file.write(",\n")
                is_first = False
                output_file.write(json.dumps(node_json))
            output_file.write("\n]\n")

        logging.info("Saved dependency graph to '{}'".format(output_path))

    def validate_node_existence(self):
        logging.info("Validating existence of build artifacts")
        for node in self.get_nodes():
            node.validate_existence()

    def get_nodes(self):
        return self.node_by_path.values()

    def dump_debug_info(self):
        logging.info("Dumping all graph nodes for debugging ({} nodes):".format(
            len(self.node_by_path)))
        for node in sorted(self.get_nodes(), key=lambda node: str(node)):
            logging.info(node)

    def _add_proto_generation_deps(self):
        """
        Add dependencies of .pb.{h,cc} files on the corresponding .proto file. We do that by
        finding .proto and .pb.{h,cc} nodes in the graph independently and matching them
        based on their path relative to the source root.

        Additionally, we are inferring dependencies between binaries (protobuf libs or in some
        cases other libraries or even tests) that use a .pb.cc file on the CMake target that
        generates these files (e.g. gen_src_yb_rocksdb_db_version_edit_proto). We add these
        inferred dependencies to the separate CMake dependency graph.
        """
        proto_node_by_rel_path = {}
        pb_h_cc_nodes_by_rel_path = {}

        cmake_dep_graph = self.get_cmake_dep_graph()

        for node in self.get_nodes():
            basename = os.path.basename(node.path)
            if node.path.endswith('.proto'):
                proto_rel_path = node.path_rel_to_src_root()
                if proto_rel_path:
                    proto_rel_path = proto_rel_path[:-6]
                    if proto_rel_path.startswith('ent/'):
                        # Remove the 'ent/' prefix because there is no separate 'ent/' prefix
                        # in the build directory.
                        proto_rel_path = proto_rel_path[4:]
                    if proto_rel_path in proto_node_by_rel_path:
                        raise RuntimeError(
                            "Multiple .proto nodes found that share the same "
                            "relative path to source root: %s and %s" % (
                                proto_node_by_rel_path[proto_rel_path], node
                            ))

                    proto_node_by_rel_path[proto_rel_path] = node
            else:
                match = PROTO_OUTPUT_FILE_NAME_RE.match(basename)
                if match:
                    proto_output_rel_path = node.path_rel_to_build_root()
                    if proto_output_rel_path:
                        append_to_list_in_dict(
                            pb_h_cc_nodes_by_rel_path,
                            os.path.join(
                                os.path.dirname(proto_output_rel_path),
                                match.group(1)
                            ),
                            node)
                        if node.path.endswith('.pb.cc'):
                            proto_gen_cmake_target = node.get_proto_gen_cmake_target()
                            for containing_binary in node.get_containing_binaries():
                                containing_cmake_target = containing_binary.get_cmake_target()
                                proto_gen_cmake_target = node.get_proto_gen_cmake_target()
                                self.cmake_dep_graph.add_dependency(
                                    containing_cmake_target,
                                    proto_gen_cmake_target
                                )

        for rel_path in proto_node_by_rel_path:
            if rel_path not in pb_h_cc_nodes_by_rel_path:
                raise ValueError(
                    "For relative path %s, found a proto file (%s) but no .pb.{h,cc} files" %
                    (rel_path, proto_node_by_rel_path[rel_path]))

        for rel_path in pb_h_cc_nodes_by_rel_path:
            if rel_path not in proto_node_by_rel_path:
                raise ValueError(
                    "For relative path %s, found .pb.{h,cc} files (%s) but no .proto" %
                    (rel_path, pb_h_cc_nodes_by_rel_path[rel_path]))

        # This is what we've verified above in two directions separately.
        assert(set(proto_node_by_rel_path.keys()) == set(pb_h_cc_nodes_by_rel_path.keys()))

        for rel_path in proto_node_by_rel_path.keys():
            proto_node = proto_node_by_rel_path[rel_path]
            # .pb.{h,cc} files need to depend on the .proto file they were generated from.
            for pb_h_cc_node in pb_h_cc_nodes_by_rel_path[rel_path]:
                pb_h_cc_node.add_dependency(proto_node)

    def _check_for_circular_dependencies(self):
        logging.info("Checking for circular dependencies")
        visited = set()
        stack = []

        def walk(node):
            if node in visited:
                return
            try:
                stack.append(node)
                visited.add(node)
                for dep in node.deps:
                    if dep in visited:
                        if dep in stack:
                            raise RuntimeError("Circular dependency loop found: %s", stack)
                        return
                    walk(dep)
            finally:
                stack.pop()

        for node in self.get_nodes():
            walk(node)

        logging.info("No circular dependencies found -- this is good (visited %d nodes)",
                     len(visited))

    def validate_proto_deps(self):
        """
        Make sure that code that depends on protobuf-generated files also depends on the
        corresponding protobuf library target.
        """

        logging.info("Validating dependencies on protobuf-generated headers")

        # TODO: only do this during graph generation.
        self._add_proto_generation_deps()
        self._check_for_circular_dependencies()

        header_node_map = {}
        lib_node_map = {}

        # For any .pb.h file, we want to find all .cc.o files that depend on it, meaning that they
        # directly or indirectly include that .pb.h file.
        #
        # Then we want to look at the containing executable or library of the .cc.o file and make
        # sure it has a CMake dependency (direct or indirect) on the protobuf generation target of
        # the form gen_src_yb_common_redis_protocol_proto.
        #
        # However, we also need to get the CMake target name corresponding to the binary
        # containing the .cc.o file.

        proto_dep_errors = []

        for node in self.get_nodes():
            if not node.path.endswith('.pb.cc.o'):
                continue
            source_deps = [dep for dep in node.deps if dep.path.endswith('.cc')]
            if len(source_deps) != 1:
                raise ValueError(
                    "Could not identify a single source dependency of node %s. Found: %s. " %
                    (node, source_deps))
            source_dep = source_deps[0]
            pb_h_path = source_dep.path[:-3] + '.h'
            pb_h_node = self.node_by_path[pb_h_path]
            proto_gen_target = pb_h_node.get_proto_gen_cmake_target()

            for rev_dep in pb_h_node.reverse_deps:
                if rev_dep.path.endswith('.cc.o'):
                    for binary in rev_dep.get_containing_binaries():
                        binary_cmake_target = binary.get_cmake_target()

                        recursive_cmake_deps = self.get_cmake_dep_graph().get_recursive_cmake_deps(
                            binary_cmake_target)
                        if proto_gen_target not in recursive_cmake_deps:
                            proto_dep_errors.append(
                                "CMake target %s does not depend directly or indirectly on target "
                                "%s but uses the header file %s. Recursive cmake deps of %s: %s" %
                                (binary_cmake_target, proto_gen_target, pb_h_path,
                                 binary_cmake_target, recursive_cmake_deps)
                            )
        if proto_dep_errors:
            for error_msg in proto_dep_errors:
                logging.error("Protobuf dependency error: %s", error_msg)
            raise RuntimeError(
                "Found targets that use protobuf-generated header files but do not declare the "
                "dependency explicitly. See the log messages above.")


class DependencyGraphTest(unittest.TestCase):
    dep_graph = None

    # Basename -> basenames affected by it.
    affected_basenames_cache = {}

    def get_affected_basenames(self, initial_basename):
        affected_basenames = self.affected_basenames_cache.get(initial_basename)
        if not affected_basenames:
            affected_basenames = self.dep_graph.affected_basenames_by_basename_for_test(
                    initial_basename)
            self.affected_basenames_cache[initial_basename] = affected_basenames
            if self.dep_graph.conf.verbose:
                # This is useful to get inspiration for new tests.
                logging.info("Files affected by {}:\n    {}".format(
                    initial_basename, "\n    ".join(sorted(affected_basenames))))
        return affected_basenames

    def assert_affected_by(self, expected_affected_basenames, initial_basename):
        """
        Asserts that all given files are affected by the given file. Other files might also be
        affected and that's OK.
        """
        affected_basenames = self.get_affected_basenames(initial_basename)
        remaining_basenames = make_set(expected_affected_basenames) - set(affected_basenames)
        if remaining_basenames:
            self.assertFalse(
                "Expected files {} to be affected by {}, but they were not".format(
                    sorted(remaining_basenames),
                    initial_basename))

    def assert_unaffected_by(self, unaffected_basenames, initial_basename):
        """
        Asserts that the given files are unaffected by the given file.
        """
        affected_basenames = self.get_affected_basenames(initial_basename)
        incorrectly_affected = make_set(unaffected_basenames) & affected_basenames
        if incorrectly_affected:
            self.assertFalse(
                    ("Expected files {} to be unaffected by {}, but they are. Other affected "
                     "files: {}").format(
                         sorted(incorrectly_affected),
                         initial_basename,
                         sorted(affected_basenames - incorrectly_affected)))

    def assert_affected_exactly_by(self, expected_affected_basenames, initial_basename):
        """
        Checks the exact set of files affected by the given file.
        """
        affected_basenames = self.get_affected_basenames(initial_basename)
        self.assertEquals(make_set(expected_affected_basenames), affected_basenames)

    def test_master_main(self):
        self.assert_affected_by([
                'libintegration-tests' + DYLIB_SUFFIX,
                'yb-master'
            ], 'master_main.cc')

    def test_tablet_server_main(self):
        self.assert_affected_by([
                'libintegration-tests' + DYLIB_SUFFIX,
                'linked_list-test'
            ], 'tablet_server_main.cc')

        self.assert_unaffected_by(['yb-master'], 'tablet_server_main.cc')

    def test_bulk_load_tool(self):
        self.assert_affected_exactly_by([
                'yb-bulk_load',
                'yb-bulk_load-test',
                'yb-bulk_load.cc.o'
            ], 'yb-bulk_load.cc')

    def test_flex_bison(self):
        self.assert_affected_by([
                'scanner_lex.l.cc'
            ], 'scanner_lex.l')
        self.assert_affected_by([
                'parser_gram.y.cc'
            ], 'parser_gram.y')

    def test_proto_deps_validity(self):
        self.dep_graph.validate_proto_deps()


def run_self_test(dep_graph):
    logging.info("Running a self-test of the {} tool".format(os.path.basename(__file__)))
    DependencyGraphTest.dep_graph = dep_graph
    suite = unittest.TestLoader().loadTestsFromTestCase(DependencyGraphTest)
    runner = unittest.TextTestRunner()
    result = runner.run(suite)
    if result.errors or result.failures:
        logging.info("Self-test of the dependency graph traversal tool failed!")
        sys.exit(1)


def get_file_category(rel_path):
    """
    Categorize file changes so that we can decide what tests to run.

    @param rel_path: path relative to the source root (not to the build root)

    >>> get_file_category('src/postgres/src/backend/executor/execScan.c')
    'postgres'
    """
    basename = os.path.basename(rel_path)

    if rel_path.split(os.sep)[0] in DIRECTORIES_THAT_DO_NOT_AFFECT_TESTS:
        return CATEGORY_DOES_NOT_AFFECT_TESTS

    if rel_path == 'yb_build.sh':
        # The main build script is being run anyway, so we hope that most issues will come out at
        # that stage.
        return CATEGORY_DOES_NOT_AFFECT_TESTS

    if basename == 'CMakeLists.txt' or basename.endswith('.cmake'):
        return 'cmake'

    if rel_path.startswith('src/postgres'):
        return 'postgres'

    if rel_path.startswith('src/') or rel_path.startswith('ent/src/'):
        return 'c++'

    if rel_path.startswith('python/'):
        return 'python'

    # Top-level subdirectories that map one-to-one to categories.
    for category_dir in ['java', 'thirdparty']:
        if rel_path.startswith(category_dir + '/'):
            return category_dir

    if rel_path.startswith('build-support/'):
        return 'build_scripts'
    return 'other'


def main():
    parser = argparse.ArgumentParser(
        description='A tool for working with the dependency graph')
    parser.add_argument('--verbose', action='store_true',
                        help='Enable debug output')
    parser.add_argument('-r', '--rebuild-graph',
                        action='store_true',
                        help='Rebuild the dependecy graph and save it to a file')
    parser.add_argument('--node-type',
                        help='Node type to look for',
                        default='any',
                        choices=['test', 'object', 'library', 'source', 'any'])
    parser.add_argument('--file-regex',
                        help='Regular expression for file names to select as initial nodes for '
                             'querying the dependency graph.')
    parser.add_argument('--file-name-glob',
                        help='Like file-regex, but applies only to file name and uses the glob '
                             'syntax instead of regex.')
    parser.add_argument('--git-diff',
                        help='Figure out the list of files to use as starting points in the '
                             'dependency graph traversal by diffing the current state of the code '
                             'against this commit. This could also be anything that could be '
                             'passed to "git diff" as a single argument.')
    parser.add_argument('--git-commit',
                        help='Similar to --git-diff, but takes a git commit ref (e.g. sha1 or '
                             'branch) and uses the set of files from that commit.')
    parser.add_argument('--build-root',
                        required=True,
                        help='E.g. <some_root>/build/debug-gcc-dynamic-community')
    parser.add_argument('command',
                        choices=COMMANDS,
                        help='Command to perform')
    parser.add_argument('--output-test-config',
                        help='Output a "test configuration file", which is a JSON containing the '
                             'resulting list of C++ tests to run to this file, a flag indicating '
                             'wheter to run Java tests or not, etc.')
    parser.add_argument('--incomplete-build',
                        action='store_true',
                        help='Skip checking for file existence. Allows using the tool after '
                             'build artifacts have been deleted.')
    args = parser.parse_args()

    if args.file_regex and args.file_name_glob:
        raise RuntimeError('--file-regex and --file-name-glob are incompatible')

    cmd = args.command
    if (not args.file_regex and
            not args.file_name_glob and
            not args.rebuild_graph and
            not args.git_diff and
            not args.git_commit and
            cmd not in COMMANDS_NOT_NEEDING_TARGET_SET):
        raise RuntimeError(
                "Neither of --file-regex, --file-name-glob, --git-{diff,commit}, or "
                "--rebuild-graph are specified, and the command is not one of: " +
                ", ".join(COMMANDS_NOT_NEEDING_TARGET_SET))

    log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="[%(filename)s:%(lineno)d] %(asctime)s %(levelname)s: %(message)s")

    conf = Configuration(args)
    if conf.file_regex and args.git_diff:
        raise RuntimeError(
                "--git-diff is incompatible with --file-{regex,name-glob}")

    if args.git_diff and args.git_commit:
        raise RuntimeError('--git-diff and --git-commit are incompatible')

    if args.git_commit:
        args.git_diff = "{}^..{}".format(args.git_commit, args.git_commit)

    graph_cache_path = os.path.join(args.build_root, 'dependency_graph.json')
    if args.rebuild_graph or not os.path.isfile(graph_cache_path):
        logging.info("Generating a dependency graph at '{}'".format(graph_cache_path))
        dep_graph_builder = DependencyGraphBuilder(conf)
        dep_graph = dep_graph_builder.build()
        dep_graph.save_as_json(graph_cache_path)
    else:
        start_time = datetime.now()
        with open(graph_cache_path) as graph_input_file:
            dep_graph = DependencyGraph(conf, json_data=json.load(graph_input_file))
        logging.info("Loaded dependency graph from '%s' in %.2f sec" %
                     (graph_cache_path, (datetime.now() - start_time).total_seconds()))
        dep_graph.validate_node_existence()

    # ---------------------------------------------------------------------------------------------
    # Commands that do not require an "initial set of targets"
    # ---------------------------------------------------------------------------------------------

    if cmd == SELF_TEST_CMD:
        run_self_test(dep_graph)
        return
    elif cmd == DEBUG_DUMP_CMD:
        dep_graph.dump_debug_info()
        return

    # ---------------------------------------------------------------------------------------------
    # Figure out the initial set of targets based on a git commit, a regex, etc.
    # ---------------------------------------------------------------------------------------------

    updated_categories = set()
    file_changes = []
    if args.git_diff:
        old_working_dir = os.getcwd()
        with WorkDirContext(conf.yb_src_root):
            git_diff_output = subprocess.check_output(
                    ['git', 'diff', args.git_diff, '--name-only']).decode('utf-8')

            initial_nodes = set()
            file_paths = set()
            for file_path in git_diff_output.split("\n"):
                file_path = file_path.strip()
                if not file_path:
                    continue
                file_changes.append(file_path)
                # It is important that we invoke os.path.realpath with the current directory set to
                # the git repository root.
                file_path = os.path.realpath(file_path)
                file_paths.add(file_path)
                node = dep_graph.node_by_path.get(file_path)
                if node:
                    initial_nodes.add(node)

        if not initial_nodes:
            logging.warning("Did not find any graph nodes for this set of files: %s", file_paths)
            for basename in set([os.path.basename(file_path) for file_path in file_paths]):
                logging.warning("Nodes for basename '{}': {}".format(
                    basename, dep_graph.find_nodes_by_basename(basename)))

    elif conf.file_regex:
        logging.info("Using file name regex: {}".format(conf.file_regex))
        initial_nodes = dep_graph.find_nodes_by_regex(conf.file_regex)
        if not initial_nodes:
            logging.warning("Did not find any graph nodes for this pattern: %s", conf.file_regex)
        for node in initial_nodes:
            file_changes.append(node.path)
    else:
        raise RuntimeError("Could not figure out how to generate the initial set of files")

    file_changes_by_category = group_by(file_changes, get_file_category)
    for category, changes in file_changes_by_category.items():
        logging.info("File changes in category '%s':", category)
        for change in sorted(changes):
            logging.info("    %s", change)
    updated_categories = set(file_changes_by_category.keys())

    results = set()
    if cmd == LIST_AFFECTED_CMD:
        results = dep_graph.find_affected_nodes(initial_nodes, args.node_type)
    elif cmd == LIST_DEPS_CMD:
        for node in initial_nodes:
            results.update(node.deps)
    elif cmd == LIST_REVERSE_DEPS_CMD:
        for node in initial_nodes:
            results.update(node.reverse_deps)
    else:
        raise RuntimeError("Unimplemented command '{}'".format(command))

    if args.output_test_config:
        test_basename_list = sorted(
                [os.path.basename(node.path) for node in results if node.node_type == 'test'])
        affected_basenames = set([os.path.basename(node.path) for node in results])

        # These are ALL tests, not just tests affected by the changes in question, used mostly
        # for logging.
        all_test_programs = [node for node in dep_graph.get_nodes() if node.node_type == 'test']
        all_test_basenames = set([os.path.basename(node.path) for node in all_test_programs])

        # A very conservative way to decide whether to run all tests. If there are changes in any
        # categories (meaning the changeset is non-empty), and there are changes in categories other
        # than C++ / Java / files known not to affect unit tests, we force re-running all tests.
        unsafe_categories = updated_categories - CATEGORIES_NOT_CAUSING_RERUN_OF_ALL_TESTS
        user_said_all_tests = get_bool_env_var('YB_RUN_ALL_TESTS')

        test_filter_re = os.getenv('YB_TEST_EXECUTION_FILTER_RE')
        manual_test_filtering_with_regex = bool(test_filter_re)

        select_all_tests_for_now = (
            bool(unsafe_categories) or user_said_all_tests or manual_test_filtering_with_regex)

        user_said_all_cpp_tests = get_bool_env_var('YB_RUN_ALL_CPP_TESTS')
        user_said_all_java_tests = get_bool_env_var('YB_RUN_ALL_JAVA_TESTS')
        cpp_files_changed = 'c++' in updated_categories
        java_files_changed = 'java' in updated_categories
        yb_master_or_tserver_changed = bool(affected_basenames & set(['yb-master', 'yb-tserver']))

        run_cpp_tests = select_all_tests_for_now or cpp_files_changed or user_said_all_cpp_tests

        run_java_tests = (
            select_all_tests_for_now or java_files_changed or yb_master_or_tserver_changed or
            user_said_all_java_tests)

        if select_all_tests_for_now:
            if user_said_all_tests:
                logging.info("User explicitly specified that all tests should be run")
            elif manual_test_filtering_with_regex:
                logging.info(
                    "YB_TEST_EXECUTION_FILTER_RE specified: %s, will filter tests at a later step",
                    test_filter_re)
            else:
                logging.info(
                    "All tests should be run based on file changes in these categories: {}".format(
                        ', '.join(sorted(unsafe_categories))))
        else:
            if run_cpp_tests:
                if user_said_all_cpp_tests:
                    logging.info("User explicitly specified that all C++ tests should be run")
                else:
                    logging.info('Will run some C++ tests, some C++ files changed')
            if run_java_tests:
                if user_said_all_java_tests:
                    logging.info("User explicitly specified that all Java tests should be run")
                else:
                    logging.info('Will run all Java tests, ' +
                                 ' and '.join(
                                     (['some Java files changed'] if java_files_changed else []) +
                                     (['yb-{master,tserver} binaries changed']
                                      if yb_master_or_tserver_changed else [])))

        if run_cpp_tests and not test_basename_list and not select_all_tests_for_now:
            logging.info('There are no C++ test programs affected by the changes, '
                         'will skip running C++ tests.')
            run_cpp_tests = False

        test_conf = dict(
            run_cpp_tests=run_cpp_tests,
            run_java_tests=run_java_tests,
            file_changes_by_category=file_changes_by_category
        )
        if test_filter_re:
            test_conf.update(test_filter_re=test_filter_re)

        if not select_all_tests_for_now:
            # We only have this kind of fine-grained filtering for C++ test programs, and for Java
            # tests we either run all of them or none.
            test_conf['cpp_test_programs'] = test_basename_list
            logging.info(
                    "{} C++ test programs should be run (out of {} possible, {}%)".format(
                        len(test_basename_list),
                        len(all_test_basenames),
                        "%.1f" % (100.0 * len(test_basename_list) / len(all_test_basenames))))
            if len(test_basename_list) != len(all_test_basenames):
                logging.info("The following C++ test programs will be run: {}".format(
                    ", ".join(sorted(test_basename_list))))

        with open(args.output_test_config, 'w') as output_file:
            output_file.write(json.dumps(test_conf, indent=2) + "\n")
        logging.info("Wrote a test configuration to {}".format(args.output_test_config))
    else:
        # For ad-hoc command-line use, mostly for testing and sanity-checking.
        for node in sorted(results, key=lambda node: [node.node_type, node.path]):
            print(node)
        logging.info("Found {} results".format(len(results)))


if __name__ == '__main__':
    main()
