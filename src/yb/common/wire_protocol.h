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
// Helpers for dealing with the protobufs defined in wire_protocol.proto.
#ifndef YB_COMMON_WIRE_PROTOCOL_H
#define YB_COMMON_WIRE_PROTOCOL_H

#include <vector>

#include "yb/client/schema.h"
#include "yb/common/wire_protocol.pb.h"
#include "yb/gutil/endian.h"
#include "yb/util/cast.h"
#include "yb/util/status.h"
#include "yb/util/net/net_fwd.h"
#include "yb/util/strongly_typed_uuid.h"

namespace yb {

class ConstContiguousRow;
struct ColumnId;
class ColumnSchema;
class faststring;
class HostPort;
class RowChangeList;
class Schema;
class Slice;

// Convert the given C++ Status object into the equivalent Protobuf.
void StatusToPB(const Status& status, AppStatusPB* pb);

// Convert the given protobuf into the equivalent C++ Status object.
Status StatusFromPB(const AppStatusPB& pb);

// Convert the specified HostPort to protobuf.
void HostPortToPB(const HostPort& host_port, HostPortPB* host_port_pb);

// Returns the HostPort created from the specified protobuf.
HostPort HostPortFromPB(const HostPortPB& host_port_pb);

bool HasHostPortPB(
    const google::protobuf::RepeatedPtrField<HostPortPB>& list, const HostPortPB& hp);

// Returns an Endpoint from HostPortPB.
CHECKED_STATUS EndpointFromHostPortPB(const HostPortPB& host_portpb, Endpoint* endpoint);

// Adds addresses in 'addrs' to 'pbs'. If an address is a wildcard (e.g., "0.0.0.0"),
// then the local machine's FQDN or its network interface address is used in its place.
CHECKED_STATUS AddHostPortPBs(const std::vector<Endpoint>& addrs,
                              google::protobuf::RepeatedPtrField<HostPortPB>* pbs);

// Simply convert the list of host ports into a repeated list of corresponding PB's.
void HostPortsToPBs(const std::vector<HostPort>& addrs,
                    google::protobuf::RepeatedPtrField<HostPortPB>* pbs);

// Convert list of HostPortPBs into host ports.
void HostPortsFromPBs(const google::protobuf::RepeatedPtrField<HostPortPB>& pbs,
                      std::vector<HostPort>* addrs);

enum SchemaPBConversionFlags {
  SCHEMA_PB_WITHOUT_IDS = 1 << 0,
};

// Convert the specified schema to protobuf.
// 'flags' is a bitfield of SchemaPBConversionFlags values.
void SchemaToPB(const Schema& schema, SchemaPB* pb, int flags = 0);

// Convert the specified schema to protobuf without column IDs.
void SchemaToPBWithoutIds(const Schema& schema, SchemaPB *pb);

// Returns the Schema created from the specified protobuf.
// If the schema is invalid, return a non-OK status.
Status SchemaFromPB(const SchemaPB& pb, Schema *schema);

// Convert the specified column schema to protobuf.
// 'flags' is a bitfield of SchemaPBConversionFlags values.
void ColumnSchemaToPB(const ColumnSchema& schema, ColumnSchemaPB *pb, int flags = 0);

// Return the ColumnSchema created from the specified protobuf.
ColumnSchema ColumnSchemaFromPB(const ColumnSchemaPB& pb);

// Convert the given list of ColumnSchemaPB objects into a Schema object.
//
// Returns InvalidArgument if the provided columns don't make a valid Schema
// (eg if the keys are non-contiguous or nullable).
Status ColumnPBsToSchema(
  const google::protobuf::RepeatedPtrField<ColumnSchemaPB>& column_pbs,
  Schema* schema);

// Returns the required information from column pbs to build the column part of SchemaPB.
CHECKED_STATUS ColumnPBsToColumnTuple(
    const google::protobuf::RepeatedPtrField<ColumnSchemaPB>& column_pbs,
    std::vector<ColumnSchema>* columns , std::vector<ColumnId>* column_ids, int* num_key_columns);

// Extract the columns of the given Schema into protobuf objects.
//
// The 'cols' list is replaced by this method.
// 'flags' is a bitfield of SchemaPBConversionFlags values.
void SchemaToColumnPBs(
  const Schema& schema,
  google::protobuf::RepeatedPtrField<ColumnSchemaPB>* cols,
  int flags = 0);

// Extract the colocated table information of the given schema into a protobuf object.
void SchemaToColocatedTableIdentifierPB(
    const Schema& schema, ColocatedTableIdentifierPB* colocated_pb);

YB_DEFINE_ENUM(UsePrivateIpMode, (cloud)(region)(zone)(never));

// Returns mode for selecting between private and public IP.
Result<UsePrivateIpMode> GetPrivateIpMode();

// Pick host and port that should be used to connect node
// broadcast_addresses - node public host ports
// private_host_ports - node private host ports
// connect_to - node placement information
// connect_from - placement information of connect originator
const HostPortPB& DesiredHostPort(
    const google::protobuf::RepeatedPtrField<HostPortPB>& broadcast_addresses,
    const google::protobuf::RepeatedPtrField<HostPortPB>& private_host_ports,
    const CloudInfoPB& connect_to,
    const CloudInfoPB& connect_from);

// Pick host and port that should be used to connect node
// registration - node registration information
// connect_from - placement information of connect originator
const HostPortPB& DesiredHostPort(
    const ServerRegistrationPB& registration, const CloudInfoPB& connect_from);

//----------------------------------- CQL value encode functions ---------------------------------
static inline void CQLEncodeLength(const int32_t length, faststring* buffer) {
  uint32_t byte_value;
  NetworkByteOrder::Store32(&byte_value, static_cast<uint32_t>(length));
  buffer->append(&byte_value, sizeof(byte_value));
}

// Encode a 32-bit length into the buffer without extending the buffer. Caller should ensure the
// buffer size is at least 4 bytes.
static inline void CQLEncodeLength(const int32_t length, void* buffer) {
  NetworkByteOrder::Store32(buffer, static_cast<uint32_t>(length));
}

// Encode a CQL number (8, 16, 32 and 64-bit integer). <num_type> is the integer type.
// <converter> converts the number from machine byte-order to network order and <data_type>
// is the coverter's return type. The converter's input type <data_type> is unsigned while
// <num_type> may be signed or unsigned.
template<typename num_type, typename data_type>
static inline void CQLEncodeNum(
    void (*converter)(void *, data_type), const num_type val, faststring* buffer) {
  static_assert(sizeof(data_type) == sizeof(num_type), "inconsistent num type size");
  CQLEncodeLength(sizeof(num_type), buffer);
  data_type byte_value;
  (*converter)(&byte_value, static_cast<data_type>(val));
  buffer->append(&byte_value, sizeof(byte_value));
}

// Encode a CQL floating point number (float or double). <float_type> is the floating point type.
// <converter> converts the number from machine byte-order to network order and <data_type>
// is the coverter's input type. The converter's input type <data_type> is an integer type.
template<typename float_type, typename data_type>
static inline void CQLEncodeFloat(
    void (*converter)(void *, data_type), const float_type val, faststring* buffer) {
  static_assert(sizeof(float_type) == sizeof(data_type), "inconsistent floating point type size");
  const data_type value = *reinterpret_cast<const data_type*>(&val);
  CQLEncodeNum(converter, value, buffer);
}

static inline void CQLEncodeBytes(const std::string& val, faststring* buffer) {
  CQLEncodeLength(val.size(), buffer);
  buffer->append(val);
}

static inline void Store8(void* p, uint8_t v) {
  *static_cast<uint8_t*>(p) = v;
}

//--------------------------------------------------------------------------------------------------
// For collections the serialized length (size in bytes -- not number of elements) depends on the
// size of their (possibly variable-length) elements so cannot be pre-computed efficiently.
// Therefore CQLStartCollection and CQLFinishCollection should be called before and, respectively,
// after serializing collection elements to set the correct value

// Allocates the space in the buffer for writing the correct length later and returns the buffer
// position after (i.e. where the serialization for the collection value will begin)
static inline int32_t CQLStartCollection(faststring* buffer) {
  CQLEncodeLength(0, buffer);
  return static_cast<int32_t>(buffer->size());
}

// Sets the value for the serialized size of a collection by subtracting the start position to
// compute length and writing it at the right position in the buffer
static inline void CQLFinishCollection(int32_t start_pos, faststring* buffer) {
  // computing collection size (in bytes)
  int32_t coll_size = static_cast<int32_t>(buffer->size()) - start_pos;

  // writing the collection size in bytes to the length component of the CQL value
  int32_t pos = start_pos - sizeof(int32_t); // subtracting size of length component
  NetworkByteOrder::Store32(&(*buffer)[pos], static_cast<uint32_t>(coll_size));
}

//----------------------------------- CQL value decode functions ---------------------------------
#define RETURN_NOT_ENOUGH(data, sz)                         \
  do {                                                      \
    if (data->size() < (sz)) {                              \
      return STATUS(NetworkError, "Truncated CQL message"); \
    }                                                       \
  } while (0)

static inline Result<int32_t> CQLDecodeLength(Slice* data) {
  RETURN_NOT_ENOUGH(data, sizeof(int32_t));
  const auto len = static_cast<int32_t>(NetworkByteOrder::Load32(data->data()));
  data->remove_prefix(sizeof(int32_t));
  return len;
}

// Decode a 32-bit length from the buffer without consuming the buffer. Caller should ensure the
// buffer size is at least 4 bytes.
static inline int32_t CQLDecodeLength(const void* buffer) {
  return static_cast<int32_t>(NetworkByteOrder::Load32(buffer));
}

// Decode a CQL number (8, 16, 32 and 64-bit integer). <num_type> is the parsed integer type.
// <converter> converts the number from network byte-order to machine order and <data_type>
// is the coverter's return type. The converter's return type <data_type> is unsigned while
// <num_type> may be signed or unsigned.
template<typename num_type, typename data_type>
static inline CHECKED_STATUS CQLDecodeNum(
    const size_t len, data_type (*converter)(const void*), Slice* data, num_type* val) {

  static_assert(sizeof(data_type) == sizeof(num_type), "inconsistent num type size");
  if (len != sizeof(num_type)) {
    return STATUS_SUBSTITUTE(NetworkError,
                             "unexpected number byte length: expected $0, provided $1",
                             static_cast<int64_t>(sizeof(num_type)), len);
  }

  RETURN_NOT_ENOUGH(data, sizeof(num_type));
  *val = static_cast<num_type>((*converter)(data->data()));
  data->remove_prefix(sizeof(num_type));
  return Status::OK();
}

// Decode a CQL floating point number (float or double). <float_type> is the parsed floating point
// type. <converter> converts the number from network byte-order to machine order and <data_type>
// is the coverter's return type. The converter's return type <data_type> is an integer type.
template<typename float_type, typename data_type>
static inline CHECKED_STATUS CQLDecodeFloat(
    const size_t len, data_type (*converter)(const void*), Slice* data, float_type* val) {
  // Make sure float and double are exactly sizeof uint32_t and uint64_t.
  static_assert(sizeof(float_type) == sizeof(data_type), "inconsistent floating point type size");
  data_type bval = 0;
  RETURN_NOT_OK(CQLDecodeNum(len, converter, data, &bval));
  *val = *reinterpret_cast<float_type*>(&bval);
  return Status::OK();
}

static inline CHECKED_STATUS CQLDecodeBytes(size_t len, Slice* data, std::string* val) {
  RETURN_NOT_ENOUGH(data, len);
  *val = std::string(util::to_char_ptr(data->data()), len);
  data->remove_prefix(len);
  return Status::OK();
}

static inline uint8_t Load8(const void* p) {
  return *static_cast<const uint8_t*>(p);
}

#undef RETURN_NOT_ENOUGH

YB_STRONGLY_TYPED_UUID(ClientId);
typedef int64_t RetryableRequestId;

// Special value which is used to initialize starting RetryableRequestId for the client and tablet
// based on min running at server side.
constexpr RetryableRequestId kInitializeFromMinRunning = -1;

struct MinRunningRequestIdTag : yb::IntegralErrorTag<int64_t> {
  // It is part of the wire protocol and should not be changed once released.
  static constexpr uint8_t kCategory = 13;

  static std::string ToMessage(Value value) {
    return Format("Min running request ID: $0", value);
  }
};

typedef yb::StatusErrorCodeImpl<MinRunningRequestIdTag> MinRunningRequestIdStatusData;

template <class Resp>
CHECKED_STATUS StatusFromResponse(const Resp& resp) {
  return resp.has_error() ? StatusFromPB(resp.error().status()) : Status::OK();
}

struct SplitChildTabletIdsTag : yb::StringVectorBackedErrorTag {
  // It is part of the wire protocol and should not be changed once released.
  static constexpr uint8_t kCategory = 14;

  static std::string ToMessage(Value value) {
    return Format("Split child tablet IDs: $0", value);
  }
};

typedef yb::StatusErrorCodeImpl<SplitChildTabletIdsTag> SplitChildTabletIdsData;

} // namespace yb

#endif  // YB_COMMON_WIRE_PROTOCOL_H
