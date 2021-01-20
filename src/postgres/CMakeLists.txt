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


set(POSTGRES_EXTRA_C_CXX_FLAGS "")
get_property(yb_cmake_include_dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY INCLUDE_DIRECTORIES)
foreach(include_dir ${yb_cmake_include_dirs})
  set(POSTGRES_EXTRA_C_CXX_FLAGS "${POSTGRES_EXTRA_C_CXX_FLAGS} -I${include_dir}")
endforeach(include_dir)
if("${COMPILER_FAMILY}" STREQUAL "gcc" AND "${COMPILER_VERSION}" VERSION_GREATER "8.0")
  string(CONCAT POSTGRES_EXTRA_C_CXX_FLAGS "${POSTGRES_EXTRA_C_CXX_FLAGS}"
         " -Wno-format-truncation"
         " -Wno-maybe-uninitialized"
         " -Wno-stringop-truncation")
endif()

set(POSTGRES_EXTRA_LD_FLAGS "")
if(NOT APPLE AND
   "${YB_BUILD_TYPE}" STREQUAL "asan" AND
   "${COMPILER_FAMILY}" STREQUAL "clang" AND
   "${COMPILER_VERSION}" VERSION_GREATER_EQUAL "10.0.0")
  set(POSTGRES_EXTRA_LD_FLAGS "${POSTGRES_EXTRA_LD_FLAGS} -ldl")
endif()

get_filename_component(OPENSSL_LIB_DIR "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)

set(build_postgres_args
    "${BUILD_SUPPORT_DIR}/build_postgres"
      --build_root          "${YB_BUILD_ROOT}"
      --cflags              "${POSTGRES_EXTRA_C_CXX_FLAGS} ${CMAKE_C_FLAGS}"
      --compiler_type       "${YB_COMPILER_TYPE}"
      --cxxflags            "${POSTGRES_EXTRA_C_CXX_FLAGS} ${CMAKE_CXX_FLAGS}"
      --ldflags             " ${CMAKE_SHARED_LINKER_FLAGS} ${POSTGRES_EXTRA_LD_FLAGS}"
      --ldflags_ex          " ${CMAKE_EXE_LINKER_FLAGS}"
      --openssl_include_dir "${OPENSSL_INCLUDE_DIR}"
      --openssl_lib_dir     "${OPENSSL_LIB_DIR}"
      --thirdparty_dir      "${YB_THIRDPARTY_DIR}")

if (NOT DEFINED LIBPQ_SHARED_LIB)
  message(FATAL_ERROR "LIBPQ_SHARED_LIB must be defined")
endif()
if (NOT DEFINED YB_PGBACKEND_SHARED_LIB)
  message(FATAL_ERROR "YB_PGBACKEND_SHARED_LIB must be defined")
endif()

# Invoke our PostgreSQL build script in two steps: "configure" and "make". The "configure" step
# does not have any dependencies so it can run in parallel with e.g. C++ code compilation.
#
# We add spaces before every "flags" parameter so that Python's argparse does not interpret them as
# its own flags.
add_custom_target(configure_postgres ALL COMMAND ${build_postgres_args} --step configure)
add_custom_target(postgres ALL COMMAND ${build_postgres_args} --step make
                           BYPRODUCTS "${LIBPQ_SHARED_LIB}" "${YB_PGBACKEND_SHARED_LIB}")
add_dependencies(postgres configure_postgres)

# ------------------------------------------------------------------------------------------------
# Dependencies between postgres and other targets
# ------------------------------------------------------------------------------------------------

if("${YB_BUILD_TYPE}" STREQUAL "compilecmds")
  message("Not making postgres depend on other libraries in the compilecmds build type.")
else()
  # Libraries that we link into the postgres binary.
  add_dependencies(postgres yb_pggate server_process)
endif()
