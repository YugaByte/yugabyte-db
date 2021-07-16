// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_UTIL_ERRNO_H
#define YB_UTIL_ERRNO_H

#include <string>

#include "yb/util/logging.h"
#include "yb/util/status.h"

namespace yb {

void ErrnoToCString(int err, char *buf, size_t buf_len);

// Return a string representing an errno.
inline std::string ErrnoToString(int err) {
  char buf[512];
  ErrnoToCString(err, buf, sizeof(buf));
  return std::string(buf);
}

inline void LogFatalForFallocateFailure(int alloc_status, int err) {
  if (alloc_status < 0) {
    if (err == EOPNOTSUPP) {
      YB_LOG_FIRST_N(FATAL, 1) << "The filesystem does not support fallocate().";
    } else if (err == ENOSYS) {
      YB_LOG_FIRST_N(FATAL, 1) << "The kernel does not implement fallocate().";
    } else {
      YB_LOG_FIRST_N(FATAL, 1) << "fallocate() failed.";
    }
  }
}

struct ErrnoTag : IntegralErrorTag<int32_t> {
  // This category id is part of the wire protocol and should not be changed once released.
  static constexpr uint8_t kCategory = 1;

  static std::string ToMessage(Value value) {
    return ErrnoToString(value);
  }
};

typedef StatusErrorCodeImpl<ErrnoTag> Errno;

} // namespace yb

#endif // YB_UTIL_ERRNO_H
