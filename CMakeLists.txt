# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# The following only applies to changes made to this file as part of YugaByte development.
#
# Portions Copyright (c) YugaByte, Inc.
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

# Portions Copyright (c) YugaByte, Inc.

# Require cmake that includes FindICU module (https://cmake.org/cmake/help/v3.7/release/3.7.html)
cmake_minimum_required(VERSION 3.7.0)

project(YugabyteDB)

# =================================================================================================
# Functions
# =================================================================================================

function(CHECK_YB_COMPILER_PATH COMPILER_PATH)
  if(NOT "${COMPILER_PATH}" MATCHES "/compiler-wrappers/(cc|c[+][+])$" AND
     NOT "${CMAKE_COMMAND}" MATCHES "/[.]?(CLion|clion)")
    message(
      SEND_ERROR
      "Invalid compiler path: '${COMPILER_PATH}'. Expected to end with one of: "
      "/compiler-wrappers/{cc,c++}. The only exception is for builds invoked from CLion, but "
      "CMAKE_COMMAND ('${CMAKE_COMMAND}') does not contain a substring '/[.]CLion' or '/[.]clion' "
      "(feel free to tweak the pattern in the top-level CMakeLists.txt if it has to be updated "
      "for the most recent version of CLion).")
  endif()
endfunction()

# Determine the number of CPUs to be used so we can call make on existing Makefiles (e.g. RocksDB)
# with the right level of parallelism.  Snippet taken from https://blog.kitware.com/how-many-ya-got/
function(DETECT_NUMBER_OF_PROCESSORS)
  if(NOT DEFINED PROCESSOR_COUNT)
    # Unknown:
    set(PROCESSOR_COUNT 0)

    # Linux:
    set(cpuinfo_file "/proc/cpuinfo")
    if(EXISTS "${cpuinfo_file}")
      file(STRINGS "${cpuinfo_file}" procs REGEX "^processor.: [0-9]+$")
      list(LENGTH procs PROCESSOR_COUNT)
    endif()

    # Mac:
    if(APPLE)
      execute_process(COMMAND /usr/sbin/sysctl -n hw.ncpu OUTPUT_VARIABLE PROCESSOR_COUNT)
      # Strip trailing newline (otherwise it may get into the generated Makefile).
      string(STRIP "${PROCESSOR_COUNT}" PROCESSOR_COUNT)
    endif()

    # Windows:
    if(WIN32)
      set(PROCESSOR_COUNT "$ENV{NUMBER_OF_PROCESSORS}")
    endif()
  endif()

  if (NOT DEFINED PROCESSOR_COUNT OR "${PROCESSOR_COUNT}" STREQUAL "")
    message(FATAL_ERROR "Could not determine the number of logical CPUs")
  endif()
  message("Detected the number of logical CPUs: ${PROCESSOR_COUNT}")
  set(PROCESSOR_COUNT "${PROCESSOR_COUNT}" PARENT_SCOPE)
endfunction()

# Prevent builds from the top-level source directory. This ensures that build output is well
# isolated from the source tree.
function(ENFORCE_OUT_OF_SOURCE_BUILD)
  if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_BINARY_DIR}")
    message(FATAL_ERROR
      "YugaByte may not be built from the top-level source directory. Create a new "
      "directory and run cmake from there, passing the path to the top-level "
      "source directory as the last argument. "
      "To override this, rerun CMake with -DYB_ALLOW_IN_SOURCE_BUILD=0. "
      "Also, delete 'CMakeCache.txt' and 'CMakeFiles' from the top-level source "
      "directory, otherwise future builds will not work.")
  endif()
endfunction()

function(DETECT_BREW)
  # Detect Linuxbrew. The logic needs to be consistent with the detect_linuxbrew function in
  # common-build-env.sh.
  set(USING_LINUXBREW FALSE)
  if(NOT APPLE)
    set(LINUXBREW_DIR "$ENV{YB_LINUXBREW_DIR}")
    if("${LINUXBREW_DIR}" STREQUAL "")
      if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/linuxbrew_path.txt")
        file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/linuxbrew_path.txt" LINUXBREW_DIR)
      else()
        set(LINUXBREW_DIR "$ENV{HOME}/.linuxbrew-yb-build")
      endif()
    endif()
    if(EXISTS "${LINUXBREW_DIR}/bin" AND
       EXISTS "${LINUXBREW_DIR}/lib")
      message("Linuxbrew found at ${LINUXBREW_DIR}")
      set(ENV{YB_LINUXBREW_DIR} "${LINUXBREW_DIR}")
      set(USING_LINUXBREW TRUE)
    else()
      message("Not using Linuxbrew: no valid Linuxbrew installation found at "
              "${LINUXBREW_DIR}")
    endif()
  endif()

  if(NOT USING_LINUXBREW)
    set(LINUXBREW_DIR "/tmp/not-using-linuxbrew")
  endif()

  set(USING_LINUXBREW "${USING_LINUXBREW}" PARENT_SCOPE)
  set(LINUXBREW_DIR "${LINUXBREW_DIR}" PARENT_SCOPE)
  set(LINUXBREW_LIB_DIR "${LINUXBREW_DIR}/lib" PARENT_SCOPE)
endfunction()

# Ensures that the YB_COMPILER_TYPE environment variable matches the auto-detected compiler family.
# Also this sets the convienience variables IS_GCC and IS_CLANG.
function(INIT_COMPILER_TYPE)
  get_filename_component(BUILD_ROOT_BASENAME "${CMAKE_CURRENT_BINARY_DIR}" NAME)

  if ("$ENV{YB_COMPILER_TYPE}" STREQUAL "")
    # TODO: deduplicate this.
    string(REGEX MATCH "^.*-zapcc-.*$" RE_MATCH_RESULT "${BUILD_ROOT_BASENAME}")
    if (NOT "${RE_MATCH_RESULT}" STREQUAL "")
      set(ENV{YB_COMPILER_TYPE} "zapcc")
    else()
      string(REGEX MATCH "^.*-gcc-.*$" RE_MATCH_RESULT "${BUILD_ROOT_BASENAME}")
      if (NOT "${RE_MATCH_RESULT}" STREQUAL "")
        set(ENV{YB_COMPILER_TYPE} "gcc")
      else()
        string(REGEX MATCH "^.*-clang-.*$" RE_MATCH_RESULT "${BUILD_ROOT_BASENAME}")
        if (NOT "${RE_MATCH_RESULT}" STREQUAL "")
          set(ENV{YB_COMPILER_TYPE} "clang")
        endif()
      endif()
    endif()
  endif()

  message("YB_COMPILER_TYPE env var: $ENV{YB_COMPILER_TYPE}")
endfunction()

# Makes sure that we are using a supported compiler family.
function(VALIDATE_COMPILER_TYPE)
  set(USING_SANITIZERS FALSE PARENT_SCOPE)
  if ("${YB_USE_ASAN}" OR "${YB_USE_TSAN}" OR "${YB_USE_UBSAN}")
    if (NOT "$ENV{YB_COMPILER_TYPE}" STREQUAL "" AND
        NOT "$ENV{YB_COMPILER_TYPE}" STREQUAL "clang")
      message(FATAL_ERROR
              "YB_COMPILER_TYPE is set to '$ENV{YB_COMPILER_TYPE}', but it must be 'clang' for "
              "ASAN/TSAN/UBSAN builds. "
              "YB_USE_ASAN=${YB_USE_ASAN}, "
              "YB_USE_TSAN=${YB_USE_TSAN}, "
              "YB_USE_UBSAN=${YB_USE_UBSAN}")
    endif()
    set(ENV{YB_COMPILER_TYPE} "clang")
    set(USING_SANITIZERS TRUE PARENT_SCOPE)
  endif()

  if ("$ENV{YB_COMPILER_TYPE}" STREQUAL "")
    set(ENV{YB_COMPILER_TYPE} "${COMPILER_FAMILY}")
  endif()

  if ("$ENV{YB_COMPILER_TYPE}" STREQUAL "zapcc")
    if (NOT "${COMPILER_FAMILY}" STREQUAL "clang")
      message(FATAL_ERROR
              "Compiler type is zapcc but the compiler family is '${COMPILER_FAMILY}' "
              "(expected to be clang)")
    endif()
  elseif (NOT "$ENV{YB_COMPILER_TYPE}" STREQUAL "${COMPILER_FAMILY}" AND
          NOT ("$ENV{YB_COMPILER_TYPE}" STREQUAL "gcc" AND "${COMPILER_FAMILY}" STREQUAL "gcc8"))
    message(FATAL_ERROR
            "YB_COMPILER_TYPE environment variable ($ENV{YB_COMPILER_TYPE}) does not match the "
            "compiler family detected by CMake (${COMPILER_FAMILY}")
  endif()

  if (NOT "${COMPILER_FAMILY}" STREQUAL "gcc" AND
      NOT "${COMPILER_FAMILY}" STREQUAL "gcc8" AND
      NOT "${COMPILER_FAMILY}" STREQUAL "clang")
    message(FATAL_ERROR "Unknown compiler family: ${COMPILER_FAMILY} (expected 'gcc' or 'clang').")
  endif()

  set(IS_CLANG FALSE PARENT_SCOPE)
  if ("${COMPILER_FAMILY}" STREQUAL "clang")
    set(IS_CLANG TRUE PARENT_SCOPE)
  endif()

  set(IS_GCC FALSE PARENT_SCOPE)
  if ("${COMPILER_FAMILY}" STREQUAL "gcc" OR "${COMPILER_FAMILY}" STREQUAL "gcc8")
    set(IS_GCC TRUE PARENT_SCOPE)
  endif()
endfunction()

# Functions for adding compiler/linker flags ------------------------------------------------------

# Linker flags applied to both executables and shared libraries. We append this both to
# CMAKE_EXE_LINKER_FLAGS and CMAKE_SHARED_LINKER_FLAGS after we finish making changes to this.
# These flags apply to both YB and RocksDB parts of the codebase.
function(ADD_LINKER_FLAGS FLAGS)
  if ($ENV{YB_VERBOSE})
    message("Adding to linker flags: ${FLAGS}")
  endif()
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FLAGS}" PARENT_SCOPE)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${FLAGS}" PARENT_SCOPE)
endfunction()

function(ADD_GLOBAL_RPATH_ENTRY RPATH_ENTRY)
  if (RPATH_ENTRY STREQUAL "")
    message(FATAL_ERROR "Trying to add an empty rpath entry.")
  endif()
  message("Adding a global rpath entry: ${RPATH_ENTRY}")
  set(FLAGS "-Wl,-rpath,${RPATH_ENTRY}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FLAGS}" PARENT_SCOPE)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${FLAGS}" PARENT_SCOPE)
endfunction()

