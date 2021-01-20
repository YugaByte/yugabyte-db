// Copyright (c) YugaByte, Inc.
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

#include "yb/util/pg_util.h"

#include <string>

#include "yb/util/format.h"
#include "yb/util/hash_util.h"

namespace yb {

// Derive a socket directory name for postgres index backfill tserver-postgres authentication
// purposes.  Since the socket itself is named with the port number, the directory holding it must
// be unique with respect to the bind address in order to avoid conflicts with other clusters
// listening on different addresses but same ports.  To avoid the 108 character limit for the socket
// path, use a hash of the bind address.
std::string PgDeriveSocketDir(const std::string& host) {
  return Format("/tmp/.yb.$0", HashUtil::MurmurHash2_64(host.c_str(), host.size(), 0 /* seed */));
}

} // namespace yb
