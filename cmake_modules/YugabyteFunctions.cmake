# Copyright (c) Yugabyte, Inc.
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

# Various CMake functions. This should eventually be organized and refactored.

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
  if(NOT DEFINED IS_CLANG)
    message(FATAL_ERROR "IS_CLANG undefined")
  endif()
  if(NOT DEFINED IS_GCC)
    message(FATAL_ERROR "IS_GCC undefined")
  endif()
  if(NOT DEFINED COMPILER_VERSION)
    message(FATAL_ERROR "COMPILER_VERSION undefined")
  endif()
  if(NOT DEFINED COMPILER_FAMILY)
    message(FATAL_ERROR "COMPILER_FAMILY undefined")
  endif()

  # Detect Linuxbrew.
  #
  # TODO: consolidate Linuxbrew detection logic between here and detect_brew in common-build-env.sh.
  # As of 10/2020 we only check the compiler version here but not in detect_brew.
  set(USING_LINUXBREW FALSE)
  if(NOT APPLE AND
     # In practice, we only use Linuxbrew with Clang 7.x.
     (NOT IS_CLANG OR "${COMPILER_VERSION}" VERSION_LESS "8.0.0") AND
     # In practice, we only use Linuxbrew with GCC 5.x.
     (NOT IS_GCC OR "${COMPILER_VERSION}" VERSION_LESS "6.0.0"))
    message("Trying to detect whether we should use Linuxbrew. "
            "IS_CLANG=${IS_CLANG}, "
            "IS_GCC=${IS_GCC}, "
            "COMPILER_VERSION=${COMPILER_VERSION}, "
            "LINUXBREW_DIR=${LINUXBREW_DIR}")
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
function(INIT_COMPILER_TYPE_FROM_BUILD_ROOT)
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
  # Make sure we can use $ENV{YB_COMPILER_TYPE} and ${YB_COMPILER_TYPE} interchangeably.
  SET(YB_COMPILER_TYPE "$ENV{YB_COMPILER_TYPE}" PARENT_SCOPE)
endfunction()

# Makes sure that we are using a supported compiler family.
function(VALIDATE_COMPILER_TYPE)
  if ("$ENV{YB_COMPILER_TYPE}" STREQUAL "")
    set(ENV{YB_COMPILER_TYPE} "${COMPILER_FAMILY}")
  endif()

  if ("$ENV{YB_COMPILER_TYPE}" STREQUAL "zapcc")
    if (NOT "${COMPILER_FAMILY}" STREQUAL "clang")
      message(FATAL_ERROR
              "Compiler type is zapcc but the compiler family is '${COMPILER_FAMILY}' "
              "(expected to be clang)")
    endif()
  endif()

  if("${YB_COMPILER_TYPE}" MATCHES "^gcc.*$" AND NOT "${COMPILER_FAMILY}" STREQUAL "gcc")
    message(FATAL_ERROR
            "Compiler type '${YB_COMPILER_TYPE}' does not match the compiler family "
            "'${COMPILER_FAMILY}'.")
  endif()

  if(("${YB_COMPILER_TYPE}" STREQUAL "gcc8" AND NOT "${COMPILER_VERSION}" MATCHES "^8[.].*$") OR
     ("${YB_COMPILER_TYPE}" STREQUAL "gcc9" AND NOT "${COMPILER_VERSION}" MATCHES "^9[.].*$"))
    message(FATAL_ERROR
            "Invalid compiler version '${COMPILER_VERSION}' for compiler type "
            "'${YB_COMPILER_TYPE}'.")
  endif()

  if (NOT "${COMPILER_FAMILY}" STREQUAL "gcc" AND
      NOT "${COMPILER_FAMILY}" STREQUAL "clang")
    message(FATAL_ERROR "Unknown compiler family: ${COMPILER_FAMILY} (expected 'gcc' or 'clang').")
  endif()

  set(IS_CLANG FALSE PARENT_SCOPE)
  if ("${COMPILER_FAMILY}" STREQUAL "clang")
    set(IS_CLANG TRUE PARENT_SCOPE)
  endif()

  set(IS_GCC FALSE PARENT_SCOPE)
  if ("${COMPILER_FAMILY}" STREQUAL "gcc")
    set(IS_GCC TRUE PARENT_SCOPE)
  endif()
endfunction()

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
  if(NOT EXISTS "${RPATH_ENTRY}")
    message(
      WARNING
      "Adding a non-existent rpath directory '${RPATH_ENTRY}'. This might be OK in case the "
      "directory is created during the build.")
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

