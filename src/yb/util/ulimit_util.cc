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

#include "yb/util/ulimit_util.h"

#include <sys/resource.h>

#include <algorithm>
#include <string>
#include <sstream>

#include <gflags/gflags.h>

#include "yb/util/env.h"
#include "yb/util/size_literals.h"
#include "yb/util/ulimit.h"

using std::string;
using std::stringstream;
using yb::operator"" _KB;
using yb::operator"" _MB;

// These flags allow operators to define process resource soft limits at runtime. Note that on some
// systems, RLIM_INFINITY is defined as -1, and setting these flags to that value will result in an
// attempt to set these resource limits to infinity. All other negative values are ignored.
DEFINE_int64(rlimit_data, RLIM_INFINITY, "Data file size limit: bytes.");
#if defined(__APPLE__)
// Note that we've chosen 10240 as the default value here since this is the default system limit
// for this resource on *macOS* as defined by OPEN_MAX in <sys/syslimits.h>
DEFINE_int64(rlimit_nofile, 10240, "Open files limit.");
#else
DEFINE_int64(rlimit_nofile, 1048576, "Open files limit.");
#endif
DEFINE_int64(rlimit_fsize, RLIM_INFINITY, "File size limit: blocks.");
DEFINE_int64(rlimit_memlock, 64_KB, "Locked memory limit: bytes.");
DEFINE_int64(rlimit_as, RLIM_INFINITY, "Memory size limit: bytes.");
DEFINE_int64(rlimit_stack, 8_MB, "Stack size limit: bytes.");
DEFINE_int64(rlimit_cpu, RLIM_INFINITY, "CPU time limit: seconds.");
DEFINE_int64(rlimit_nproc, 12000, "User process limit.");

// Note: we've observed it can take a while to dump full cores, esp. on systems like kubernetes or
// consumer laptops. In order to avoid causing more usability issues, we disable this flag for now
// and accept the system default rather than setting this to RLIM_INFINITY as suggested in our
// onboarding docs.
// DEFINE_int64(rlimit_core, RLIM_INFINITY, "Core file size limit: blocks.");

namespace {

const std::map<int, const int64_t&> kRlimitsToInit = {
  // {RLIMIT_CORE, FLAGS_rlimit_core},
  {RLIMIT_DATA, FLAGS_rlimit_data},
  {RLIMIT_NOFILE, FLAGS_rlimit_nofile},
  {RLIMIT_FSIZE, FLAGS_rlimit_fsize},
  {RLIMIT_MEMLOCK, FLAGS_rlimit_memlock},
  {RLIMIT_AS, FLAGS_rlimit_as},
  {RLIMIT_STACK, FLAGS_rlimit_stack},
  {RLIMIT_CPU, FLAGS_rlimit_cpu},
  {RLIMIT_NPROC, FLAGS_rlimit_nproc},
};

const std::map<int, std::string> kRdescriptions = {
  {RLIMIT_CORE, "core file size"},
  {RLIMIT_DATA, "data seg size"},
  {RLIMIT_NOFILE, "open files"},
  {RLIMIT_FSIZE, "file size"},
  {RLIMIT_MEMLOCK, "max locked memory"},
  {RLIMIT_AS, "max memory size"},
  {RLIMIT_STACK, "stack size"},
  {RLIMIT_CPU, "cpu time"},
  {RLIMIT_NPROC, "max user processes"},
};

} // namespace

