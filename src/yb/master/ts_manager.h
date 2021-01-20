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
#ifndef YB_MASTER_TS_MANAGER_H
#define YB_MASTER_TS_MANAGER_H

#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/function.hpp>

#include "yb/gutil/thread_annotations.h"
#include "yb/gutil/macros.h"

#include "yb/util/capabilities.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/status.h"

#include "yb/common/common.pb.h"

#include "yb/rpc/rpc_fwd.h"

#include "yb/master/master_fwd.h"

namespace yb {

class NodeInstancePB;

namespace master {

typedef std::string TabletServerId;
typedef std::unordered_set<HostPort, HostPortHash> BlacklistSet;

// A callback that is called when the number of tablet servers reaches a certain number.
typedef boost::function<void()> TSCountCallback;

// Tracks the servers that the master has heard from, along with their
// last heartbeat, etc.
//
// Note that TSDescriptors are never deleted, even if the TS crashes
// and has not heartbeated in quite a while. This makes it simpler to
// keep references to TSDescriptors elsewhere in the master without
// fear of lifecycle problems. Dead servers are "dead, but not forgotten"
// (they live on in the heart of the master).
//
// This class is thread-safe.
class TSManager {
 public:
  TSManager();
  virtual ~TSManager();

  // Lookup the tablet server descriptor for the given instance identifier.
  // If the TS has never registered, or this instance doesn't match the
  // current instance ID for the TS, then a NotFound status is returned.
  // Otherwise, *desc is set and OK is returned.
  CHECKED_STATUS LookupTS(const NodeInstancePB& instance,
                          TSDescriptorPtr* desc);

  // Lookup the tablet server descriptor for the given UUID.
  // Returns false if the TS has never registered.
  // Otherwise, *desc is set and returns true.
  bool LookupTSByUUID(const std::string& uuid,
                      TSDescriptorPtr* desc);

  // Register or re-register a tablet server with the manager.
  //
  // If successful, *desc reset to the registered descriptor.
  CHECKED_STATUS RegisterTS(const NodeInstancePB& instance,
                            const TSRegistrationPB& registration,
                            CloudInfoPB local_cloud_info,
                            rpc::ProxyCache* proxy_cache,
                            RegisteredThroughHeartbeat registered_through_heartbeat =
                                RegisteredThroughHeartbeat::kTrue);

  // Return all of the currently registered TS descriptors into the provided list.
  void GetAllDescriptors(TSDescriptorVector* descs) const;
  TSDescriptorVector GetAllDescriptors() const;

  // Return all of the currently registered TS descriptors that have sent a
  // heartbeat recently, indicating that they're alive and well.
  // Optionally pass in blacklist as a set of HostPorts to return all live non-blacklisted servers.
  void GetAllLiveDescriptors(TSDescriptorVector* descs,
                             const BlacklistSet blacklist = BlacklistSet()) const;

  // Return all of the currently registered TS descriptors that have sent a heartbeat
  // recently and are in the same 'cluster' with given placement uuid.
  // Optionally pass in blacklist as a set of HostPorts to return all live non-blacklisted servers.
  void GetAllLiveDescriptorsInCluster(TSDescriptorVector* descs, string placement_uuid,
                                      const BlacklistSet blacklist = BlacklistSet(),
                                      bool primary_cluster = true) const;

  // Return all of the currently registered TS descriptors that have sent a
  // heartbeat, indicating that they're alive and well, recently and have given
  // full report of their tablets as well.
  void GetAllReportedDescriptors(TSDescriptorVector* descs) const;

  // Return the tablet server descriptor running on the given port.
  const TSDescriptorPtr GetTSDescriptor(const HostPortPB& host_port) const;

  // Check if the placement uuid of the tserver is same as given cluster uuid.
  static bool IsTsInCluster(const TSDescriptorPtr& ts, string cluster_uuid);

  static bool IsTsBlacklisted(const TSDescriptorPtr& ts,
                              const BlacklistSet blacklist);

  // Register a callback to be called when the number of tablet servers reaches a certain number.
  // The callback is removed after it is called once.
  void SetTSCountCallback(int min_count, TSCountCallback callback);

 private:
  void GetDescriptors(std::function<bool(const TSDescriptorPtr&)> condition,
                      TSDescriptorVector* descs) const;


  void GetAllDescriptorsUnlocked(TSDescriptorVector* descs) const REQUIRES_SHARED(lock_);
  void GetDescriptorsUnlocked(std::function<bool(const TSDescriptorPtr&)> condition,
                      TSDescriptorVector* descs) const REQUIRES_SHARED(lock_);

  int GetCountUnlocked() const REQUIRES_SHARED(lock_);

  mutable rw_spinlock lock_;

  typedef std::unordered_map<std::string, TSDescriptorPtr> TSDescriptorMap;
  TSDescriptorMap servers_by_id_ GUARDED_BY(lock_);

  // This callback will be called when the number of tablet servers reaches the given number.
  TSCountCallback ts_count_callback_ GUARDED_BY(lock_);
  int ts_count_callback_min_count_ GUARDED_BY(lock_) = 0;

  DISALLOW_COPY_AND_ASSIGN(TSManager);
};

} // namespace master
} // namespace yb

#endif // YB_MASTER_TS_MANAGER_H
