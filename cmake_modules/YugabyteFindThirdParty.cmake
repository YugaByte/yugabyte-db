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

# Finding various third-party dependencies, mostly using find_package.

#[[

We are setting FPHSA_NAME_MISMATCHED to TRUE above to avoid warnings like this. We get a lot of
these and they are probably not a problem. Also they might be happening more frequently on macOS due
to its case-insensitive file system.

  The package name passed to `find_package_handle_standard_args` (GLOG) does
  not match the name of the calling package (GLog).  This can lead to
  problems in calling code that expects `find_package` result variables
  (e.g., `_FOUND`) to follow a certain pattern.
Call Stack (most recent call first):
  cmake_modules/FindGLog.cmake:35 (find_package_handle_standard_args)
  cmake_modules/YugabyteFindThirdPartyPackages.cmake:26 (find_package)
  CMakeLists.txt:966 (include)
This warning is for project developers.  Use -Wno-dev to suppress it.

]]

set(FPHSA_NAME_MISMATCHED TRUE)

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

if (NOT APPLE)
  ## libunwind (dependent of glog)
  ## Doesn't build on OSX.
  if(IS_CLANG AND "${COMPILER_VERSION}" VERSION_GREATER_EQUAL "10.0.0")
    set(UNWIND_HAS_ARCH_SPECIFIC_LIB FALSE)
  else()
    set(UNWIND_HAS_ARCH_SPECIFIC_LIB TRUE)
  endif()
  find_package(LibUnwind REQUIRED)
  include_directories(SYSTEM ${UNWIND_INCLUDE_DIR})
  ADD_THIRDPARTY_LIB(unwind
    STATIC_LIB "${UNWIND_STATIC_LIB}"
    SHARED_LIB "${UNWIND_SHARED_LIB}")
  if(UNWIND_HAS_ARCH_SPECIFIC_LIB)
    ADD_THIRDPARTY_LIB(unwind-arch
        STATIC_LIB "${UNWIND_STATIC_ARCH_LIB}"
        SHARED_LIB "${UNWIND_SHARED_ARCH_LIB}")
  endif()
  list(APPEND YB_BASE_LIBS unwind)

  ## libuuid -- also only needed on Linux as it is part of system libraries on macOS.
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

# -------------------------------------------------------------------------------------------------
# Deciding whether to use tcmalloc
# -------------------------------------------------------------------------------------------------

# Do not use tcmalloc for ASAN/TSAN but also temporarily for gcc8 and gcc9, because initdb crashes
# with bad deallocation with those compilers. That needs to be properly investigated.
if ("${YB_TCMALLOC_ENABLED}" STREQUAL "")
  if ("${YB_BUILD_TYPE}" MATCHES "^(asan|tsan)$")
    set(YB_TCMALLOC_ENABLED "0")
    message("Not using tcmalloc due to build type ${YB_BUILD_TYPE}")
  else()
    set(YB_TCMALLOC_ENABLED "1")
    message("Using tcmalloc due to build type ${YB_BUILD_TYPE}")
  endif()
else()
  if (NOT "${YB_TCMALLOC_ENABLED}" MATCHES "^[01]$")
    message(FATAL_ERROR
            "YB_TCMALLOC_ENABLED has an invalid value '${YB_TCMALLOC_ENABLED}'. Can be 0, 1, or "
            "empty (undefined).")
  endif()

  message("YB_TCMALLOC_ENABLED is already set to '${YB_TCMALLOC_ENABLED}'")
endif()

if ("${YB_TCMALLOC_ENABLED}" STREQUAL "1")
  message("Using tcmalloc")
  ## Google PerfTools
  ##
  find_package(GPerf REQUIRED)

  ADD_THIRDPARTY_LIB(tcmalloc
    STATIC_LIB "${TCMALLOC_STATIC_LIB}"
    SHARED_LIB "${TCMALLOC_SHARED_LIB}")
  ADD_THIRDPARTY_LIB(profiler
    STATIC_LIB "${PROFILER_STATIC_LIB}"
    SHARED_LIB "${PROFILER_SHARED_LIB}")
  list(APPEND YB_BASE_LIBS tcmalloc profiler)
  ADD_CXX_FLAGS("-DTCMALLOC_ENABLED")
else()
  message("Not using tcmalloc, YB_TCMALLOC_ENABLED is '${YB_TCMALLOC_ENABLED}'")
endif()

#
# -------------------------------------------------------------------------------------------------

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

# Workaround for
# http://stackoverflow.com/questions/9948375/cmake-find-package-succeeds-but-returns-wrong-path
set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_SYSTEM_PATHS ON)

if("${YB_COMPILER_TYPE}" MATCHES "^gcc[0-9]+$")
  # TODO: display this only if using a devtoolset compiler on CentOS, and ideally only if the error
  # actually happens.
  message("Note: if Boost fails to find Threads, you might need to install the "
          "devtoolset-N-libatomic-devel package for the devtoolset you are using.")
endif()

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
