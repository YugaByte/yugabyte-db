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

#ifndef YB_MASTER_UTIL_YQL_VTABLE_HELPERS_H
#define YB_MASTER_UTIL_YQL_VTABLE_HELPERS_H

#include <future>

#include "yb/master/master.pb.h"

#include "yb/util/net/net_fwd.h"
#include "yb/util/net/inetaddress.h"
#include "yb/util/result.h"
#include "yb/util/uuid.h"

namespace yb {
namespace master {
namespace util {

template<class T> struct GetValueHelper;

// In some cases we need to preserve the QLValuePB.
template<> struct GetValueHelper<QLValuePB> {
  static const QLValuePB& Apply(const QLValuePB& value_pb, const DataType data_type);
};

template<> struct GetValueHelper<std::string> {
  static QLValuePB Apply(const std::string& strval, const DataType data_type);
  static QLValuePB Apply(const char* strval, size_t len, const DataType data_type);
};

// Need specialization for char[N] to handle strings literals.
template<std::size_t N> struct GetValueHelper<char[N]> {
  static QLValuePB Apply(const char* strval, const DataType data_type) {
    return GetValueHelper<std::string>::Apply(strval, N - 1, data_type);
  }
};

template<> struct GetValueHelper<int32_t> {
  static QLValuePB Apply(int32_t intval, DataType data_type);
};

template<> struct GetValueHelper<InetAddress> {
  static QLValuePB Apply(const InetAddress& inet_val, const DataType data_type);
};

template<> struct GetValueHelper<Uuid> {
  static QLValuePB Apply(const Uuid& uuid_val, const DataType data_type);
};

template<> struct GetValueHelper<bool> {
  static QLValuePB Apply(const bool bool_val, const DataType data_type);
};

template<class T>
QLValuePB GetValue(const T& t, DataType data_type) {
  typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type CleanedT;
  return GetValueHelper<CleanedT>::Apply(t, data_type);
}

QLValuePB GetTokensValue(size_t index, size_t node_count);

QLValuePB GetReplicationValue(int replication_factor);

bool RemoteEndpointMatchesTServer(const TSInformationPB& ts_info,
                                  const InetAddress& remote_endpoint);

struct PublicPrivateIPFutures {
  std::shared_future<Result<IpAddress>> private_ip_future;
  std::shared_future<Result<IpAddress>> public_ip_future;
};

PublicPrivateIPFutures GetPublicPrivateIPFutures(
    const TSInformationPB& ts_info, DnsResolver* resolver);

}  // namespace util
}  // namespace master
}  // namespace yb

#endif // YB_MASTER_UTIL_YQL_VTABLE_HELPERS_H