macro(YB_SETUP_CLANG THIRDPARTY_BUILD_TYPE)
  message("YB_SETUP_CLANG: THIRDPARTY_BUILD_TYPE=${THIRDPARTY_BUILD_TYPE}")
  ADD_CXX_FLAGS("-stdlib=libc++")

  # Disables using the precompiled template specializations for std::string, shared_ptr, etc
  # so that the annotations in the header actually take effect.
  ADD_CXX_FLAGS("-D_GLIBCXX_EXTERN_TEMPLATE=0")

  set(LIBCXX_DIR "${YB_THIRDPARTY_DIR}/installed/${THIRDPARTY_BUILD_TYPE}/libcxx")
  if(NOT EXISTS "${LIBCXX_DIR}")
    message(FATAL_ERROR "libc++ directory does not exist: '${LIBCXX_DIR}'")
  endif()
  set(LIBCXX_INCLUDE_DIR "${LIBCXX_DIR}/include/c++/v1")
  if(NOT EXISTS "${LIBCXX_INCLUDE_DIR}")
    message(FATAL_ERROR "libc++ include directory does not exist: '${LIBCXX_INCLUDE_DIR}'")
  endif()
  ADD_GLOBAL_RPATH_ENTRY("${LIBCXX_DIR}/lib")

  # This needs to appear before adding third-party dependencies that have their headers in the
  # Linuxbrew include directory, because otherwise we'll pick up the standard library headers from
  # the Linuxbrew include directory too.
  include_directories(SYSTEM "${LIBCXX_INCLUDE_DIR}")

  ADD_CXX_FLAGS("-nostdinc++")
  ADD_LINKER_FLAGS("-L${LIBCXX_DIR}/lib")
  if(NOT EXISTS "${LIBCXX_DIR}/lib")
    message(FATAL_ERROR "libc++ library directory does not exist: '${LIBCXX_DIR}/lib'")
  endif()
endmacro()

# This is a macro because we need to call functions that set flags on the parent scope.
macro(YB_SETUP_SANITIZER)
  if(NOT "${YB_BUILD_TYPE}" MATCHES "^(asan|tsan)$")
    message(
      FATAL_ERROR
      "YB_SETUP_SANITIZER can only be invoked for asan/tsan build types. "
      "Build type: ${YB_BUILD_TYPE}.")
  endif()

  if("${COMPILER_FAMILY}" STREQUAL "clang")
    message("Using instrumented libc++ (build type: ${YB_BUILD_TYPE})")
    YB_SETUP_CLANG("${YB_BUILD_TYPE}")
  else()
    message("Not using ${SANITIZER}-instrumented standard C++ library for compiler family "
            "${COMPILER_FAMILY} yet.")
  endif()

  if("${YB_BUILD_TYPE}" STREQUAL "asan")
    if("${COMPILER_FAMILY}" STREQUAL "clang" AND
       "${COMPILER_VERSION}" VERSION_GREATER_EQUAL "10.0.0" AND
       NOT APPLE)
      # TODO: see if we can use static libasan instead (requires third-party changes).
      ADD_CXX_FLAGS("-shared-libasan")
      ADD_LINKER_FLAGS("-lunwind")

      # TODO: this is mostly needed because we depend on the ASAN runtime shared library and that
      # depends on libc++ but does not have the rpath set correctly, so we have to add our own
      # dependency on libc++ so it gets resolved using our rpath.
      ADD_LINKER_FLAGS("-lc++")

      execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-search-dirs
        OUTPUT_VARIABLE CLANG_PRINT_SEARCH_DIRS_OUTPUT)
      if ("${CLANG_PRINT_SEARCH_DIRS_OUTPUT}" MATCHES ".*libraries: =([^:]+)(:.*|$)" )
        set(CLANG_RUNTIME_LIB_DIR "${CMAKE_MATCH_1}/lib/linux")
        if(NOT EXISTS "${CLANG_RUNTIME_LIB_DIR}")
          message(FATAL_ERROR "Clang runtime directory does not exist: ${CLANG_RUNTIME_LIB_DIR}")
        endif()
        ADD_GLOBAL_RPATH_ENTRY("${CLANG_RUNTIME_LIB_DIR}")
      else()
        message(FATAL_ERROR
                "Could not parse the output of 'clang -print-search-dirs': "
                "${CLANG_PRINT_SEARCH_DIRS_OUTPUT}")
      endif()
    endif()

    ADD_CXX_FLAGS("-fsanitize=address")
    ADD_CXX_FLAGS("-DADDRESS_SANITIZER")

    # Compile and link against the thirdparty ASAN instrumented libstdcxx.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    if("${COMPILER_FAMILY}" STREQUAL "gcc")
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lubsan -ldl")
      ADD_CXX_FLAGS("-Wno-error=maybe-uninitialized")
    endif()
  elseif("${YB_BUILD_TYPE}" STREQUAL "tsan")
    ADD_CXX_FLAGS("-fsanitize=thread")

    # Enables dynamic_annotations.h to actually generate code
    ADD_CXX_FLAGS("-DDYNAMIC_ANNOTATIONS_ENABLED")

    # changes atomicops to use the tsan implementations
    ADD_CXX_FLAGS("-DTHREAD_SANITIZER")

    # Compile and link against the thirdparty TSAN instrumented libstdcxx.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
  else()
    message(FATAL_ERROR "Invalid build type for YB_SETUP_SANITIZER: '${YB_BUILD_TYPE}'")
  endif()
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

