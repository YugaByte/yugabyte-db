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

#include "yb/master/util/yql_vtable_helpers.h"

#include <boost/container/small_vector.hpp>

#include "yb/common/ql_value.h"

#include "yb/util/yb_partition.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/net/net_util.h"

namespace yb {
namespace master {
namespace util {

// Ideally, we want clients to use YB's own load-balancing policy for Cassandra to route the
// requests to the respective nodes hosting the partition keys. But for clients using vanilla
// drivers and thus Cassandra's own token-aware policy, we still want the requests to hit our nodes
// evenly. To do that, we split Cassandra's token ring (signed 64-bit number space) evenly and
// return the token for each node in the node list.
QLValuePB GetTokensValue(size_t index, size_t node_count) {
  CHECK_GT(node_count, 0);
  QLValuePB value_pb;
  QLValuePB *token = value_pb.mutable_set_value()->add_elems();
  token->set_string_value(YBPartition::CqlTokenSplit(node_count, index));
  return value_pb;
}

bool RemoteEndpointMatchesList(const google::protobuf::RepeatedPtrField<HostPortPB>& host_ports,
                               const InetAddress& remote_endpoint) {
  for (const HostPortPB& rpc_address : host_ports) {
    // host portion of rpc_address might be a hostname and hence we need to resolve it.
    boost::container::small_vector<IpAddress, 5> resolved_addresses;
    if (!HostToAddresses(rpc_address.host(), &resolved_addresses).ok()) {
      LOG (WARNING) << "Could not resolve host: " << rpc_address.host();
      continue;
    }
    if (std::find(
            resolved_addresses.begin(), resolved_addresses.end(), remote_endpoint.address()) !=
        resolved_addresses.end()) {
      return true;
    }
  }

  return false;
}

bool RemoteEndpointMatchesTServer(const TSInformationPB& ts_info,
                                  const InetAddress& remote_endpoint) {
  const auto& common = ts_info.registration().common();
  if (RemoteEndpointMatchesList(common.private_rpc_addresses(), remote_endpoint)) {
    return true;
  }
  if (RemoteEndpointMatchesList(common.broadcast_addresses(), remote_endpoint)) {
    return true;
  }
  return false;
}

QLValuePB GetReplicationValue(int replication_factor) {
  QLValuePB value_pb;
  QLMapValuePB *map_value = value_pb.mutable_map_value();

  // replication strategy
  QLValuePB *elem = map_value->add_keys();
  elem->set_string_value("class");
  elem = map_value->add_values();
  elem->set_string_value("org.apache.cassandra.locator.SimpleStrategy");

  // replication factor
  elem = map_value->add_keys();
  elem->set_string_value("replication_factor");
  elem = map_value->add_values();
  elem->set_string_value(std::to_string(replication_factor));

  return value_pb;
}

PublicPrivateIPFutures GetPublicPrivateIPFutures(
    const TSInformationPB& ts_info, DnsResolver* resolver) {
  const auto& common = ts_info.registration().common();
  PublicPrivateIPFutures result;

  const auto& private_host = common.private_rpc_addresses()[0].host();
  if (private_host.empty()) {
    std::promise<Result<IpAddress>> promise;
    result.private_ip_future = promise.get_future();
    promise.set_value(STATUS_FORMAT(
        IllegalState, "Tablet server $0 doesn't have any rpc addresses registered",
        ts_info.tserver_instance().permanent_uuid()));
    return result;
  }

  result.private_ip_future = resolver->ResolveFuture(private_host);

  if (!common.broadcast_addresses().empty()) {
    result.public_ip_future = resolver->ResolveFuture(common.broadcast_addresses()[0].host());
  } else {
    result.public_ip_future = result.private_ip_future;
  }

  return result;
}

const QLValuePB& GetValueHelper<QLValuePB>::Apply(
    const QLValuePB& value_pb, const DataType data_type) {
  return value_pb;
}

QLValuePB GetValueHelper<std::string>::Apply(const std::string& strval, const DataType data_type) {
  QLValuePB value_pb;
  switch (data_type) {
    case STRING:
      value_pb.set_string_value(strval);
      break;
    case BINARY:
      value_pb.set_binary_value(strval);
      break;
    default:
      LOG(ERROR) << "unexpected string type " << data_type;
      break;
  }
  return value_pb;
}

QLValuePB GetValueHelper<std::string>::Apply(
    const char* strval, size_t len, const DataType data_type) {
  QLValuePB value_pb;
  switch (data_type) {
    case STRING:
      value_pb.set_string_value(strval, len);
      break;
    case BINARY:
      value_pb.set_binary_value(strval, len);
      break;
    default:
      LOG(ERROR) << "unexpected string type " << data_type;
      break;
  }
  return value_pb;
}

QLValuePB GetValueHelper<int32_t>::Apply(const int32_t intval, const DataType data_type) {
  QLValuePB value_pb;
  switch (data_type) {
    case INT64:
      value_pb.set_int64_value(intval);
      break;
    case INT32:
      value_pb.set_int32_value(intval);
      break;
    case INT16:
      value_pb.set_int16_value(intval);
      break;
    case INT8:
      value_pb.set_int8_value(intval);
      break;
    default:
      LOG(ERROR) << "unexpected int type " << data_type;
      break;
  }
  return value_pb;
}

QLValuePB GetValueHelper<InetAddress>::Apply(
    const InetAddress& inet_val, const DataType data_type) {
  QLValuePB result;
  QLValue::set_inetaddress_value(inet_val, &result);
  return result;
}

QLValuePB GetValueHelper<Uuid>::Apply(const Uuid& uuid_val, const DataType data_type) {
  QLValuePB result;
  QLValue::set_uuid_value(uuid_val, &result);
  return result;
}

QLValuePB GetValueHelper<bool>::Apply(const bool bool_val, const DataType data_type) {
  QLValuePB value_pb;
  value_pb.set_bool_value(bool_val);
  return value_pb;
}

}  // namespace util
}  // namespace master
}  // namespace yb