# CXX_YB_COMMON_FLAGS are flags that are common across the 'src/yb' portion of the codebase (but do
# not apply to the 'src/rocksdb' part). "Common" in the name refers to the fact that they are common
# across different build types.
#
# Compiler flags that are common across debug/release builds:
#  -msse4.2: Enable sse4.2 compiler intrinsics.
#  -Wall: Enable all warnings.
#  -Wno-sign-compare: suppress warnings for comparison between signed and unsigned integers
#  -Wno-deprecated: some of the gutil code includes old things like ext/hash_set, ignore that
#  -pthread: enable multithreaded malloc
#  -fno-strict-aliasing
#     Assume programs do not follow strict aliasing rules.  GCC cannot always verify whether strict
#     aliasing rules are indeed followed due to fundamental limitations in escape analysis, which
#     can result in subtle bad code generation.  This has a small perf hit but worth it to avoid
#     hard to debug crashes.

function(ADD_CXX_FLAGS FLAGS)
  if ($ENV{YB_VERBOSE})
    message("Adding C++ flags: ${FLAGS}")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FLAGS}" PARENT_SCOPE)
endfunction()

DETECT_BREW()

ADD_CXX_FLAGS("-Werror")
ADD_CXX_FLAGS("-fno-strict-aliasing -msse4.2 -Wall -Wno-sign-compare -Wno-deprecated")
ADD_CXX_FLAGS("-Winvalid-pch")
ADD_CXX_FLAGS("-pthread -DBOOST_BIND_NO_PLACEHOLDERS")
ADD_LINKER_FLAGS("-pthread")
if (NOT APPLE)
  ADD_CXX_FLAGS("-DBOOST_UUID_RANDOM_PROVIDER_FORCE_POSIX")
endif()
ADD_CXX_FLAGS("-DROCKSDB_PLATFORM_POSIX -DROCKSDB_USING_THREAD_STATUS=0")
ADD_CXX_FLAGS("-DBOOST_ERROR_CODE_HEADER_ONLY")

function(YB_INCLUDE_EXTENSIONS)
  file(RELATIVE_PATH CUR_REL_LIST_FILE "${YB_SRC_ROOT}" "${CMAKE_CURRENT_LIST_FILE}")
  get_filename_component(CUR_REL_LIST_NAME_NO_EXT "${CUR_REL_LIST_FILE}" NAME_WE)
  get_filename_component(CUR_REL_LIST_DIR "${CUR_REL_LIST_FILE}" DIRECTORY)

  set(YB_MATCHING_ENTERPRISE_DIR "${YB_SRC_ROOT}/ent/${CUR_REL_LIST_DIR}" PARENT_SCOPE)
  set(YB_MATCHING_ENTERPRISE_DIR "${YB_SRC_ROOT}/ent/${CUR_REL_LIST_DIR}")

  set(INCLUDED_PATH "${YB_MATCHING_ENTERPRISE_DIR}/${CUR_REL_LIST_NAME_NO_EXT}-include.txt")
  message("Including '${INCLUDED_PATH}' into '${CMAKE_CURRENT_LIST_FILE}'")
  include("${INCLUDED_PATH}")
endfunction()

set(YB_NUM_TESTS "0" CACHE INTERNAL "Number of tests" FORCE)
set(YB_NUM_INCLUDED_TESTS "0" CACHE INTERNAL "Number of included tests" FORCE)
set(YB_NUM_EXECUTABLES "0" CACHE INTERNAL "Number of executables" FORCE)
set(YB_NUM_INCLUDED_EXECUTABLES "0" CACHE INTERNAL "Number of included executables" FORCE)
set(YB_ALL_DEPS "" CACHE INTERNAL "All dependencies" FORCE)

# This is used to let the add_executable wrapper know if we're adding a test.
set(YB_ADDING_TEST_EXECUTABLE "FALSE" CACHE INTERNAL "" FORCE)

function(yb_remember_dependency target)
  # We use \\n instead of a real newline as this is stored in the CMake cache, and some versions
  # of CMake can't parse their own cache in case some values have newlines.
  set(YB_ALL_DEPS "${YB_ALL_DEPS}\\n${target}: ${ARGN}" CACHE INTERNAL "All dependencies" FORCE)
endfunction()

# Wrap add_dependencies so that we can capture dependencies for external processing. We use this
# when determining what tests to run for a particular set of changes.
function(add_dependencies target)
  if (TARGET "${target}" OR NOT ${YB_FILTERING_TARGETS})
    yb_remember_dependency(${target} ${ARGN})
    _add_dependencies(${target} ${ARGN})
  endif()
endfunction()

function(target_link_libraries target)
  if (TARGET "${target}" OR NOT ${YB_FILTERING_TARGETS})
    yb_remember_dependency(${target} ${ARGN})
    _target_link_libraries(${target} ${ARGN})
  endif()
endfunction()

# We override add_executable to ensure that whenever any executable is built, the latest symlink is
# re-created, and also to filter the set of executables if -DYB_EXECUTABLE_FILTER_RE is specified.
# This filtering is useful to keep CLion responsive when only working on a subset of code including
# e.g. yb-master / yb-tserver and some tests.
function(add_executable name)
  if (NOT ${YB_ADDING_TEST_EXECUTABLE})
    # Count non-test executables.
    math(EXPR NEW_NUM_EXECUTABLES "${YB_NUM_EXECUTABLES} + 1")
    set(YB_NUM_EXECUTABLES "${NEW_NUM_EXECUTABLES}" CACHE INTERNAL "Number of executables" FORCE)
  endif()

  if (NOT "${YB_EXECUTABLE_FILTER_RE}" STREQUAL "" AND
      NOT ${YB_ADDING_TEST_EXECUTABLE} AND
      NOT "${name}" STREQUAL "bfql_codegen" AND
      NOT "${name}" STREQUAL "bfpg_codegen" AND
      NOT "${name}" STREQUAL "run-with-timeout" AND
      NOT "${name}" STREQUAL "protoc-gen-insertions" AND
      NOT "${name}" STREQUAL "protoc-gen-yrpc")
    # Only do this filtering for non-test executables. Tests can be filtered separately using
    # YB_TEST_FILTER_RE.
    string(REGEX MATCH "${YB_EXECUTABLE_FILTER_RE}" EXECUTABLE_FILTER_MATCH_RESULT "${name}")
    if ("${EXECUTABLE_FILTER_MATCH_RESULT}" STREQUAL "")
      return()
    endif()
    message("Executable matched the filter: ${name}")
  endif()

  if (NOT ${YB_ADDING_TEST_EXECUTABLE})
    math(EXPR NEW_NUM_INCLUDED_EXECUTABLES "${YB_NUM_INCLUDED_EXECUTABLES} + 1")
    set(YB_NUM_INCLUDED_EXECUTABLES "${NEW_NUM_INCLUDED_EXECUTABLES}" CACHE INTERNAL
        "Number of included executables" FORCE)
  endif()

  # Call through to the original add_executable function.
  _add_executable("${name}" ${ARGN})
  if (NOT "$ENV{YB_DISABLE_LATEST_SYMLINK}" STREQUAL "1")
    add_dependencies(${name} latest_symlink)
  endif()
endfunction()

macro(YB_SETUP_CLANG BUILD_TYPE)
  ADD_CXX_FLAGS("-stdlib=libc++")

  # Disables using the precompiled template specializations for std::string, shared_ptr, etc
  # so that the annotations in the header actually take effect.
  ADD_CXX_FLAGS("-D_GLIBCXX_EXTERN_TEMPLATE=0")

  set(LIBCXX_DIR "${YB_THIRDPARTY_DIR}/installed/${BUILD_TYPE}/libcxx")
  set(LIBCXX_INCLUDE_DIR "${LIBCXX_DIR}/include/c++/v1")
  ADD_GLOBAL_RPATH_ENTRY("${LIBCXX_DIR}/lib")

  # This needs to appear before adding third-party dependencies that have their headers in the
  # Linuxbrew include directory, because otherwise we'll pick up the standard library headers from
  # the Linuxbrew include directory too.
  include_directories(SYSTEM "${LIBCXX_INCLUDE_DIR}")

  ADD_CXX_FLAGS("-nostdinc++")
  ADD_LINKER_FLAGS("-L${LIBCXX_DIR}/lib")

  # Strictly speaking, TSAN doesn't require dynamic linking. But it does require all code to be
  # position independent, and the easiest way to guarantee that is via dynamic linking (not all 3rd
  # party archives are compiled with -fPIC e.g. boost).
  if("${YB_LINK}" STREQUAL "a")
    message("Using dynamic linking for ${BUILD_TYPE}")
    set(YB_LINK "d")
  elseif("${YB_LINK}" STREQUAL "s")
    message(FATAL_ERROR "Cannot use ${BUILD_TYPE} with static linking")
  endif()
endmacro()

macro(YB_SETUP_SANITIZER SANITIZER)
  if(NOT (("${COMPILER_FAMILY}" STREQUAL "clang")))
    message(FATAL_ERROR "Cannot use ${SANITIZER} without clang")
  endif()

  string(TOLOWER "${SANITIZER}" LOWER_SANITIZER)

  message("Using ${SANITIZER}-instrumented libc++")
  YB_SETUP_CLANG("${LOWER_SANITIZER}")
endmacro()

function(SHOW_FOUND_BOOST_DETAILS BOOST_LIBRARY_TYPE)
  message("Results of finding Boost ${BOOST_LIBRARY_TYPE} libraries:")
  message("    Boost_FOUND: ${Boost_FOUND}")
  message("    Boost_INCLUDE_DIRS: ${Boost_INCLUDE_DIRS}")
  message("    Boost_LIBRARY_DIRS: ${Boost_LIBRARY_DIRS}")
  message("    Boost_LIBRARIES: ${Boost_LIBRARIES}")
  message("    Boost_SYSTEM_FOUND: ${Boost_SYSTEM_FOUND}")
  message("    Boost_SYSTEM_LIBRARY: ${Boost_SYSTEM_LIBRARY}")
  message("    Boost_THREAD_FOUND: ${Boost_THREAD_FOUND}")
  message("    Boost_THREAD_LIBRARY: ${Boost_THREAD_LIBRARY}")
  message("    Boost_VERSION: ${Boost_VERSION}")
  message("    Boost_LIB_VERSION: ${Boost_LIB_VERSION}")
  message("    Boost_MAJOR_VERSION: ${Boost_MAJOR_VERSION}")
  message("    Boost_MINOR_VERSION: ${Boost_MINOR_VERSION}")
  message("    Boost_SUBMINOR_VERSION: ${Boost_SUBMINOR_VERSION}")
  message("    Boost_LIB_DIAGNOSTIC_DEFINITIONS: ${Boost_LIB_DIAGNOSTIC_DEFINITIONS}")
endfunction()

# ------------------------------------------------------------------------------------------------
# Main build

if ("$ENV{YB_DO_NOT_BUILD_TESTS}" STREQUAL "1")
  message("Skipping building tests (YB_DO_NOT_BUILD_TESTS environment variable is set to 1)")
  set(BUILD_TESTS OFF)
else()
  set(BUILD_TESTS ON)
endif()

