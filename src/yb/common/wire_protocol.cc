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

#include "yb/common/wire_protocol.h"

#include <string>
#include <vector>

#include "yb/common/entity_ids.h"
#include "yb/common/row.h"
#include "yb/gutil/port.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/fastmem.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/errno.h"
#include "yb/util/faststring.h"
#include "yb/util/logging.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/safe_math.h"
#include "yb/util/slice.h"
#include "yb/util/enums.h"

#include "yb/yql/cql/ql/util/errcodes.h"

using google::protobuf::RepeatedPtrField;
using std::vector;

DEFINE_string(use_private_ip, "never",
              "When to use private IP for connection. "
              "cloud - would use private IP if destination node is located in the same cloud. "
              "region - would use private IP if destination node is located in the same cloud and "
                  "region. "
              "zone - would use private IP if destination node is located in the same cloud, "
                  "region and zone."
              "never - would never use private IP if broadcast address is specified.");
namespace yb {

namespace {

template <class Index, class Value>
void SetAt(
    Index index, const Value& value, const Value& default_value, std::vector<Value>* vector) {
  size_t int_index = static_cast<size_t>(index);
  size_t new_size = vector->size();
  while (new_size <= int_index) {
    new_size = std::max<size_t>(1, new_size * 2);
  }
  vector->resize(new_size, default_value);
  (*vector)[int_index] = value;
}

std::vector<AppStatusPB::ErrorCode> CreateStatusToErrorCode() {
  std::vector<AppStatusPB::ErrorCode> result;
  const auto default_value = AppStatusPB::UNKNOWN_ERROR;
#define YB_SET_STATUS_TO_ERROR_CODE(name, pb_name, value, message) \
    SetAt(Status::BOOST_PP_CAT(k, name), AppStatusPB::pb_name, default_value, &result); \
    static_assert( \
        to_underlying(AppStatusPB::pb_name) == to_underlying(Status::BOOST_PP_CAT(k, name)), \
        "The numeric value of AppStatusPB::" BOOST_PP_STRINGIZE(pb_name) " defined in" \
            " wire_protocol.proto does not match the value of Status::k" BOOST_PP_STRINGIZE(name) \
            " defined in status.h.");
  BOOST_PP_SEQ_FOR_EACH(YB_STATUS_FORWARD_MACRO, YB_SET_STATUS_TO_ERROR_CODE, YB_STATUS_CODES);
#undef YB_SET_STATUS_TO_ERROR_CODe
  return result;
}

const std::vector<AppStatusPB::ErrorCode> kStatusToErrorCode = CreateStatusToErrorCode();

std::vector<Status::Code> CreateErrorCodeToStatus() {
  size_t max_index = 0;
  for (const auto error_code : kStatusToErrorCode) {
    if (error_code == AppStatusPB::UNKNOWN_ERROR) {
      continue;
    }
    max_index = std::max(max_index, static_cast<size_t>(error_code));
  }

  std::vector<Status::Code> result(max_index + 1);
  for (size_t int_code = 0; int_code != kStatusToErrorCode.size(); ++int_code) {
    if (kStatusToErrorCode[int_code] == AppStatusPB::UNKNOWN_ERROR) {
      continue;
    }
    result[static_cast<size_t>(kStatusToErrorCode[int_code])] = static_cast<Status::Code>(int_code);
  }

  return result;
}

const std::vector<Status::Code> kErrorCodeToStatus = CreateErrorCodeToStatus();

} // namespace

void StatusToPB(const Status& status, AppStatusPB* pb) {
  pb->Clear();

  if (status.ok()) {
    pb->set_code(AppStatusPB::OK);
    // OK statuses don't have any message or posix code.
    return;
  }

  auto code = static_cast<size_t>(status.code()) < kStatusToErrorCode.size()
      ? kStatusToErrorCode[status.code()] : AppStatusPB::UNKNOWN_ERROR;
  pb->set_code(code);
  if (code == AppStatusPB::UNKNOWN_ERROR) {
    LOG(WARNING) << "Unknown error code translation connect_from internal error "
                 << status << ": sending UNKNOWN_ERROR";
    // For unknown status codes, include the original stringified error
    // code.
    pb->set_message(status.CodeAsString() + ": " + status.message().ToBuffer());
  } else {
    // Otherwise, just encode the message itself, since the other end
    // will reconstruct the other parts of the ToString() response.
    pb->set_message(status.message().cdata(), status.message().size());
  }

  auto error_codes = status.ErrorCodesSlice();
  pb->set_errors(error_codes.data(), error_codes.size());
  // We always has 0 as terminating byte for error codes, so non empty error codes would have
  // more than one bytes.
  if (error_codes.size() > 1) {
    // Set old protobuf fields for backward compatibility.
    Errno err(status);
    if (err != 0) {
      pb->set_posix_code(err.value());
    }
    const auto* ql_error_data = status.ErrorData(ql::QLError::kCategory);
    if (ql_error_data) {
      pb->set_ql_error_code(static_cast<int64_t>(ql::QLErrorTag::Decode(ql_error_data)));
    }
  }

  pb->set_source_file(status.file_name());
  pb->set_source_line(status.line_number());
}

struct WireProtocolTabletServerErrorTag {
  static constexpr uint8_t kCategory = 5;

