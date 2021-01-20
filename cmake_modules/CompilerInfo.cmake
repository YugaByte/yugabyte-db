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
# Sets COMPILER_FAMILY to 'clang' or 'gcc'
# Sets COMPILER_VERSION to the version
message("YB_COMPILER_TYPE env var: $ENV{YB_COMPILER_TYPE}")

if("$ENV{YB_COMPILER_TYPE}" STREQUAL "")
  message(FATAL_ERROR "YB_COMPILER_TYPE environment variable is empty or undefined")
endif()

if("${YB_COMPILER_TYPE}" STREQUAL "")
  message(FATAL_ERROR "YB_COMPILER_TYPE CMake variable is empty or undefined")
endif()

if(NOT "$ENV{YB_COMPILER_TYPE}" STREQUAL "${YB_COMPILER_TYPE}")
  message(FATAL_ERROR
          "Values of the YB_COMPILER_TYPE environment variable ($ENV{YB_COMPILER_TYPE})"
          "and the YB_COMPILER_TYPE CMake variable (${YB_COMPILER_TYPE}) do not match.")
endif()

message("CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -v
                ERROR_VARIABLE COMPILER_VERSION_FULL)
message("Compiler version information:\n${COMPILER_VERSION_FULL}")

if("${COMPILER_VERSION_FULL}" MATCHES ".*clang version ([0-9]+([.][0-9]+)*)[ -].*")
  set(COMPILER_FAMILY "clang")
  set(COMPILER_VERSION "${CMAKE_MATCH_1}")
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*gcc [(]GCC[)] ([0-9]+([.][0-9]+)*)[ -].*")
  # E.g. gcc (GCC) 8.3.1 20190311 (Red Hat 8.3.1-3)
  set(COMPILER_FAMILY "gcc")
  set(COMPILER_VERSION "${CMAKE_MATCH_1}")
# clang on Linux and Mac OS X before 10.9
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*clang version.*")
  set(COMPILER_FAMILY "clang")
  string(REGEX REPLACE ".*clang version ([0-9]+\\.[0-9]+).*" "\\1"
    COMPILER_VERSION "${COMPILER_VERSION_FULL}")
# clang on Mac OS X 10.9 and later
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*based on LLVM.*")
  set(COMPILER_FAMILY "clang")
  string(REGEX REPLACE ".*based on LLVM ([0-9]+\\.[0.9]+).*" "\\1"
    COMPILER_VERSION "${COMPILER_VERSION_FULL}")

# clang on Mac OS X, XCode 7.
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*clang-700\\..*")
  set(COMPILER_FAMILY "clang")
  set(COMPILER_VERSION "3.7.0svn")

# a different version of clang
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*[(]clang-[0-9.]+[)].*")
  set(COMPILER_FAMILY "clang")
  string(REGEX REPLACE ".*[(]clang-([0-9.]+)[)].*" "\\1"
    COMPILER_VERSION "${COMPILER_VERSION_FULL}")

# gcc
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*gcc version 8.*")
  set(COMPILER_FAMILY "gcc")
  string(REGEX REPLACE ".*gcc version ([0-9\\.]+).*" "\\1"
    COMPILER_VERSION "${COMPILER_VERSION_FULL}")
elseif("${COMPILER_VERSION_FULL}" MATCHES ".*gcc version.*")
  set(COMPILER_FAMILY "gcc")
  string(REGEX REPLACE ".*gcc version ([0-9\\.]+).*" "\\1"
    COMPILER_VERSION "${COMPILER_VERSION_FULL}")
else()
  message(FATAL_ERROR "Unknown compiler. Version info:\n${COMPILER_VERSION_FULL}")
endif()
message("Selected compiler family '${COMPILER_FAMILY}', version '${COMPILER_VERSION}'")
