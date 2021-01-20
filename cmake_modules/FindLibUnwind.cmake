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
# - Find libunwind (libunwind.h, libunwind.so)
#
# This module defines
#  UNWIND_INCLUDE_DIR, directory containing headers
#  UNWIND_SHARED_LIB, path to libunwind's shared library
#  UNWIND_STATIC_LIB, path to libunwind's static library

# For some reason, we have to link to two libunwind shared object files:
# one arch-specific and one not.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
  SET(LIBUNWIND_ARCH "arm")
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64")
  SET(LIBUNWIND_ARCH "x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^i.86$")
  SET(LIBUNWIND_ARCH "x86")
endif()

if(NOT DEFINED UNWIND_HAS_ARCH_SPECIFIC_LIB)
  set(UNWIND_HAS_ARCH_SPECIFIC_LIB TRUE)
endif()

find_path(UNWIND_INCLUDE_DIR libunwind.h
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

find_library(UNWIND_SHARED_LIB unwind
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

if(UNWIND_HAS_ARCH_SPECIFIC_LIB)
  find_library(UNWIND_SHARED_ARCH_LIB unwind-${LIBUNWIND_ARCH}
    NO_CMAKE_SYSTEM_PATH
    NO_SYSTEM_ENVIRONMENT_PATH)
endif()

find_library(UNWIND_STATIC_LIB libunwind.a
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

if(UNWIND_HAS_ARCH_SPECIFIC_LIB)
  find_library(UNWIND_STATIC_ARCH_LIB libunwind-${LIBUNWIND_ARCH}.a
    NO_CMAKE_SYSTEM_PATH
    NO_SYSTEM_ENVIRONMENT_PATH)
endif()

include(FindPackageHandleStandardArgs)

set(_UNWIND_REQUIRED_VARS
    UNWIND_SHARED_LIB
    UNWIND_STATIC_LIB
    UNWIND_INCLUDE_DIR)
if(UNWIND_HAS_ARCH_SPECIFIC_LIB)
  list(APPEND _UNWIND_REQUIRED_VARS
       UNWIND_SHARED_ARCH_LIB
       UNWIND_STATIC_ARCH_LIB)
endif()

find_package_handle_standard_args(UNWIND REQUIRED_VARS ${_UNWIND_REQUIRED_VARS})