# A wrapper around add_library() for YugabyteDB libraries.
#
# Required arguments:
#
# LIB_NAME is the name of the library. It must come first.
# SRCS is the list of source files to compile into the library.
# DEPS is the list of targets that both library variants depend on.
#
# Optional arguments:
#
# NONLINK_DEPS is the list of (non-linked) targets that the library depends on.
# COMPILE_FLAGS is a string containing any additional compilation flags that should be added to the
# library.
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

# Import a shared library (e.g. libpq) that is built as part of PostgreSQL as a CMake target.
# See https://github.com/yugabyte/yugabyte-db/issues/5853: we currently have to run this function
# separately for each library from each CMakeLists.txt file that uses that library.
function(ADD_POSTGRES_SHARED_LIBRARY LIB_NAME SHARED_LIB_PATH)
  if ("${SHARED_LIB_PATH}" STREQUAL "")
    message(FATAL_ERROR
            "Shared library path cannot be empty. "
            "LIB_NAME=${LIB_NAME} "
            "CMAKE_CURRENT_LIST_DIR=${CMAKE_CURRENT_LIST_DIR}")
  endif()
  add_library(${LIB_NAME} SHARED IMPORTED)
  set_target_properties(${LIB_NAME} PROPERTIES IMPORTED_LOCATION "${SHARED_LIB_PATH}")
  # "postgres" is the target that actually builds this shared library.
  add_dependencies(${LIB_NAME} postgres)
  message("Added target ${LIB_NAME} for a shared library built as part of PostgreSQL code: "
          "${SHARED_LIB_PATH} (invoked from ${CMAKE_CURRENT_LIST_FILE})")
endfunction()

function(parse_build_root_basename)
  get_filename_component(YB_BUILD_ROOT_BASENAME "${CMAKE_CURRENT_BINARY_DIR}" NAME)
  string(REPLACE "-" ";" YB_BUILD_ROOT_BASENAME_COMPONENTS ${YB_BUILD_ROOT_BASENAME})
  list(LENGTH YB_BUILD_ROOT_BASENAME_COMPONENTS YB_BUILD_ROOT_BASENAME_COMPONENTS_LENGTH)
  if(YB_BUILD_ROOT_BASENAME_COMPONENTS_LENGTH LESS 3 OR
     YB_BUILD_ROOT_BASENAME_COMPONENTS_LENGTH GREATER 4)
    message(
        FATAL_ERROR
        "Wrong number of components of the build root basename: "
        "${YB_BUILD_ROOT_BASENAME_COMPONENTS_LENGTH}. Expected 3 or 4 components. "
        "Basename: ${YB_BUILD_ROOT_BASENAME}")
  endif()
  list(GET YB_BUILD_ROOT_BASENAME_COMPONENTS 0 YB_BUILD_TYPE)
  set(YB_BUILD_TYPE "${YB_BUILD_TYPE}" PARENT_SCOPE)
  if(NOT "${YB_COMPILER_TYPE}" STREQUAL "" AND
     NOT "${YB_COMPILER_TYPE}" STREQUAL "${YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME}")
    message(
        FATAL_ERROR
        "The YB_COMPILER_TYPE CMake variable is already set to '${YB_COMPILER_TYPE}', but the "
        "value auto-detected from the build root basename '${YB_BUILD_ROOT_BASENAME}' is "
        "different: '${YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME}'.")
  endif()

  list(GET YB_BUILD_ROOT_BASENAME_COMPONENTS 1 YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME)
  if(NOT "$ENV{YB_COMPILER_TYPE}" STREQUAL "" AND
     NOT "$ENV{YB_COMPILER_TYPE}" STREQUAL "${YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME}")
    message(
        FATAL_ERROR
        "The YB_COMPILER_TYPE environment variable is already set to '${YB_COMPILER_TYPE}', but "
        "the value auto-detected from the build root basename '${YB_BUILD_ROOT_BASENAME}' is "
        "different: '${YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME}'.")
  endif()
  set(YB_COMPILER_TYPE "${YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME}" PARENT_SCOPE)
  set(ENV{YB_COMPILER_TYPE} "${YB_COMPILER_TYPE_FROM_BUILD_ROOT_BASENAME}")

  list(GET YB_BUILD_ROOT_BASENAME_COMPONENTS 2 YB_LINKING_TYPE)
  if(NOT "${YB_LINKING_TYPE}" MATCHES "^(static|dynamic)$")
    message(
        FATAL_ERROR
        "Invalid linking type from the build root basename '${YB_BUILD_ROOT_BASENAME}': "
        "'${YB_LINKING_TYPE}'. Expected 'static' or 'dynamic'.")
  endif()
  set(YB_LINKING_TYPE "${YB_LINKING_TYPE}" PARENT_SCOPE)
endfunction()