get_filename_component(YB_BUILD_ROOT_BASENAME "${CMAKE_CURRENT_BINARY_DIR}" NAME)
string(REPLACE "-" ";" YB_BUILD_ROOT_BASENAME_COMPONENTS ${YB_BUILD_ROOT_BASENAME})
list(GET YB_BUILD_ROOT_BASENAME_COMPONENTS 0 YB_BUILD_TYPE)
message("YB_BUILD_TYPE: ${YB_BUILD_TYPE}")
message("CMAKE_MAKE_PROGRAM: ${CMAKE_MAKE_PROGRAM}")

# CMAKE_LINK_DEPENDS_NO_SHARED prevents prevent re-linking dependents of a shared library when it
# is re-linked. Enabling the optimized behavior by default, and allowing to customize it with the
# YB_CMAKE_LINK_DEPENDS_NO_SHARED environment variable.
if (NOT "$ENV{YB_CMAKE_LINK_DEPENDS_NO_SHARED}" STREQUAL "")
  message(
    "Setting CMAKE_LINK_DEPENDS_NO_SHARED to '$ENV{YB_CMAKE_LINK_DEPENDS_NO_SHARED}' "
    "based on the YB_CMAKE_LINK_DEPENDS_NO_SHARED environment variable.")
  set(CMAKE_LINK_DEPENDS_NO_SHARED $ENV{YB_CMAKE_LINK_DEPENDS_NO_SHARED})
else()
  set(CMAKE_LINK_DEPENDS_NO_SHARED 1)
endif()

set(YB_FILTERING_TARGETS FALSE)
if (NOT "${YB_TEST_FILTER_RE}" STREQUAL "" OR NOT "${YB_EXECUTABLE_FILTER_RE}" STREQUAL "")
  set(YB_FILTERING_TARGETS TRUE)
endif()

