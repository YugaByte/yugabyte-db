"""
Copyright (c) YugaByte, Inc.

This module provides common utility functions.
"""

import itertools
import logging
import os
import re
import sys
import json
import subprocess
import shlex


MODULE_DIR = os.path.dirname(os.path.realpath(__file__))
YB_SRC_ROOT = os.path.realpath(os.path.join(MODULE_DIR, '..', '..'))
NINJA_BUILD_ROOT_PART_RE = re.compile(r'-ninja($|(?=-))')
JSON_INDENTATION = 2

# We default this to the env var or to the actual local thirdparty call, but we expose a getter
# and setter below that should be imported and used to access this global.
_YB_THIRDPARTY_DIR = os.path.realpath(
        os.environ.get("YB_THIRDPARTY_DIR", os.path.join(YB_SRC_ROOT, 'thirdparty')))


def get_thirdparty_dir():
    global _YB_THIRDPARTY_DIR
    return _YB_THIRDPARTY_DIR


def set_thirdparty_dir(thirdparty_dir):
    global _YB_THIRDPARTY_DIR
    _YB_THIRDPARTY_DIR = thirdparty_dir
    os.environ["YB_THIRDPARTY_DIR"] = thirdparty_dir


def sorted_grouped_by(arr, key_fn):
    """
    Group the given collection by the key computed using the given function. The collection does not
    have to be already sorted.
    @return a list of (key, list_of_values) tuples where keys are sorted
    """
    return [(k, list(v)) for (k, v) in itertools.groupby(sorted(arr, key=key_fn), key_fn)]


def group_by(arr, key_fn):
    """
    Given a collection and a function that computes a key, returns a map from keys to all values
    with that key.
    """
    return dict(sorted_grouped_by(arr, key_fn))


def make_list(obj):
    """
    Convert the given object to a list. Strings get converted to a list of one string, not to a
    list of their characters.
    """
    if isinstance(obj, str):
        return [str]
    return list(obj)


def make_set(obj):
    return set(make_list(obj))


def init_env(verbose):
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s [%(filename)s:%(lineno)d %(levelname)s] %(message)s")


def get_build_type_from_build_root(build_root):
    return os.path.basename(build_root).split('-')[0]


def get_compiler_type_from_build_root(build_root):
    build_root_basename_components = os.path.basename(build_root).split('-')
    if len(build_root_basename_components) < 2:
        raise ValueError(
                "Too few components in build root basename: %s (build root: %s). "
                "Cannot get compiler type." % (build_root_basename_components, build_root))
    return build_root_basename_components[1]


def set_env_vars_from_build_root(build_root):
    build_root_from_env = os.getenv('BUILD_ROOT')
    if build_root_from_env is not None and build_root_from_env != build_root:
        raise ValueError(
            "The BUILD_ROOT environment variable is %s but the build root is being set to %s" % (
                build_root_from_env, build_root))
    os.environ['BUILD_ROOT'] = build_root

    compiler_type_from_build_root = get_compiler_type_from_build_root(build_root)
    compiler_type_from_env = os.getenv('YB_COMPILER_TYPE')
    if (compiler_type_from_env is not None and
            compiler_type_from_env != compiler_type_from_build_root):
        raise ValueError(
            "The YB_COMPILER_TYPE environment variable is %s but the compiler type derived "
            "from the build root is %s" % (compiler_type_from_env, compiler_type_from_build_root))
    os.environ['YB_COMPILER_TYPE'] = compiler_type_from_build_root


def safe_path_join(*args):
    """Like os.path.join, but allows the first argument to be None."""
    if args[0] is None:
        return None
    return os.path.join(*args)


def set_to_comma_sep_str(items):
    if items:
        return ", ".join(sorted(set(items)))
    return "(empty)"


def convert_to_non_ninja_build_root(build_root):
    """
    >>> convert_to_non_ninja_build_root('/foo/bar-ninja-bar/yugabyte/build/some-build-ninja')
    '/foo/bar-ninja-bar/yugabyte/build/some-build'
    >>> convert_to_non_ninja_build_root('/foo/bar-ninja-bar/yugabyte/build/some-build-ninja-foo')
    '/foo/bar-ninja-bar/yugabyte/build/some-build-foo'
    """
    directory = os.path.dirname(build_root)
    basename = os.path.basename(build_root)
    return os.path.join(directory, NINJA_BUILD_ROOT_PART_RE.sub('', basename))


def is_ninja_build_root(build_root):
    return build_root != convert_to_non_ninja_build_root(build_root)