  enum Value {};

  static size_t EncodedSize(Value value) {
    return sizeof(Value);
  }

  static uint8_t* Encode(Value value, uint8_t* out) {
    Store<Value, LittleEndian>(out, value);
    return out + sizeof(Value);
  }
};

// Backward compatibility.
Status StatusFromOldPB(const AppStatusPB& pb) {
  auto code = kErrorCodeToStatus[pb.code()];

  auto status_factory = [code, &pb](const Slice& errors) {
    return Status(
        code, pb.source_file().c_str(), pb.source_line(), pb.message(), errors, DupFileName::kTrue);
  };

  #define ENCODE_ERROR_AND_RETURN_STATUS(Tag, value) \
    auto error_code = static_cast<Tag::Value>((value)); \
    auto size = 2 + Tag::EncodedSize(error_code); \
    uint8_t* buffer = static_cast<uint8_t*>(alloca(size)); \
    buffer[0] = Tag::kCategory; \
    Tag::Encode(error_code, buffer + 1); \
    buffer[size - 1] = 0; \
    return status_factory(Slice(buffer, size)); \
    /**/

  if (code == Status::kQLError) {
    if (!pb.has_ql_error_code()) {
      return STATUS(InternalError, "Query error code missing");
    }

    ENCODE_ERROR_AND_RETURN_STATUS(ql::QLErrorTag, pb.ql_error_code())
  } else if (pb.has_posix_code()) {
    if (code == Status::kIllegalState || code == Status::kLeaderNotReadyToServe ||
        code == Status::kLeaderHasNoLease) {

      ENCODE_ERROR_AND_RETURN_STATUS(WireProtocolTabletServerErrorTag, pb.posix_code())
    } else {
      ENCODE_ERROR_AND_RETURN_STATUS(ErrnoTag, pb.posix_code())
    }
  }

  return Status(code, pb.source_file().c_str(), pb.source_line(), pb.message(), "",
                nullptr /* error */, DupFileName::kTrue);
  #undef ENCODE_ERROR_AND_RETURN_STATUS
}

Status StatusFromPB(const AppStatusPB& pb) {
  if (pb.code() == AppStatusPB::OK) {
    return Status::OK();
  } else if (pb.code() == AppStatusPB::UNKNOWN_ERROR ||
             static_cast<size_t>(pb.code()) >= kErrorCodeToStatus.size()) {
    LOG(WARNING) << "Unknown error code in status: " << pb.ShortDebugString();
    return STATUS_FORMAT(
        RuntimeError, "($0 unknown): $1", pb.code(), pb.message());
  }

  if (pb.has_errors()) {
    return Status(kErrorCodeToStatus[pb.code()], pb.source_file().c_str(), pb.source_line(),
                  pb.message(), pb.errors(), DupFileName::kTrue);
  }

  return StatusFromOldPB(pb);
}

void HostPortToPB(const HostPort& host_port, HostPortPB* host_port_pb) {
  host_port_pb->set_host(host_port.host());
  host_port_pb->set_port(host_port.port());
}

HostPort HostPortFromPB(const HostPortPB& host_port_pb) {
  HostPort host_port;
  host_port.set_host(host_port_pb.host());
  host_port.set_port(host_port_pb.port());
  return host_port;
}

bool HasHostPortPB(
    const google::protobuf::RepeatedPtrField<HostPortPB>& list, const HostPortPB& hp) {
  for (const auto& i : list) {
    if (i.host() == hp.host() && i.port() == hp.port()) {
      return true;
    }
  }
  return false;
}

Status EndpointFromHostPortPB(const HostPortPB& host_portpb, Endpoint* endpoint) {
  HostPort host_port = HostPortFromPB(host_portpb);
  return EndpointFromHostPort(host_port, endpoint);
}

void HostPortsToPBs(const std::vector<HostPort>& addrs, RepeatedPtrField<HostPortPB>* pbs) {
  for (const auto& addr : addrs) {
    HostPortToPB(addr, pbs->Add());
  }
}

void HostPortsFromPBs(const RepeatedPtrField<HostPortPB>& pbs, std::vector<HostPort>* addrs) {
  addrs->reserve(pbs.size());
  for (const auto& pb : pbs) {
    addrs->push_back(HostPortFromPB(pb));
  }
}

Status AddHostPortPBs(const std::vector<Endpoint>& addrs,
                      RepeatedPtrField<HostPortPB>* pbs) {
  for (const auto& addr : addrs) {
    HostPortPB* pb = pbs->Add();
    pb->set_port(addr.port());
    if (addr.address().is_unspecified()) {
      VLOG(4) << " Asked to add unspecified address: " << addr.address();
      auto status = GetFQDN(pb->mutable_host());
      if (!status.ok()) {
        std::vector<IpAddress> locals;
        if (!GetLocalAddresses(FLAGS_net_address_filter, &locals).ok() ||
            locals.empty()) {
          return status;
        }
        for (auto& address : locals) {
          if (pb == nullptr) {
            pb = pbs->Add();
            pb->set_port(addr.port());
          }
          pb->set_host(address.to_string());
          VLOG(4) << "Adding local address: " << pb->host();
          pb = nullptr;
        }
      } else {
        VLOG(4) << "Adding FQDN " << pb->host();
      }
    } else {
      pb->set_host(addr.address().to_string());
      VLOG(4) << "Adding specific address: " << pb->host();
    }
  }
  return Status::OK();
}

void SchemaToColocatedTableIdentifierPB(
    const Schema& schema, ColocatedTableIdentifierPB* colocated_pb) {
  if (schema.has_pgtable_id()) {
    colocated_pb->set_pgtable_id(schema.pgtable_id());
  } else if (schema.has_cotable_id()) {
    colocated_pb->set_cotable_id(schema.cotable_id().ToString());
  }

}

void SchemaToPB(const Schema& schema, SchemaPB *pb, int flags) {
  pb->Clear();
  SchemaToColocatedTableIdentifierPB(schema, pb->mutable_colocated_table_id());
  SchemaToColumnPBs(schema, pb->mutable_columns(), flags);
  schema.table_properties().ToTablePropertiesPB(pb->mutable_table_properties());
}

void SchemaToPBWithoutIds(const Schema& schema, SchemaPB *pb) {
  pb->Clear();
  SchemaToColumnPBs(schema, pb->mutable_columns(), SCHEMA_PB_WITHOUT_IDS);
}

Status SchemaFromPB(const SchemaPB& pb, Schema *schema) {
  // Conver the columns.
  vector<ColumnSchema> columns;
  vector<ColumnId> column_ids;
  int num_key_columns = 0;
  RETURN_NOT_OK(ColumnPBsToColumnTuple(pb.columns(), &columns, &column_ids, &num_key_columns));

  // Convert the table properties.
  TableProperties table_properties = TableProperties::FromTablePropertiesPB(pb.table_properties());
  RETURN_NOT_OK(schema->Reset(columns, column_ids, num_key_columns, table_properties));

  if (pb.has_colocated_table_id()) {
    switch (pb.colocated_table_id().value_case()) {
      case ColocatedTableIdentifierPB::kCotableId: {
        Uuid cotable_id;
        RETURN_NOT_OK(cotable_id.FromString(pb.colocated_table_id().cotable_id()));
        schema->set_cotable_id(cotable_id);
        break;
      }
      case ColocatedTableIdentifierPB::kPgtableId:
        schema->set_pgtable_id(pb.colocated_table_id().pgtable_id());
        break;
      case ColocatedTableIdentifierPB::VALUE_NOT_SET:
        break;
    }
  }
  return Status::OK();
}

void ColumnSchemaToPB(const ColumnSchema& col_schema, ColumnSchemaPB *pb, int flags) {
  pb->Clear();
  pb->set_name(col_schema.name());
  col_schema.type()->ToQLTypePB(pb->mutable_type());
  pb->set_is_nullable(col_schema.is_nullable());
  pb->set_is_static(col_schema.is_static());
  pb->set_is_counter(col_schema.is_counter());
  pb->set_order(col_schema.order());
  pb->set_sorting_type(col_schema.sorting_type());
  // We only need to process the *hash* primary key here. The regular primary key is set by the
  // conversion for SchemaPB. The reason is that ColumnSchema and ColumnSchemaPB are not matching
  // 1 to 1 as ColumnSchema doesn't have "is_key" field. That was Kudu's code, and we keep it that
  // way for now.
  if (col_schema.is_hash_key()) {
    pb->set_is_key(true);
    pb->set_is_hash_key(true);
  }
}


ColumnSchema ColumnSchemaFromPB(const ColumnSchemaPB& pb) {
  // Only "is_hash_key" is used to construct ColumnSchema. The field "is_key" will be read when
  // processing SchemaPB.
  return ColumnSchema(pb.name(), QLType::FromQLTypePB(pb.type()), pb.is_nullable(),
                      pb.is_hash_key(), pb.is_static(), pb.is_counter(), pb.order(),
                      ColumnSchema::SortingType(pb.sorting_type()));
}

CHECKED_STATUS ColumnPBsToColumnTuple(
    const RepeatedPtrField<ColumnSchemaPB>& column_pbs,
    vector<ColumnSchema>* columns , vector<ColumnId>* column_ids, int* num_key_columns) {
  columns->reserve(column_pbs.size());
  bool is_handling_key = true;
  for (const ColumnSchemaPB& pb : column_pbs) {
    columns->push_back(ColumnSchemaFromPB(pb));
    if (pb.is_key()) {
      if (!is_handling_key) {
        return STATUS(InvalidArgument,
                      "Got out-of-order key column", pb.ShortDebugString());
      }
      (*num_key_columns)++;
    } else {
      is_handling_key = false;
    }
    if (pb.has_id()) {
      column_ids->push_back(ColumnId(pb.id()));
    }
  }

  DCHECK_LE((*num_key_columns), columns->size());
  return Status::OK();
}

Status ColumnPBsToSchema(const RepeatedPtrField<ColumnSchemaPB>& column_pbs,
                         Schema* schema) {

  vector<ColumnSchema> columns;
  vector<ColumnId> column_ids;
  int num_key_columns = 0;
  RETURN_NOT_OK(ColumnPBsToColumnTuple(column_pbs, &columns, &column_ids, &num_key_columns));

  // TODO(perf): could make the following faster by adding a
  // Reset() variant which actually takes ownership of the column
  // vector.
  return schema->Reset(columns, column_ids, num_key_columns);
}

void SchemaToColumnPBs(const Schema& schema,
                       RepeatedPtrField<ColumnSchemaPB>* cols,
                       int flags) {
  cols->Clear();
  int idx = 0;
  for (const ColumnSchema& col : schema.columns()) {
    ColumnSchemaPB* col_pb = cols->Add();
    ColumnSchemaToPB(col, col_pb);
    col_pb->set_is_key(idx < schema.num_key_columns());

    if (schema.has_column_ids() && !(flags & SCHEMA_PB_WITHOUT_IDS)) {
      col_pb->set_id(schema.column_id(idx));
    }

    idx++;
  }
}

Result<UsePrivateIpMode> GetPrivateIpMode() {
  for (auto i : kUsePrivateIpModeList) {
    if (FLAGS_use_private_ip == ToCString(i)) {
      return i;
    }
  }
  return STATUS_FORMAT(
      IllegalState,
      "Invalid value of FLAGS_use_private_ip: $0, using private ip everywhere",
      FLAGS_use_private_ip);
}

UsePrivateIpMode GetMode() {
  auto result = GetPrivateIpMode();
  if (result.ok()) {
    return *result;
  }
  YB_LOG_EVERY_N_SECS(WARNING, 300) << result.status();
  return UsePrivateIpMode::never;
}

bool UsePublicIp(const CloudInfoPB& connect_to,
                 const CloudInfoPB& connect_from) {
  auto mode = GetMode();

  if (mode == UsePrivateIpMode::never) {
    return true;
  }
  if (connect_to.placement_cloud() != connect_from.placement_cloud()) {
    return true;
  }
  if (mode == UsePrivateIpMode::cloud) {
    return false;
  }
  if (connect_to.placement_region() != connect_from.placement_region()) {
    return true;
  }
  if (mode == UsePrivateIpMode::region) {
    return false;
  }
  if (connect_to.placement_zone() != connect_from.placement_zone()) {
    return true;
  }
  return mode != UsePrivateIpMode::zone;
}

const HostPortPB& DesiredHostPort(
    const google::protobuf::RepeatedPtrField<HostPortPB>& broadcast_addresses,
    const google::protobuf::RepeatedPtrField<HostPortPB>& private_host_ports,
    const CloudInfoPB& connect_to,
    const CloudInfoPB& connect_from) {
  if (!broadcast_addresses.empty() && UsePublicIp(connect_to, connect_from)) {
    return broadcast_addresses[0];
  }
  if (!private_host_ports.empty()) {
    return private_host_ports[0];
  }
  static const HostPortPB empty_host_port;
  return empty_host_port;
}

const HostPortPB& DesiredHostPort(const ServerRegistrationPB& registration,
                                  const CloudInfoPB& connect_from) {
  return DesiredHostPort(
      registration.broadcast_addresses(), registration.private_rpc_addresses(),
      registration.cloud_info(), connect_from);
}

} // namespace yb