set(YB_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
message("YB_SRC_ROOT: ${YB_SRC_ROOT}")

set(YB_THIRDPARTY_DIR "$ENV{YB_THIRDPARTY_DIR}")
if("${YB_THIRDPARTY_DIR}" STREQUAL "")
  if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/thirdparty_path.txt")
    file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/thirdparty_path.txt" YB_THIRDPARTY_DIR)
  else()
    set(YB_THIRDPARTY_DIR "${YB_SRC_ROOT}/thirdparty")
  endif()
  set(ENV{YB_THIRDPARTY_DIR} "${YB_THIRDPARTY_DIR}")
endif()
message("YB_THIRDPARTY_DIR: ${YB_THIRDPARTY_DIR}")

set(YB_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(ENV{YB_BUILD_ROOT} "${YB_BUILD_ROOT}")
get_filename_component(YB_BUILD_ROOT_PARENT "${YB_BUILD_ROOT}" DIRECTORY)
message("YB_BUILD_ROOT: ${YB_BUILD_ROOT}")

DETECT_NUMBER_OF_PROCESSORS()

# Detect the shared library suffix on this platform
set(YB_STATIC_LIBRARY_SUFFIX ".a")
if(APPLE)
  set(YB_SHARED_LIBRARY_SUFFIX ".dylib")
else()
  set(YB_SHARED_LIBRARY_SUFFIX ".so")
endif()
message("Using shared library suffix '${YB_SHARED_LIBRARY_SUFFIX}'.")

message("CMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
CHECK_YB_COMPILER_PATH(${CMAKE_C_COMPILER})

message("CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
CHECK_YB_COMPILER_PATH(${CMAKE_CXX_COMPILER})

set(BUILD_SUPPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build-support)

# Provide a 'latest' symlink to this build directory if the "blessed" multi-build layout is
# detected:
#
# build/
# build/<first build directory>
# build/<second build directory>
# ...
set(LATEST_BUILD_SYMLINK_PATH "${YB_BUILD_ROOT_PARENT}/latest")
if (NOT "$ENV{YB_DISABLE_LATEST_SYMLINK}" STREQUAL "1")
  message("LATEST SYMLINK PATH: ${LATEST_BUILD_SYMLINK_PATH}")
  if ("${CMAKE_CURRENT_BINARY_DIR}" STREQUAL "${LATEST_BUILD_SYMLINK_PATH}")
    message(FATAL_ERROR "Should not run cmake inside the build/latest symlink. "
            "First change directories into the destination of the symlink.")
  endif()

  # This option is needed in addition to -sf, while linking, to force the link to occur on
  # directories. Unfortunately, Linux & MacOS differ on the option name.
  if (NOT APPLE)
    set(MORE_ARGS "-T")
  else()
    set(MORE_ARGS "-h")
  endif()
  add_custom_target(latest_symlink ALL
    "${BUILD_SUPPORT_DIR}/create_latest_symlink.sh"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${LATEST_BUILD_SYMLINK_PATH}"
    COMMENT "Recreating the 'latest' symlink at '${LATEST_BUILD_SYMLINK_PATH}'")
endif()

add_custom_target(dummy_target ALL
  COMMAND cat /dev/null
  COMMENT "Dummy target for dependency resolution testing")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake_modules")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake_standard_modules")
include(CMakeParseArguments)

# Allow "make install" to not depend on all targets.
#
# Must be declared in the top-level CMakeLists.txt.
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY true)

# Also allow specifying -DNO_REBUILD_THIRDPARTY, because CLion does not always pass user-specified
# environment variables correctly.
if ("${YB_NO_REBUILD_THIRDPARTY}" STREQUAL "1")
  set(ENV{NO_REBUILD_THIRDPARTY} "1")
else()
  # Do not try building thirdparty outside our source location
  if(NOT "${YB_THIRDPARTY_DIR}" STREQUAL "${YB_SRC_ROOT}/thirdparty")
    set(ENV{NO_REBUILD_THIRDPARTY} "1")
  endif()
endif()

INIT_COMPILER_TYPE()

# This helps find the right third-party build directory.
if (YB_USE_TSAN)
  set(THIRDPARTY_INSTRUMENTATION_TYPE "tsan")
elseif (YB_USE_ASAN)
  set(THIRDPARTY_INSTRUMENTATION_TYPE "asan")
elseif ("$ENV{YB_COMPILER_TYPE}" STREQUAL "clang") # Cannot use IS_CLANG before building 3rd party
  set(THIRDPARTY_INSTRUMENTATION_TYPE "clang_uninstrumented")
elseif ("$ENV{YB_COMPILER_TYPE}" STREQUAL "gcc8")
  set(THIRDPARTY_INSTRUMENTATION_TYPE "gcc8_uninstrumented")
  ADD_LINKER_FLAGS("-L${LINUXBREW_LIB_DIR}/gcc/8")
elseif ("$ENV{YB_COMPILER_TYPE}" STREQUAL "gcc")
  set(THIRDPARTY_INSTRUMENTATION_TYPE "uninstrumented")
else()
  message(FATAL_ERROR "Not handled compiler family: ${COMPILER_FAMILY}.")
endif()

# Make sure thirdparty stuff is up-to-date.
if ("$ENV{NO_REBUILD_THIRDPARTY}" STREQUAL "")
  set(BUILD_THIRDPARTY_ARGS --build-type ${THIRDPARTY_INSTRUMENTATION_TYPE})
  message("Invoking build_thirdparty.sh with these arguments: ${BUILD_THIRDPARTY_ARGS}")
  execute_process(
    COMMAND ${YB_THIRDPARTY_DIR}/build_thirdparty.sh ${BUILD_THIRDPARTY_ARGS}
    RESULT_VARIABLE THIRDPARTY_SCRIPT_RESULT)
  if (NOT (${THIRDPARTY_SCRIPT_RESULT} EQUAL 0))
    message(FATAL_ERROR "Thirdparty was built unsuccessfully, terminating.")
  endif()
else()
  message("The NO_REBUILD_THIRDPARTY environment variable is non-empty, skipping the third-party"
          "build")
endif()

# Generate a Clang compile_commands.json "compilation database" file for use
# with various development tools, such as Vim's YouCompleteMe plugin.
# See http://clang.llvm.org/docs/JSONCompilationDatabase.html
if ("$ENV{CMAKE_EXPORT_COMPILE_COMMANDS}" STREQUAL "1" OR
    "$ENV{YB_RUN_AFFECTED_TESTS_ONLY}" STREQUAL "1")
  set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
endif()

# -------------------------------------------------------------------------------------------------
# Build type (debug, release, fastdebug, etc.)
# -------------------------------------------------------------------------------------------------

# If no build type is specified, default to debug builds
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif(NOT CMAKE_BUILD_TYPE)

string (TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE)

# Alias RELEASE as RELWITHDEBINFO and MINSIZEREL. These are common CMake
# release type names and this provides compatibility with the CLion IDE.
if ("${CMAKE_BUILD_TYPE}" STREQUAL "RELWITHDEBINFO" OR "${CMAKE_BUILD_TYPE}" STREQUAL "MINSIZEREL")
  set(CMAKE_BUILD_TYPE RELEASE)
endif()

############################################################
# Compiler type (gcc, clang, etc.)
############################################################
# Determine compiler version. This will set COMPILER_FAMILY.
include(CompilerInfo)

message("Using COMPILER_FAMILY=${COMPILER_FAMILY}")

# We can only use IS_CLANG after calling VALIDATE_COMPILER_TYPE.
VALIDATE_COMPILER_TYPE()
if (NOT APPLE)
  # To enable 16-byte atomics support we should specify appropriate architecture.
  ADD_CXX_FLAGS("-march=ivybridge")
  ADD_CXX_FLAGS("-mcx16")
endif()

# Include compiler type and version in the compiler command line so that binaries built by different
# versions of the compiler will have different keys in ccache.
ADD_CXX_FLAGS("-DYB_COMPILER_TYPE=$ENV{YB_COMPILER_TYPE}")
ADD_CXX_FLAGS("-DYB_COMPILER_VERSION=${COMPILER_VERSION}")
ADD_CXX_FLAGS("-DROCKSDB_LIB_IO_POSIX")
ADD_CXX_FLAGS("-DSNAPPY")
ADD_CXX_FLAGS("-DZLIB")
if ($ENV{YB_COMPILER_TYPE} STREQUAL "zapcc")
  ADD_CXX_FLAGS("-DYB_ZAPCC")
endif()

############################################################
# Compiler flags
############################################################

if(NOT APPLE)
  # The following flags are required to not assume the presence of too many CPU features so that the
  # code built can run on many platforms. For example, support building on c4.xlarge in AWS (dev
  # servers) and running on c3.xlarge (flash-based cluster machines). There are a couple more flags
  # (-mno-abm and -mno-movbe) that are not recognized by some clang versions we are using, so they
  # are being added in the gcc-specific section below.  We have also found that these flags don't
  # work on Mac OS X, so we're not using them there.
  ADD_CXX_FLAGS("-mno-avx -mno-bmi -mno-bmi2 -mno-fma")
endif()

# We want access to the PRI* print format macros.
ADD_CXX_FLAGS("-D__STDC_FORMAT_MACROS")

# Do not warn about uses of deprecated declarations. RocksDB has a few instances of those.
ADD_CXX_FLAGS("-Wno-deprecated-declarations")

ADD_CXX_FLAGS("-DGFLAGS=gflags")

# Don't allow virtual classes with non-virtual destructors.
ADD_CXX_FLAGS("-Wnon-virtual-dtor")

# Flags common to gcc and clang.
ADD_CXX_FLAGS("-Werror=enum-compare")
ADD_CXX_FLAGS("-Werror=reorder")
ADD_CXX_FLAGS("-Werror=switch")
ADD_CXX_FLAGS("-Werror=return-type")
ADD_CXX_FLAGS("-Werror=non-virtual-dtor")

if(IS_CLANG)
  ADD_CXX_FLAGS("-Werror=string-plus-int")
  ADD_CXX_FLAGS("-Werror=return-stack-address")
  ADD_CXX_FLAGS("-Werror=implicit-fallthrough")
  # Remove this check when all Clang builds will use libc++.
  if("${USING_SANITIZERS}" OR APPLE)
    ADD_CXX_FLAGS("-D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS")
    ADD_CXX_FLAGS("-Wthread-safety-analysis")
  endif()
  if("$ENV{YB_ENABLE_STATIC_ANALYZER}" STREQUAL "1")
    if(APPLE)
      message("YB_ENABLE_STATIC_ANALYZER is set to 1, but we are not running the static analyzer"
              " on macOS yet")
    else()
      message("YB_ENABLE_STATIC_ANALYZER is set to 1, enabling Clang static analyzer")
      include(yb_clang_analyzer)
      message("clang_analyzer_flags=${clang_analyzer_flags}")
      ADD_CXX_FLAGS("${clang_analyzer_flags}")
    endif()
  else()
    message("YB_ENABLE_STATIC_ANALYZER is not set to 1, not enabling Clang static analyzer")
  endif()

  # The actual static analysis is turned on using the --analyze flag.
endif()

if(USING_LINUXBREW AND IS_CLANG)
  # We only need these when building using clang. If we use Linuxbrew's gcc, it should set the
  # equivalent of these options automatically.
  ADD_LINKER_FLAGS("-Wl,--dynamic-linker=${LINUXBREW_LIB_DIR}/ld.so")
  ADD_LINKER_FLAGS("--gcc-toolchain=${LINUXBREW_DIR}")
endif()

if(IS_GCC)
  # For now code relies on fact that all libraries will be linked to binary (for using FLAGS_*)
  # This flag is enabled implicitly on centos but not on ubuntu
  # TODO: Subtitute it with '-as-needed' instead to avoid linking with unused library (issue #1495)
  # for reducing pocess start time
  ADD_LINKER_FLAGS("-Wl,-no-as-needed")
endif()

if(USING_LINUXBREW)
  # This is needed for finding correct versions of Flex, Bison, and other tools.
  set(CMAKE_PREFIX_PATH "${LINUXBREW_DIR}" ${CMAKE_PREFIX_PATH})
endif()

# For both RocksDB and YugaByte code, we need to set the OS flag on Mac. Some code in RocksDB also
# distinguishes further into BSD vs everything else, but we probably do not care further.
if(APPLE)
  ADD_CXX_FLAGS("-DOS_MACOSX")
elseif(NOT "${USING_SANITIZERS}")
  message("Specifying linker flags to not allow any undefined symbols")
  ADD_LINKER_FLAGS("-Wl,--no-undefined -Wl,--no-allow-shlib-undefined")
else()
  message("Not specifying linker flags to not allow any undefined symbols: this is ASAN/TSAN")
endif()

# We don't want to use any stubs; that's exclusively for builds using our
# exported client headers).
ADD_CXX_FLAGS(-DYB_HEADERS_NO_STUBS=1)

# compiler flags for different build types (run 'cmake -DCMAKE_BUILD_TYPE=<type> .')
# For all builds:
# For CMAKE_BUILD_TYPE=Debug
#   -ggdb: Enable gdb debugging
#   -O1: Enable minimum optimization
#   -fno-omit-frame-pointer
#       use frame pointers to allow simple stack frame walking for backtraces.
# For CMAKE_BUILD_TYPE=FastDebug
#   Same as DEBUG (TODO: remove fastdebug build type, use debug instead)
# For CMAKE_BUILD_TYPE=Release
#   -O3: Enable all compiler optimizations
#   -g: Enable symbols for profiler tools (TODO: remove for shipping)
#   -DNDEBUG: Turn off dchecks/asserts/debug only code.
#   -fno-omit-frame-pointer
#       use frame pointers to allow simple stack frame walking for backtraces.
#       This has a small perf hit but worth it for the ability to profile in production
# For profile guided optimization (PGO) builds, in addition to the flags for release builds:
#   1. Build first with CMAKE_BUILD_TYPE_PROFILE_GEN:
#     -fprofile-generate: Indicates compiler should insert profile guided optimization events
#   2. Run the benchmarks (generates *.gcda profiling data).
#   3. Build again with CMAKE_BUILD_TYPE_PROFILE_BUILD
#     -fprofile-use: Compiler will use the profile outputs for optimizations
set(CXX_FLAGS_DEBUG "-ggdb -O1 -fno-omit-frame-pointer")
set(CXX_FLAGS_FASTDEBUG ${CXX_FLAGS_DEBUG})
set(CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG -fno-omit-frame-pointer")

# Nullify CMake's predefined flags since we are handling different build types on our own.
# Without this change CMake will add flags from these variables and
# optimization level will be changed because in case of multiple -O flags gcc uses the last one.
set(CMAKE_CXX_FLAGS_DEBUG "")
set(CMAKE_CXX_FLAGS_RELEASE "")

if (NOT "${YB_USE_LTO}" STREQUAL "")
  set(CXX_FLAGS_RELEASE "${CXX_FLAGS_RELEASE} flto -fno-use-linker-plugin")
endif()

set(CXX_FLAGS_PROFILE_GEN "${CXX_FLAGS_RELEASE} -fprofile-generate")
set(CXX_FLAGS_PROFILE_BUILD "${CXX_FLAGS_RELEASE} -fprofile-use")

set(CLANG_GCC_TOOLCHAIN "")
if(IS_CLANG AND NOT APPLE AND USING_LINUXBREW)
  set(CLANG_GCC_TOOLCHAIN "${LINUXBREW_DIR}")
endif()

# Set compile flags based on the build type.
message("Configured for ${CMAKE_BUILD_TYPE} build "
        "(set with cmake -DCMAKE_BUILD_TYPE={release,debug,...})")
if ("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
  ADD_CXX_FLAGS("${CXX_FLAGS_DEBUG}")
  if(NOT APPLE AND EXISTS "/usr/bin/ld.gold")
    if ("$ENV{YB_NO_LD_GOLD}")
      message("YB_NO_LD_GOLD is defined, not using ld.gold")
    else()
      message("Not using ld.gold to stay compatible with Linuxbrew-based gcc toolchain")
      if(FALSE)
        set(LD_GOLD_FLAGS "-fuse-ld=gold")
        # TODO: should we also add this to CMAKE_SHARED_LINKER_FLAGS?
        message("Adding flags in front of CMAKE_EXE_LINKER_FLAGS to use ld.gold: ${LD_GOLD_FLAGS}")
        set(CMAKE_EXE_LINKER_FLAGS "${LD_GOLD_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
      endif()
    endif()
  endif()
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "FASTDEBUG")
  ADD_CXX_FLAGS("${CXX_FLAGS_FASTDEBUG}")
  # We specify RocksDB debug level that corresponds to the -O1 optimization level, the same as
  # the rest of YB code in the "fastdebug" mode (used for ASAN/TSAN).
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  ADD_CXX_FLAGS("${CXX_FLAGS_RELEASE}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "PROFILE_GEN")
  ADD_CXX_FLAGS("${CXX_FLAGS_PROFILE_GEN}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "PROFILE_BUILD")
  ADD_CXX_FLAGS("${CXX_FLAGS_PROFILE_BUILD}")
else()
  message(FATAL_ERROR "Unknown build type: ${CMAKE_BUILD_TYPE}")
endif ()

if (IS_CLANG)
  # Using Clang with ccache causes a bunch of spurious warnings that are
  # purportedly fixed in the next version of ccache. See the following for details:
  #
  #   http://petereisentraut.blogspot.com/2011/05/ccache-and-clang.html
  #   http://petereisentraut.blogspot.com/2011/09/ccache-and-clang-part-2.html

  # Clang generates ambiguous member template warnings when calling the ev++ api.
  ADD_CXX_FLAGS("-Wno-ambiguous-member-template")

  # Emit warnings (that we then treat as errors in compiler-wrapper.sh) on unannotated fallthrough
  # in switch statements. This applies to both YB and RocksDB parts of the code.
  ADD_CXX_FLAGS("-Wimplicit-fallthrough")

  # Silence the "unused argument" warning.
  ADD_CXX_FLAGS("-Qunused-arguments")

  # Only hardcode -fcolor-diagnostics if stderr is opened on a terminal. Otherwise
  # the color codes show up as noisy artifacts.
  #
  # This test is imperfect because 'cmake' and 'make' can be run independently
  # (with different terminal options), and we're testing during the former.
  execute_process(COMMAND test -t 2 RESULT_VARIABLE YB_IS_TTY)
  if ((${YB_IS_TTY} EQUAL 0) AND (NOT ("$ENV{TERM}" STREQUAL "dumb")))
    message("Running in a controlling terminal")
    ADD_CXX_FLAGS("-fcolor-diagnostics")
  else()
    message("Running without a controlling terminal or in a dumb terminal")
  endif()
elseif("${COMPILER_FAMILY}" STREQUAL "gcc" OR "${COMPILER_FAMILY}" STREQUAL "gcc8")
  # Blacklist gcc versions known to generate broken optimized code.
  #
  # See KUDU-1030 for more details.
  if ("${COMPILER_VERSION}" MATCHES "^4.[67]")
    message(FATAL_ERROR "Building with gcc version ${COMPILER_VERSION} is "
      "forbidden as it is known to produce broken code in release mode. "
      "Upgrade gcc, or build with clang from the thirdparty directory.")
  endif()

  # These flags are not understood by some versions of clang we are using.
  ADD_CXX_FLAGS("-mno-abm -mno-movbe")
else()
  message(FATAL_ERROR "Unknown compiler family: ${COMPILER_FAMILY}")
endif()

set(YB_PREFIX_COMMON "${YB_THIRDPARTY_DIR}/installed/common")

# Flag to enable thread sanitizer (clang or gcc 4.8)
if (${YB_USE_TSAN})
  YB_SETUP_SANITIZER("TSAN")
  ADD_CXX_FLAGS("-fsanitize=thread")

  # Enables dynamic_annotations.h to actually generate code
  ADD_CXX_FLAGS("-DDYNAMIC_ANNOTATIONS_ENABLED")

  # changes atomicops to use the tsan implementations
  ADD_CXX_FLAGS("-DTHREAD_SANITIZER")

  # Compile and link against the thirdparty TSAN instrumented libstdcxx.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
elseif (${YB_USE_ASAN})
  YB_SETUP_SANITIZER("ASAN")

  ADD_CXX_FLAGS("-fsanitize=address")
  ADD_CXX_FLAGS("-DADDRESS_SANITIZER")

  # Compile and link against the thirdparty ASAN instrumented libstdcxx.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
elseif (USING_LINUXBREW AND IS_CLANG)
  YB_SETUP_CLANG("clang_uninstrumented")
endif()

if (USING_LINUXBREW)
  include_directories(SYSTEM ${LINUXBREW_DIR}/include)
endif()

if (NOT IS_CLANG)
  ADD_LINKER_FLAGS("-latomic")
endif()

if (NOT "$ENV{YB_LINK}" STREQUAL "" AND NOT "$ENV{YB_LINK}" STREQUAL "${YB_LINK}")
  if (YB_LINK)
    message(FATAL_ERROR "Conflicting values of YB_LINK CMake variable (\"${YB_LINK}\") and "
      "environment variable (\"$ENV{YB_LINK}\").")
  endif()
  message("YB_LINK is not set as a CMake variable, using the YB_LINK environment variable: "
          "\"$ENV{YB_LINK}\".")
  set(YB_LINK "$ENV{YB_LINK}")
endif()

# Sanity check linking option.
if (NOT YB_LINK)
  message("YB_LINK not specified, determining linking type automatically")
  set(YB_LINK "a")
elseif(NOT ("${YB_LINK}" MATCHES "^[a-z]+$" AND (
                "auto" MATCHES "^${YB_LINK}" OR
                "dynamic" MATCHES "^${YB_LINK}" OR
                "static" MATCHES "^${YB_LINK}"
            )))
  message(FATAL_ERROR "Unknown value \"${YB_LINK}\" for YB_LINK, must be auto|dynamic|static")
else()
  message("YB_LINK explicitly specified as \"${YB_LINK}\".")
  # Remove all but the first letter.
  string(SUBSTRING "${YB_LINK}" 0 1 YB_LINK)
endif()

# Clang does not support using ASAN and TSAN simultaneously.
if ("${YB_USE_ASAN}" AND "${YB_USE_TSAN}")
  message(SEND_ERROR "Can only enable one of ASAN or TSAN at a time")
endif()

# Flag to enable clang undefined behavior sanitizer
# We explicitly don't enable all of the sanitizer flags:
# - disable 'vptr' because it currently crashes somewhere in boost::intrusive::list code
# - disable 'alignment' because unaligned access is really OK on Nehalem and we do it
#   all over the place.
if (${YB_USE_UBSAN})
  if(NOT (("${COMPILER_FAMILY}" STREQUAL "clang") OR
          ("${COMPILER_FAMILY}" STREQUAL "gcc" AND "${COMPILER_VERSION}" VERSION_GREATER "4.9")))
    message(SEND_ERROR "Cannot use UBSAN without clang or gcc >= 4.9")
  endif()
  set(CXX_NO_SANITIZE_FLAG "alignment")
  if(NOT APPLE)
    set(CXX_NO_SANITIZE_FLAG "${CXX_NO_SANITIZE_FLAG},vptr")
    ADD_CXX_FLAGS("-fsanitize=undefined")
  endif()
  ADD_CXX_FLAGS("-fno-sanitize-recover=all -fno-sanitize=${CXX_NO_SANITIZE_FLAG}")
  ADD_CXX_FLAGS("-fsanitize-recover=float-cast-overflow")
endif ()

if ("${YB_USE_UBSAN}" OR "${YB_USE_ASAN}" OR "${YB_USE_TSAN}")
  # GCC 4.8 and 4.9 (latest as of this writing) don't allow you to specify a
  # sanitizer blacklist.
  if("${COMPILER_FAMILY}" STREQUAL "clang")
    # Require clang 3.4 or newer; clang 3.3 has issues with TSAN and pthread
    # symbol interception.
    if("${COMPILER_VERSION}" VERSION_LESS "3.4")
      message(SEND_ERROR "Must use clang 3.4 or newer to run a sanitizer build."
        " Try using clang from thirdparty/")
    endif()
    ADD_CXX_FLAGS("-fsanitize-blacklist=${BUILD_SUPPORT_DIR}/sanitize-blacklist.txt")
  else()
    message(WARNING "GCC does not support specifying a sanitizer blacklist. Known sanitizer "
                    "check failures will not be suppressed.")
  endif()
endif()

# Code coverage
if ("${YB_GENERATE_COVERAGE}")
  if("${CMAKE_CXX_COMPILER}" MATCHES ".*clang.*")
    # There appears to be some bugs in clang 3.3 which cause code coverage
    # to have link errors, not locating the llvm_gcda_* symbols.
    # This should be fixed in llvm 3.4 with http://llvm.org/viewvc/llvm-project?view=revision&revision=184666
    message(SEND_ERROR "Cannot currently generate coverage with clang")
  endif()
  ADD_CXX_FLAGS("--coverage -DCOVERAGE_BUILD")

  # For coverage to work properly, we need to use static linkage. Otherwise,
  # __gcov_flush() doesn't properly flush coverage from every module.
  # See http://stackoverflow.com/questions/28164543/using-gcov-flush-within-a-library-doesnt-force-the-other-modules-to-yield-gc
  if("${YB_LINK}" STREQUAL "a")
    message("Using static linking for coverage build")
    set(YB_LINK "s")
  elseif("${YB_LINK}" STREQUAL "d")
    message(SEND_ERROR "Cannot use coverage with static linking")
  endif()
endif()

if (YB_INSTRUMENT_FUNCTIONS)
  # We parse this special-case preprocessor definition flag in compiler-wrapper and add
  # -finstrument-functions if input files match this pattern. Another consequence of passing this
  # to the compiler wrapper through the command line is that changing this causes all files to be
  # recompiled, which is necessary in the common case to ensure proper instrumentation or lack
  # thereof.
  #
  # Need to escape parentheses and pipe in the regex.
  # add_cxx_flags("-DYB_INSTRUMENT_FUNCTIONS_REL_PATH_RE=src/\\(yb\\|postgres\\)/")
  add_cxx_flags("-DYB_INSTRUMENT_FUNCTIONS_REL_PATH_RE=^postgres_build/")
  # Some functions in PostgreSQL are detected as not returning anything in this mode.
  add_cxx_flags("-Wno-error=return-type")
endif()

# If we still don't know what kind of linking to perform, choose based on
# build type (developers like fast builds).
if ("${YB_LINK}" STREQUAL "a")
  if ("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG" OR
      "${CMAKE_BUILD_TYPE}" STREQUAL "FASTDEBUG" OR
      "${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
    # We use dynamic builds by default even in the release mode.
    message("Using dynamic linking for ${CMAKE_BUILD_TYPE} builds")
    set(YB_LINK "d")
  else()
    message("Using static linking for ${CMAKE_BUILD_TYPE} builds")
    set(YB_LINK "s")
  endif()
endif()

# Are we using the gold linker? It doesn't work with dynamic linking as
# weak symbols aren't properly overridden, causing tcmalloc to be omitted.
# Let's flag this as an error in RELEASE builds (we shouldn't release a
# product like this).
#
# See https://sourceware.org/bugzilla/show_bug.cgi?id=16979 for details.
#
# The gold linker is only for ELF binaries, which OSX doesn't use. We can
# just skip.
if (NOT APPLE)
  set(LINKER_DETECTION_COMMAND "${CMAKE_CXX_COMPILER} ${CMAKE_EXE_LINKER_FLAGS} -Wl,--version")
  # TODO: how do we execute a command line stored in a variable without interpreting the entire
  # string (including spaces) as the command name? Currently, just using "bash -c".
  execute_process(COMMAND bash -c "${LINKER_DETECTION_COMMAND}"
    OUTPUT_VARIABLE LINKER_OUTPUT
    ERROR_VARIABLE LINKER_DETECTION_ERROR_OUTPUT
    RESULT_VARIABLE LINKER_DETECTION_EXIT_CODE)
  if(NOT ${LINKER_DETECTION_EXIT_CODE} EQUAL 0)
    message(FATAL_ERROR
      "Linker detection command '${LINKER_DETECTION_COMMAND}' failed with exit code\
 '${LINKER_DETECTION_EXIT_CODE}'. Error output: ${LINKER_DETECTION_ERROR_OUTPUT}")
  endif()
  message("LINKER_DETECTION_COMMAND=${LINKER_DETECTION_COMMAND}")
  message("LINKER_OUTPUT=${LINKER_OUTPUT}")
endif ()
if (LINKER_OUTPUT MATCHES "gold")
  if ("${YB_LINK}" STREQUAL "d" AND
      "${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
    message(SEND_ERROR "Cannot use gold with dynamic linking in a RELEASE build "
      "as it would cause tcmalloc symbols to get dropped")
  else()
    message("Using gold linker")
  endif()
  set(YB_USING_GOLD 1)
else()
  message("Using ld linker")
endif()

message("Linking type: ${YB_LINK} (d=dynamic, s=static)")
# Having set YB_LINK due to build type and/or sanitizer, it's now safe to
# act on its value.
if ("${YB_LINK}" STREQUAL "d")
  set(BUILD_SHARED_LIBS ON)

  # Position independent code is only necessary when producing shared objects.
  ADD_CXX_FLAGS(-fPIC)
endif()

# where to put generated archives (.a files)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")

# where to put generated libraries (.so files)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

# where to put generated binaries
set(EXECUTABLE_OUTPUT_PATH "${YB_BUILD_ROOT}/bin")
file(MAKE_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}")

# Generated sources always have higher priority than even the "ent" directory include files.
include_directories(${CMAKE_CURRENT_BINARY_DIR}/src)
include_directories(ent/src)
include_directories(src)

############################################################
# Visibility
############################################################
# For generate_export_header() and add_compiler_export_flags().
include(GenerateExportHeader)

# Compile TIme REducer
# See https://github.com/sakra/cotire
include(cotire)

# add_library() wrapper that adds a second variant of the library for use in the
# exported YB C++ client. This variant is suffixed with "_exported" and is
# compiled with special visibility flags to hide all symbols except those that
# are part of the public ABI.
#
# There are two different kinds of exported libraries: internal and leaf.
# Internal libraries are static archives while leaf libraries are shared
# objects built from internal libraries. In practice there is only one leaf
# library: the YB C++ client itself.
#
# Arguments:
#
# LIB_NAME is the name of the library. It must come first. Required.
#
# SRCS is the list of source files to compile into the library. Required.
#
# DEPS is the list of targets that both library variants depend on. Required.
#
# NONLINK_DEPS is the list of (non-linked) targets that the library depends on. Optional.
#
# COMPILE_FLAGS is a string containing any additional compilation flags that
# should be added to the library. Optional.
function(ADD_YB_LIBRARY LIB_NAME)
  # Parse the arguments.
  set(options "")
  set(one_value_args COMPILE_FLAGS)
  set(multi_value_args SRCS DEPS NONLINK_DEPS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(SEND_ERROR "Error: unrecognized arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  add_library(${LIB_NAME} ${ARG_SRCS})
  if(ARG_COMPILE_FLAGS)
    set_target_properties(${LIB_NAME}
      PROPERTIES COMPILE_FLAGS ${ARG_COMPILE_FLAGS})
  endif()
  target_link_libraries(${LIB_NAME} ${ARG_DEPS})
  yb_remember_dependency(${LIB_NAME} ${ARG_DEPS})
  if(ARG_NONLINK_DEPS)
    add_dependencies(${LIB_NAME} ${ARG_NONLINK_DEPS})
  endif()
endfunction()

############################################################
# Testing
############################################################

# Compose ctest prefix and test binary name based on test filename and directory
# REL_TEST_NAME is the name of the test (see ADD_YB_TEST function).
# CTEST_PREFIX_RV - ctest prefix will be stored in a variable with name ${CTEST_PREFIX_RV}.
# TEST_BINARY_NAME_RV - test binary name will be stored in a variable with name
#                       ${TEST_BINARY_NAME_RV}.
# (Note: "RV" means "return value" here).
function(GET_TEST_PREFIX_AND_BINARY_NAME CTEST_PREFIX_RV TEST_BINARY_NAME_RV REL_TEST_NAME)
  set(DIR ${CMAKE_CURRENT_LIST_DIR})
  while (true)
    get_filename_component(DIR_NAME ${DIR} NAME_WE)
    # We want to use parent directory name as a prefix if test is located in "test" or "tests"
    # directory, so tests from 'src/ql/test' will be compiled into 'tests-ql' rather than
    # 'tests-test'.
    if (NOT "${DIR_NAME}" MATCHES "^(test|tests)$")
      set(${CTEST_PREFIX_RV} "${DIR_NAME}" PARENT_SCOPE)
      break()
    endif()
    get_filename_component(DIR ${DIR} DIRECTORY)
    if (${DIR} STREQUAL ${YB_SRC_ROOT})
      # If we have tests in 'src/test' or 'src/tests' - use following prefix:
      set(${CTEST_PREFIX_RV} "other" PARENT_SCOPE)
      break()
    endif()
  endwhile()
  get_filename_component(FILE_NAME ${REL_TEST_NAME} NAME_WE)
  set(${TEST_BINARY_NAME_RV} "${FILE_NAME}" PARENT_SCOPE)
endfunction()

# Add a new test case, with or without an executable that should be built.
#
# REL_TEST_NAME is the name of the test. It may be a single component
# (e.g. monotime-test) or contain additional components (e.g.
# net/net_util-test). Either way, the last component must be a globally
# unique name.
#
# Arguments after the test name will be passed to set_tests_properties().
function(ADD_YB_TEST REL_TEST_NAME)
  math(EXPR NEW_YB_NUM_TESTS "${YB_NUM_TESTS} + 1")
  set(YB_NUM_TESTS "${NEW_YB_NUM_TESTS}" CACHE INTERNAL "Number of tests" FORCE)
  if(NOT ${BUILD_TESTS})
    return()
  endif()
  if(NOT "${YB_TEST_FILTER_RE}" STREQUAL "")
    string(REGEX MATCH "${YB_TEST_FILTER_RE}" TEST_FILTER_MATCH_RESULT "${REL_TEST_NAME}")
    if ("${TEST_FILTER_MATCH_RESULT}" STREQUAL "")
      return()
    endif()
    message("Test program matched the filter: ${REL_TEST_NAME}")
  endif()

  math(EXPR NEW_YB_NUM_INCLUDED_TESTS "${YB_NUM_INCLUDED_TESTS} + 1")
  set(YB_NUM_INCLUDED_TESTS "${NEW_YB_NUM_INCLUDED_TESTS}" CACHE INTERNAL
      "Number of included tests" FORCE)

  GET_TEST_PREFIX_AND_BINARY_NAME(CTEST_PREFIX TEST_BINARY_NAME ${REL_TEST_NAME})
  SET(TEST_BINARY_DIR "${YB_BUILD_ROOT}/tests-${CTEST_PREFIX}")
  SET(CTEST_TEST_NAME "${CTEST_PREFIX}_${TEST_BINARY_NAME}")

  # We're using CMAKE_CURRENT_LIST_DIR instead of CMAKE_CURRENT_SOURCE_DIR here so that we can
  # include a CMakeLists.txt from a different directory and add tests whose sources reside in the
  # same directory as the included CMakeLists.txt.
  SET(TEST_SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/${REL_TEST_NAME}.cc")

  if(YB_MATCHING_ENTERPRISE_DIR)
    SET(TEST_SOURCE_PATH_ENTERPRISE "${YB_MATCHING_ENTERPRISE_DIR}/${REL_TEST_NAME}.cc")
    if(EXISTS "${TEST_SOURCE_PATH}" AND
       EXISTS "${TEST_SOURCE_PATH_ENTERPRISE}" AND
       NOT "${TEST_SOURCE_PATH}" STREQUAL "${TEST_SOURCE_PATH_ENTERPRISE}")
      message(FATAL_ERROR "Both ${TEST_SOURCE_PATH} and ${TEST_SOURCE_PATH_ENTERPRISE} exist!")
    endif()
    if(NOT EXISTS "${TEST_SOURCE_PATH}")
      SET(TEST_SOURCE_PATH "${TEST_SOURCE_PATH_ENTERPRISE}")
    endif()
  endif()

  if(NOT EXISTS "${TEST_SOURCE_PATH}")
    message(FATAL_ERROR "Test source '${TEST_SOURCE_PATH}' does not exist.")
  endif()

  set(TEST_PATH "${TEST_BINARY_DIR}/${TEST_BINARY_NAME}")
  set(LAST_ADDED_TEST_BINARY_PATH "${TEST_PATH}" PARENT_SCOPE)
  set(LAST_ADDED_TEST_BINARY_NAME "${TEST_BINARY_NAME}" PARENT_SCOPE)

  # We need the ${CMAKE_CURRENT_LIST_DIR} prefix, because in case of including a CMakeLists.txt
  # file from a different directory, the test's source might not be in
  # ${CMAKE_CURRENT_SOURCE_DIR}.

  # This is used to let add_executable know whether we're adding a test.
  set(YB_ADDING_TEST_EXECUTABLE "TRUE" CACHE INTERNAL "" FORCE)

  add_executable("${TEST_BINARY_NAME}" "${TEST_SOURCE_PATH}")

  set(YB_ADDING_TEST_EXECUTABLE "FALSE" CACHE INTERNAL "" FORCE)

  set_target_properties(${TEST_BINARY_NAME}
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_BINARY_DIR}")
  target_link_libraries(${TEST_BINARY_NAME} ${YB_TEST_LINK_LIBS})
  ADD_COMMON_YB_TEST_DEPENDENCIES(${REL_TEST_NAME})
  add_custom_target("${CTEST_TEST_NAME}" DEPENDS "${TEST_BINARY_NAME}")
  add_test("${CTEST_TEST_NAME}" "${BUILD_SUPPORT_DIR}/run-test.sh" "${TEST_PATH}")
  set_target_properties(
    ${CTEST_TEST_NAME}
    PROPERTIES
    TEST_BINARY_PATH "${TEST_PATH}"
  )
  set_target_properties(${TEST_BINARY_NAME} PROPERTIES COMPILE_FLAGS "-DDISABLE_STATUS_NODISCARD")

  if(ARGN)
    set_tests_properties(${TEST_NAME} PROPERTIES ${ARGN})
  endif()
endfunction()

function(ADD_YB_TESTS TESTS)
  foreach(TEST ${TESTS})
    ADD_YB_TEST(${TEST})
  endforeach(TEST)
endfunction()

# A wrapper for add_dependencies() that takes BUILD_TESTS into account.
function(ADD_YB_TEST_DEPENDENCIES REL_TEST_NAME)
  if(NOT ${BUILD_TESTS})
    return()
  endif()
  GET_TEST_PREFIX_AND_BINARY_NAME(CTEST_PREFIX TEST_BINARY_NAME ${REL_TEST_NAME})

  add_dependencies(${TEST_BINARY_NAME} ${ARGN})
endfunction()

function(YB_TEST_TARGET_LINK_LIBRARIES REL_TEST_NAME)
  if(NOT ${BUILD_TESTS})
    return()
  endif()
  GET_TEST_PREFIX_AND_BINARY_NAME(CTEST_PREFIX TEST_BINARY_NAME ${REL_TEST_NAME})

  target_link_libraries(${TEST_BINARY_NAME} ${ARGN})
endfunction()

function(YB_TEST_TARGET_INCLUDE_DIRECTORIES REL_TEST_NAME)
  if(NOT ${BUILD_TESTS})
    return()
  endif()
  GET_TEST_PREFIX_AND_BINARY_NAME(CTEST_PREFIX TEST_BINARY_NAME ${REL_TEST_NAME})

  target_include_directories(${TEST_BINARY_NAME} ${ARGN})
endfunction()

function(YB_TEST_TARGET_COMPILE_OPTIONS REL_TEST_NAME)
  if(NOT ${BUILD_TESTS})
    return()
  endif()
  GET_TEST_PREFIX_AND_BINARY_NAME(CTEST_PREFIX TEST_BINARY_NAME ${REL_TEST_NAME})

  target_compile_options(${TEST_BINARY_NAME} ${ARGN})
endfunction()


# Adds dependencies common to all tests, both main YB tests added using ADD_YB_TESTS,
# and RocksDB tests, which are added in rocksdb/CMakeLists.txt using add_test directly.
function(ADD_COMMON_YB_TEST_DEPENDENCIES REL_TEST_NAME)
  # Our ctest-based wrapper, run-test.sh, uses this tool to put a time limit on tests.
  ADD_YB_TEST_DEPENDENCIES(${REL_TEST_NAME} run-with-timeout)
endfunction()

enable_testing()

############################################################
# Dependencies
############################################################
function(ADD_THIRDPARTY_LIB LIB_NAME)
  set(options)
  set(one_value_args SHARED_LIB STATIC_LIB)
  set(multi_value_args DEPS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(SEND_ERROR "Error: unrecognized arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  if(("${YB_LINK}" STREQUAL "s" AND ARG_STATIC_LIB) OR (NOT ARG_SHARED_LIB))
    if(NOT ARG_STATIC_LIB)
      message(FATAL_ERROR "No static or shared library provided for ${LIB_NAME}")
    endif()
    add_library(${LIB_NAME} STATIC IMPORTED)
    set_target_properties(${LIB_NAME}
      PROPERTIES IMPORTED_LOCATION "${ARG_STATIC_LIB}")
    message("Added static library dependency ${LIB_NAME}: ${ARG_STATIC_LIB}")
  else()
    add_library(${LIB_NAME} SHARED IMPORTED)
    set_target_properties(${LIB_NAME}
      PROPERTIES IMPORTED_LOCATION "${ARG_SHARED_LIB}")
    message("Added shared library dependency ${LIB_NAME}: ${ARG_SHARED_LIB}")
  endif()

  if(ARG_DEPS)
    set_target_properties(${LIB_NAME}
      PROPERTIES IMPORTED_LINK_INTERFACE_LIBRARIES "${ARG_DEPS}")
  endif()

  # Set up an "exported variant" for this thirdparty library (see "Visibility"
  # above). It's the same as the real target, just with an "_exported" suffix.
  # We prefer the static archive if it exists (as it's akin to an "internal"
  # library), but we'll settle for the shared object if we must.
  #
  # A shared object exported variant will force any "leaf" library that
  # transitively depends on it to also depend on it at runtime; this is
  # desirable for some libraries (e.g. cyrus_sasl).
  set(LIB_NAME_EXPORTED ${LIB_NAME}_exported)
  if(ARG_STATIC_LIB)
    add_library(${LIB_NAME_EXPORTED} STATIC IMPORTED)
    set_target_properties(${LIB_NAME_EXPORTED}
      PROPERTIES IMPORTED_LOCATION "${ARG_STATIC_LIB}")
  else()
    add_library(${LIB_NAME_EXPORTED} SHARED IMPORTED)
    set_target_properties(${LIB_NAME_EXPORTED}
      PROPERTIES IMPORTED_LOCATION "${ARG_SHARED_LIB}")
  endif()
  if(ARG_DEPS)
    set_target_properties(${LIB_NAME_EXPORTED}
      PROPERTIES IMPORTED_LINK_INTERFACE_LIBRARIES "${ARG_DEPS}")
  endif()
endfunction()

# Look in thirdparty prefix paths before anywhere else for system dependencies.
set(CMAKE_PREFIX_PATH ${YB_PREFIX_COMMON} ${CMAKE_PREFIX_PATH})
ADD_LINKER_FLAGS("-L${YB_PREFIX_COMMON}/lib")

set(YB_THIRDPARTY_INSTALLED_DEPS_DIR
    "${YB_THIRDPARTY_DIR}/installed/${THIRDPARTY_INSTRUMENTATION_TYPE}")
set(CMAKE_PREFIX_PATH ${YB_THIRDPARTY_INSTALLED_DEPS_DIR} ${CMAKE_PREFIX_PATH})

set(YB_THIRDPARTY_INSTALLED_DIR ${YB_PREFIX_COMMON})

# See thirdparty/yb_build_thirdparty_main.py for an explanation of differences between installed and
# installed-deps.
ADD_GLOBAL_RPATH_ENTRY("${YB_THIRDPARTY_INSTALLED_DIR}/lib")
ADD_GLOBAL_RPATH_ENTRY("${YB_THIRDPARTY_INSTALLED_DEPS_DIR}/lib")

# If we are using a non-default gcc or clang compiler, we need to add its library directory to
# rpath.
if(IS_GCC AND NOT "$ENV{YB_GCC_PREFIX}" STREQUAL "")
  # TODO: this works for the gcc 6.2.0 build on Linux, might be different on other platforms.
  ADD_GLOBAL_RPATH_ENTRY("$ENV{YB_GCC_PREFIX}/lib64")
endif()
if(IS_CLANG AND NOT "$ENV{YB_CLANG_PREFIX}" STREQUAL "")
  # TODO: this works for the Linux binary clang 3.9 package, might be different on Mac OS X.
  ADD_GLOBAL_RPATH_ENTRY("$ENV{YB_CLANG_PREFIX}/lib")
endif()

# -------------------------------------------------------------------------------------------------

## OpenSSL

find_package(OpenSSL REQUIRED)
include_directories(SYSTEM ${OPENSSL_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(openssl
  SHARED_LIB "${OPENSSL_CRYPTO_LIBRARY}"
  DEPS ${OPENSSL_LIB_DEPS})

message("OPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}")
message("OPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}")
message("OPENSSL_FOUND=${OPENSSL_FOUND}")
message("OPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}")
message("OPENSSL_LIBRARIES=${OPENSSL_LIBRARIES}")
message("OPENSSL_VERSION=${OPENSSL_VERSION}")

if (NOT "${OPENSSL_CRYPTO_LIBRARY}" MATCHES "^${YB_THIRDPARTY_DIR}/.*")
  message(FATAL_ERROR "OPENSSL_CRYPTO_LIBRARY not in ${YB_THIRDPARTY_DIR}.")
endif()
if (NOT "${OPENSSL_SSL_LIBRARY}" MATCHES "^${YB_THIRDPARTY_DIR}/.*")
  message(FATAL_ERROR "OPENSSL_SSL_LIBRARY not in ${YB_THIRDPARTY_DIR}.")
endif()

## GLog
find_package(GLog REQUIRED)
include_directories(SYSTEM ${GLOG_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(glog
  STATIC_LIB "${GLOG_STATIC_LIB}"
  SHARED_LIB "${GLOG_SHARED_LIB}")
list(APPEND YB_BASE_LIBS glog)

find_package(CDS REQUIRED)
include_directories(SYSTEM ${CDS_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(cds
  STATIC_LIB "${CDS_STATIC_LIB}"
  SHARED_LIB "${CDS_SHARED_LIB}")

## libunwind (dependent of glog)
## Doesn't build on OSX.
if (NOT APPLE)
  find_package(LibUnwind REQUIRED)
  include_directories(SYSTEM ${UNWIND_INCLUDE_DIR})
  ADD_THIRDPARTY_LIB(unwind
    STATIC_LIB "${UNWIND_STATIC_LIB}"
    SHARED_LIB "${UNWIND_SHARED_LIB}")
  ADD_THIRDPARTY_LIB(unwind-arch
    STATIC_LIB "${UNWIND_STATIC_ARCH_LIB}"
    SHARED_LIB "${UNWIND_SHARED_ARCH_LIB}")
  list(APPEND YB_BASE_LIBS unwind)

  find_package(LibUuid REQUIRED)
  include_directories(SYSTEM ${LIBUUID_INCLUDE_DIR})
  ADD_THIRDPARTY_LIB(libuuid
    STATIC_LIB "${LIBUUID_STATIC_LIB}"
    SHARED_LIB "${LIBUUID_SHARED_LIB}")
endif()

## GFlags
find_package(GFlags REQUIRED)
include_directories(SYSTEM ${GFLAGS_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(gflags
  STATIC_LIB "${GFLAGS_STATIC_LIB}"
  SHARED_LIB "${GFLAGS_SHARED_LIB}")
list(APPEND YB_BASE_LIBS gflags)

## GMock
find_package(GMock REQUIRED)
include_directories(SYSTEM ${GMOCK_INCLUDE_DIR} ${GTEST_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(gmock
  STATIC_LIB ${GMOCK_STATIC_LIBRARY}
  SHARED_LIB ${GMOCK_SHARED_LIBRARY})

## Protobuf
add_custom_target(gen_proto)
find_package(Protobuf REQUIRED)
include_directories(SYSTEM ${PROTOBUF_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(protobuf
  STATIC_LIB "${PROTOBUF_STATIC_LIBRARY}"
  SHARED_LIB "${PROTOBUF_SHARED_LIBRARY}")
ADD_THIRDPARTY_LIB(protoc
  STATIC_LIB "${PROTOBUF_PROTOC_STATIC_LIBRARY}"
  SHARED_LIB "${PROTOBUF_PROTOC_LIBRARY}"
  DEPS protobuf)
find_package(YRPC REQUIRED)

## Snappy
find_package(Snappy REQUIRED)
include_directories(SYSTEM ${SNAPPY_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(snappy
  STATIC_LIB "${SNAPPY_STATIC_LIB}"
  SHARED_LIB "${SNAPPY_SHARED_LIB}")

## Libev
find_package(LibEv REQUIRED)
include_directories(SYSTEM ${LIBEV_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(libev
  STATIC_LIB "${LIBEV_STATIC_LIB}"
  SHARED_LIB "${LIBEV_SHARED_LIB}")

## libbacktrace
if(NOT APPLE)
find_package(LibBacktrace REQUIRED)
include_directories(SYSTEM ${LIBBACKTRACE_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(libbacktrace
  STATIC_LIB "${LIBBACKTRACE_STATIC_LIB}"
  SHARED_LIB "${LIBBACKTRACE_SHARED_LIB}")
endif()

## LZ4
find_package(Lz4 REQUIRED)
include_directories(SYSTEM ${LZ4_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(lz4 STATIC_LIB "${LZ4_STATIC_LIB}")

## ZLib
find_package(Zlib REQUIRED)
include_directories(SYSTEM ${ZLIB_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(zlib
  STATIC_LIB "${ZLIB_STATIC_LIB}"
  SHARED_LIB "${ZLIB_SHARED_LIB}")

## Squeasel
find_package(Squeasel REQUIRED)
include_directories(SYSTEM ${SQUEASEL_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(squeasel
  STATIC_LIB "${SQUEASEL_STATIC_LIB}")

## Hiredis
find_package(Hiredis REQUIRED)
include_directories(SYSTEM ${HIREDIS_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(hiredis STATIC_LIB "${HIREDIS_STATIC_LIB}")

if (NOT "${YB_USE_ASAN}" AND
    NOT "${YB_USE_TSAN}" AND
    NOT "${YB_USING_GOLD}")
  ## Google PerfTools
  ##
  ## Disabled with TSAN/ASAN as well as with gold (see comment
  ## near definition of YB_USING_GOLD).
  find_package(GPerf REQUIRED)
  message("Deciding whether to use tcmalloc based on:\
          YB_TCMALLOC_AVAILABLE=${YB_TCMALLOC_AVAILABLE}")
else()
  set(YB_TCMALLOC_AVAILABLE "Ignored")
endif()

if ("${YB_TCMALLOC_AVAILABLE}" STREQUAL "")
  message("Using tcmalloc")
  ADD_THIRDPARTY_LIB(tcmalloc
    STATIC_LIB "${TCMALLOC_STATIC_LIB}"
    SHARED_LIB "${TCMALLOC_SHARED_LIB}")
  ADD_THIRDPARTY_LIB(profiler
    STATIC_LIB "${PROFILER_STATIC_LIB}"
    SHARED_LIB "${PROFILER_SHARED_LIB}")
  list(APPEND YB_BASE_LIBS tcmalloc profiler)
  ADD_CXX_FLAGS("-DTCMALLOC_ENABLED")
  set(YB_TCMALLOC_AVAILABLE 1)
else()
  message("Not using tcmalloc, because of '${YB_TCMALLOC_AVAILABLE}'")
endif()

## curl
find_package(CURL REQUIRED)
include_directories(SYSTEM ${CURL_INCLUDE_DIRS})
ADD_THIRDPARTY_LIB(curl STATIC_LIB "${CURL_LIBRARIES}")

## crcutil
find_package(Crcutil REQUIRED)
include_directories(SYSTEM ${CRCUTIL_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(crcutil
  STATIC_LIB "${CRCUTIL_STATIC_LIB}"
  SHARED_LIB "${CRCUTIL_SHARED_LIB}")

## crypt_blowfish -- has no shared library!
find_package(CryptBlowfish REQUIRED)
include_directories(SYSTEM ${CRYPT_BLOWFISH_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(crypt_blowfish
  STATIC_LIB "${CRYPT_BLOWFISH_STATIC_LIB}")

## librt
if (NOT APPLE)
  find_library(RT_LIB_PATH rt)
  if(NOT RT_LIB_PATH)
    message(FATAL_ERROR "Could not find librt on the system path")
  endif()
  ADD_THIRDPARTY_LIB(rt
    SHARED_LIB "${RT_LIB_PATH}")
endif()

## Boost

# BOOST_ROOT is used by find_package(Boost ...) below.
set(BOOST_ROOT "${YB_THIRDPARTY_INSTALLED_DEPS_DIR}")

### Workaround for http://stackoverflow.com/questions/9948375/cmake-find-package-succeeds-but-returns-wrong-path
set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_SYSTEM_PATHS ON)

# Find Boost static libraries.
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost COMPONENTS system thread REQUIRED)
show_found_boost_details("static")

set(BOOST_STATIC_LIBS ${Boost_LIBRARIES})
list(LENGTH BOOST_STATIC_LIBS BOOST_STATIC_LIBS_LEN)
list(SORT BOOST_STATIC_LIBS)

# Find Boost shared libraries.
set(Boost_USE_STATIC_LIBS OFF)
find_package(Boost COMPONENTS system thread REQUIRED)
show_found_boost_details("shared")
set(BOOST_SHARED_LIBS ${Boost_LIBRARIES})
list(LENGTH BOOST_SHARED_LIBS BOOST_SHARED_LIBS_LEN)
list(SORT BOOST_SHARED_LIBS)

# We should have found the same number of libraries both times.
if(NOT ${BOOST_SHARED_LIBS_LEN} EQUAL ${BOOST_STATIC_LIBS_LEN})
  set(ERROR_MSG "Boost static and shared libraries are inconsistent.")
  set(ERROR_MSG "${ERROR_MSG} Static libraries: ${BOOST_STATIC_LIBS}.")
  set(ERROR_MSG "${ERROR_MSG} Shared libraries: ${BOOST_SHARED_LIBS}.")
  message(FATAL_ERROR "${ERROR_MSG}")
endif()

# Add each pair of static/shared libraries.
math(EXPR LAST_IDX "${BOOST_STATIC_LIBS_LEN} - 1")
foreach(IDX RANGE ${LAST_IDX})
  list(GET BOOST_STATIC_LIBS ${IDX} BOOST_STATIC_LIB)
  list(GET BOOST_SHARED_LIBS ${IDX} BOOST_SHARED_LIB)

  # Remove the prefix/suffix from the library name.
  #
  # e.g. libboost_system-mt --> boost_system
  get_filename_component(LIB_NAME ${BOOST_STATIC_LIB} NAME_WE)
  string(REGEX REPLACE "lib([^-]*)(-mt)?" "\\1" LIB_NAME_NO_PREFIX_SUFFIX ${LIB_NAME})
  ADD_THIRDPARTY_LIB(${LIB_NAME_NO_PREFIX_SUFFIX}
    # Boost's static library is not compiled with PIC so it cannot be used to link a shared
    # library. So skip that and use the shared library instead.
    # STATIC_LIB "${BOOST_STATIC_LIB}"
    SHARED_LIB "${BOOST_SHARED_LIB}")
  list(APPEND YB_BASE_LIBS ${LIB_NAME_NO_PREFIX_SUFFIX})
endforeach()
include_directories(SYSTEM ${Boost_INCLUDE_DIR})

find_package(ICU COMPONENTS i18n uc REQUIRED)
include_directories(SYSTEM ${ICU_INCLUDE_DIRS})
ADD_THIRDPARTY_LIB(icui18n
  STATIC_LIB "${ICU_I18N_LIBRARIES}"
  SHARED_LIB "${ICU_I18N_LIBRARIES}")
ADD_THIRDPARTY_LIB(icuuc
  STATIC_LIB "${ICU_UC_LIBRARIES}"
  SHARED_LIB "${ICU_UC_LIBRARIES}")
message("ICU_I18N_LIBRARIES=${ICU_I18N_LIBRARIES}")
message("ICU_UC_LIBRARIES=${ICU_UC_LIBRARIES}")

## Libuv
find_package(LibUv REQUIRED)
include_directories(SYSTEM ${LIBUV_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(libuv
  STATIC_LIB "${LIBUV_STATIC_LIB}"
  SHARED_LIB "${LIBUV_SHARED_LIB}")

## C++ Cassandra driver
find_package(Cassandra REQUIRED)
include_directories(SYSTEM ${LIBCASSANDRA_INCLUDE_DIR})
ADD_THIRDPARTY_LIB(cassandra SHARED_LIB "${LIBCASSANDRA_SHARED_LIB}")

############################################################
# Linker setup
############################################################
set(YB_MIN_TEST_LIBS yb_test_main yb_test_util ${YB_BASE_LIBS})
set(YB_TEST_LINK_LIBS ${YB_MIN_TEST_LIBS})

############################################################
# "make ctags" target
############################################################
if (UNIX)
  add_custom_target(ctags ctags --languages=c++,c -L
    `find ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_BINARY_DIR}/src`)
endif (UNIX)

############################################################
# "make etags" target
#
# Requires the exuberant-ctags system package.
############################################################
if (UNIX)
  add_custom_target(etags etags --members --declarations
    `find ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_BINARY_DIR}/src
          -name \\*.cc -or -name \\*.hh -or -name \\*.cpp -or
          -name \\*.h -or -name \\*.c`)
endif (UNIX)

############################################################
# "make cscope" target
############################################################
if (UNIX)
  add_custom_target(cscope
    find ${CMAKE_CURRENT_SOURCE_DIR}/src
         ${CMAKE_CURRENT_BINARY_DIR}/src
         -name \\*.cc -or -name \\*.hh -or -name \\*.cpp -or
         -name \\*.h -or -name \\*.c
    > cscope.files && cscope -q -b)
endif (UNIX)

############################################################
# "make lint" target
############################################################
if (UNIX)
  # Full lint
  add_custom_target(lint ${BUILD_SUPPORT_DIR}/lint.sh)
  # Incremental lint - only checks files changed since the last
  # merged upstream commit
  add_custom_target(ilint ${BUILD_SUPPORT_DIR}/lint.sh -c)
endif (UNIX)

############################################################
# "make docs" target
############################################################
if (UNIX)
  add_custom_target(docs
    # The docs output HTML will end up in a docs/ subdir.
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/support/scripts/make_docs.sh
      --build_root ${CMAKE_CURRENT_BINARY_DIR})
endif (UNIX)

############################################################
# "make site" target
############################################################
if (UNIX)
  add_custom_target(site
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/support/scripts/make_site.sh)
endif (UNIX)

############################################################
# Subdirectories
############################################################

# For any C code, use the same flags as for C++.
set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS}")

# C++-only flags.
if ("${COMPILER_FAMILY}" STREQUAL "gcc" AND
    ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 7.0)
  # Used with GCC 7.
  ADD_CXX_FLAGS("-faligned-new")
endif()

string (REPLACE "-Werror=non-virtual-dtor" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
string (REPLACE "-Werror=reorder" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
string (REPLACE "-Wnon-virtual-dtor" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
string (REPLACE "-Woverloaded-virtual" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})


message("CMAKE_C_FLAGS ${CMAKE_C_FLAGS}")

set(CMAKE_CXX_STANDARD 14)

if ("${COMPILER_FAMILY}" STREQUAL "gcc8")
  # Remove after switching to C++17
  ADD_CXX_FLAGS("-Wno-aligned-new")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (USING_LINUXBREW)
  # Add the Linuxbrew library directory last.
  ADD_LINKER_FLAGS("-L${LINUXBREW_LIB_DIR}")
  ADD_GLOBAL_RPATH_ENTRY("${LINUXBREW_LIB_DIR}")
  ADD_LINKER_FLAGS("-L/usr/lib64")
  ADD_GLOBAL_RPATH_ENTRY("/usr/lib64")
endif()

message("Linker flags: ${CMAKE_EXE_LINKER_FLAGS}")

set(PQ_SHARED_LIB
    "${YB_BUILD_ROOT}/postgres/lib/libpq${YB_SHARED_LIBRARY_SUFFIX}")
set(YB_PGBACKEND_SHARED_LIB
    "${YB_BUILD_ROOT}/postgres/lib/libyb_pgbackend${YB_SHARED_LIBRARY_SUFFIX}")

# -------------------------------------------------------------------------------------------------
# Subdirectories
# -------------------------------------------------------------------------------------------------

add_subdirectory(src/yb/rocksdb)
add_subdirectory(src/yb/gutil)
add_subdirectory(src/yb/util)
add_subdirectory(src/yb/common)
add_subdirectory(src/yb/fs)
add_subdirectory(src/yb/server)
add_subdirectory(src/yb/tablet)
add_subdirectory(src/yb/util/bfql)
add_subdirectory(src/yb/util/bfpg)
add_subdirectory(src/yb/rpc)
add_subdirectory(src/yb/tserver)
add_subdirectory(src/yb/consensus)
add_subdirectory(src/yb/master)
add_subdirectory(src/yb/client)
add_subdirectory(src/yb/integration-tests)
add_subdirectory(src/yb/benchmarks)
add_subdirectory(src/yb/tools)
add_subdirectory(src/yb/rocksutil)
add_subdirectory(src/yb/docdb)
add_subdirectory(src/yb/yql)
add_subdirectory(src/yb/cdc)

# PostgreSQL source code is not in src/yb so that we can grep non-PostgreSQL YB code easily.
add_subdirectory(src/postgres)

include("${YB_SRC_ROOT}/ent/CMakeLists.txt")

# ------------------------------------------------------------------------------------------------
# Utilities for collecting the dependency graph
# ------------------------------------------------------------------------------------------------

# We could not accumulate multiple lines in YB_ALL_DEPS due to a CMake bug with cache variables,
# but we can write multiple lines in the output file.
string(REPLACE "\\n" "\n" YB_ALL_DEPS_FINAL "${YB_ALL_DEPS}")
# This file wlll contains dependencies in the following form, separated by newlines:
# target: dep1;dep2;..;depN
# The same target can appear on the left hand side of multiple such lines.
file(WRITE "${YB_BUILD_ROOT}/yb_cmake_deps.txt" "${YB_ALL_DEPS_FINAL}")

math(EXPR YB_NUM_EXCLUDED_TESTS "${YB_NUM_TESTS} - ${YB_NUM_INCLUDED_TESTS}")
message("Total tests: ${YB_NUM_TESTS}, "
        "included: ${YB_NUM_INCLUDED_TESTS}, "
        "excluded: ${YB_NUM_EXCLUDED_TESTS}")
if(NOT ${BUILD_TESTS} AND NOT ${YB_NUM_INCLUDED_TESTS} STREQUAL "0")
  message(FATAL_ERROR "BUILD_TESTS is not set, but we still added ${YB_NUM_INCLUDED_TESTS} tests")
endif()

math(EXPR YB_NUM_EXCLUDED_EXECUTABLES "${YB_NUM_EXECUTABLES} - ${YB_NUM_INCLUDED_EXECUTABLES}")
message("Total non-test executables: ${YB_NUM_EXECUTABLES}, "
        "included: ${YB_NUM_INCLUDED_EXECUTABLES}, "
        "excluded: ${YB_NUM_EXCLUDED_EXECUTABLES}")