def get_bool_env_var(env_var_name):
    value = os.environ.get(env_var_name, None)
    if value is None:
        return False

    return value.lower() in ['1', 't', 'true', 'y', 'yes']


def is_yb_src_root(d):
    for subdir in ['.git', 'src', 'java', 'bin', 'build-support']:
        if not os.path.exists(os.path.join(d, subdir)):
            return False
    return True


def get_yb_src_root_from_build_root(build_dir, verbose=False, must_succeed=False):
    """
    Given a build directory, find the YB Git repository's root corresponding to it.
    """
    yb_src_root = None
    current_dir = build_dir
    while current_dir != '/':
        subdir_candidates = [current_dir]
        # Handle the "external build" directory case, in which the code is located in e.g.
        # ~/code/yugabyte, and the build directories are inside ~/code/yugabyte__build.
        if current_dir.endswith('__build'):
            subdir_candidates.append(current_dir[:-7])
            if subdir_candidates[-1].endswith('/'):
                subdir_candidates = subdir_candidates[:-1]

        for git_subdir_candidate in subdir_candidates:
            git_dir_candidate = os.path.join(current_dir, git_subdir_candidate)
            if is_yb_src_root(git_dir_candidate):
                yb_src_root = git_dir_candidate
                break
        if yb_src_root:
            break
        current_dir = os.path.dirname(current_dir)

    if yb_src_root:
        if verbose:
            logging.info(
                    "Found YB git repository root %s corresponding to the build directory %s",
                    yb_src_root, build_dir)
    else:
        error_msg = "Could not find git repository root by walking up from %s" % build_dir
        logging.warn(error_msg)
        if must_succeed:
            raise RuntimeError(error_msg)

    return yb_src_root


def is_macos():
    return sys.platform == 'darwin'


def write_json_file(json_data, output_path, description_for_log=None):
    with open(output_path, 'w') as output_file:
        json.dump(json_data, output_file, indent=JSON_INDENTATION)
        if description_for_log is None:
            logging.info("Wrote %s: %s", description_for_log, output_path)


def read_json_file(input_path):
    try:
        with open(input_path) as input_file:
            return json.load(input_file)
    except:  # noqa: E129
        # We re-throw the exception anyway.
        logging.error("Failed reading JSON file %s", input_path)
        raise


def get_absolute_path_aliases(path):
    """
    Returns a list of different variants (just an absolute path vs. all symlinks resolved) for the
    given path.
    """
    return sorted(set([os.path.abspath(path), os.path.realpath(path)]))


def find_executable(rel_path, must_find=False):
    """
    Similar to the UNIX "which" command.
    """
    if os.path.isabs(rel_path):
        raise ValueError("Expected an absolute path, got: %s", rel_path)
    path_env_var = os.getenv('PATH')
    for search_dir in path_env_var.split(os.path.pathsep):
        joined_path = os.path.join(search_dir, rel_path)
        if os.path.exists(joined_path) and os.access(joined_path, os.X_OK):
            return joined_path
    if must_find:
        raise IOError("Could not find executable %s. PATH: %s" % (rel_path, path_env_var))


def rm_rf(path):
    if path == '/':
        raise ValueError("Cannot remove directory recursively: %s", path)
    if os.path.isabs(path):
        raise ValueError("Absolute path required, got %s", path)

    subprocess.check_call(['rm', '-rf', path])


def shlex_join(args):
    if hasattr(shlex, 'join'):
        return shlex.join(args)
    return ' '.join([shlex.quote(arg) for arg in args])


def check_call_and_log(args):
    cmd_str = shlex_join(args)
    logging.info("Running command: %s", cmd_str)
    try:
        subprocess.check_call(args)
    except subprocess.CalledProcessError as ex:
        logging.exception("Command failed with exit code %d: %s", ex.returncode, cmd_str)
        raise ex


def dict_set_or_del(d, k, v):
    """
    Set the value of the given key in a dictionary to the given value, or delete it if the value
    is None.
    """
    if v is None:
        if k in d:
            del d[k]
    else:
        d[k] = v


class EnvVarContext:
    """
    Sets the given environment variables and restores them on exit. A None value means the variable
    is undefined.
    """
    def __init__(self, **env_vars):
        self.env_vars = env_vars

    def __enter__(self):
        self.saved_env_vars = {}
        for env_var_name, new_value in self.env_vars.items():
            self.saved_env_vars[env_var_name] = os.environ.get(env_var_name)
            dict_set_or_del(os.environ, env_var_name, new_value)

    def __exit__(self, exc_type, exc_val, exc_tb):
        for env_var_name, saved_value in self.saved_env_vars.items():
            dict_set_or_del(os.environ, env_var_name, saved_value)