namespace yb {

static stringstream& getLimit(
    stringstream& ss, const std::string pfx, const std::string sfx, int resource, int rightshift) {
  ss << "ulimit: " << pfx << " ";
  const auto limits_or_status = Env::Default()->GetUlimit(resource);
  if (limits_or_status.ok()) {
    const ResourceLimit soft = limits_or_status->soft;
    if (soft.IsUnlimited()) {
      ss << "unlimited";
    } else {
      ss << (soft.RawValue() >> rightshift);
    }
    ss << "(";
    const ResourceLimit hard = limits_or_status->hard;
    if (hard.IsUnlimited()) {
      ss << "unlimited";
    } else {
      ss << (hard.RawValue() >> rightshift);
    }
    ss << ")";
  } else {
    ss << "-1";
  }
  ss << (sfx[0] ? " " : "") << sfx << "\n";

  return ss;
}

static string getCommandLineDescription(int resource) {
  if (resource == RLIMIT_CORE) {
    return "core file size";
  }
  if (resource == RLIMIT_DATA) {
    return "data seg size";
  }
  if (resource == RLIMIT_NOFILE) {
    return "open files";
  }
  if (resource == RLIMIT_FSIZE) {
    return "file size";
  }
#if !defined(__APPLE__)
  if (resource == RLIMIT_SIGPENDING) {
    return "pending signals";
  }
  if (resource == RLIMIT_LOCKS) {
    return "file locks";
  }
#endif
  if (resource == RLIMIT_MEMLOCK) {
    return "max locked memory";
  }
  if (resource == RLIMIT_AS) {
    return "max memory size";
  }
  if (resource == RLIMIT_STACK) {
    return "stack size";
  }
  if (resource == RLIMIT_CPU) {
    return "cpu time";
  }
  if (resource == RLIMIT_NPROC) {
    return "max user processes";
  }
  return "UNKNOWN";
}

string UlimitUtil::GetUlimitInfo() {
  stringstream ss;
  ss << "\n";
  getLimit(ss, getCommandLineDescription(RLIMIT_CORE), "blks", RLIMIT_CORE, 0);
  getLimit(ss, getCommandLineDescription(RLIMIT_DATA), "kb", RLIMIT_DATA, 10);
  getLimit(ss, getCommandLineDescription(RLIMIT_NOFILE), "", RLIMIT_NOFILE, 0);
  getLimit(ss, getCommandLineDescription(RLIMIT_FSIZE), "blks", RLIMIT_FSIZE, 0);
#if !defined(__APPLE__)
  getLimit(ss, getCommandLineDescription(RLIMIT_SIGPENDING), "", RLIMIT_SIGPENDING, 0);
  getLimit(ss, getCommandLineDescription(RLIMIT_LOCKS), "", RLIMIT_LOCKS, 0);
#endif
  getLimit(ss, getCommandLineDescription(RLIMIT_MEMLOCK), "kb", RLIMIT_MEMLOCK, 10);
  getLimit(ss, getCommandLineDescription(RLIMIT_AS), "kb", RLIMIT_AS, 10);
  getLimit(ss, getCommandLineDescription(RLIMIT_STACK), "kb", RLIMIT_STACK, 10);
  getLimit(ss, getCommandLineDescription(RLIMIT_CPU), "secs", RLIMIT_CPU, 0);
  getLimit(ss, getCommandLineDescription(RLIMIT_NPROC), "", RLIMIT_NPROC, 0);

  return ss.str();
}

void UlimitUtil::InitUlimits() {
  for (const auto& kv : kRlimitsToInit) {
    int resource_id = kv.first;
    const ResourceLimit min_soft_limit(kv.second);
    const string resource_name = getCommandLineDescription(resource_id);

    if (min_soft_limit.IsNegative()) {
      LOG(INFO)
          << "Skipping setrlimit for " << resource_name
          << " with negative specified min value " << min_soft_limit.ToString();
      continue;
    }

    const auto limits_or_status = Env::Default()->GetUlimit(resource_id);
    if (!limits_or_status.ok()) {
      LOG(ERROR) << "Unable to fetch hard limit for resource " << resource_name
                 << " Skipping initialization.";
      continue;
    }

    const ResourceLimit sys_soft_limit = limits_or_status->soft;
    if (min_soft_limit <= sys_soft_limit) {
      LOG(INFO)
          << "Configured soft limit for " << resource_name
          << " is already larger than specified min value (" << sys_soft_limit.ToString()
          << " vs. " << min_soft_limit.ToString() << "). Skipping.";
      continue;
    }

    const ResourceLimit sys_hard_limit = limits_or_status->hard;
    const ResourceLimit new_soft_limit = std::min(sys_hard_limit, min_soft_limit);

    Status set_ulim_status = Env::Default()->SetUlimit(resource_id, new_soft_limit, resource_name);
    if (!set_ulim_status.ok()) {
      LOG(ERROR) << "Unable to set new soft limit for resource " << resource_name
                 << " error: " << set_ulim_status.ToString();
    }
  }
}

}  // namespace yb
