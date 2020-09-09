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

#include <lz4.h>
#include <snappy.h>

#include <regex>

#include "yb/client/client.h"

#include "yb/common/ql_protocol.pb.h"
#include "yb/common/ql_value.h"

#include "yb/yql/cql/cqlserver/cql_message.h"
#include "yb/yql/cql/cqlserver/cql_processor.h"

#include "yb/gutil/endian.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/random_util.h"

namespace yb {
namespace cqlserver {

using std::shared_ptr;
using std::unique_ptr;
using std::string;
using std::set;
using std::vector;
using std::unordered_map;
using strings::Substitute;
using util::to_char_ptr;
using util::to_uchar_ptr;
using snappy::GetUncompressedLength;
using snappy::MaxCompressedLength;
using snappy::RawUncompress;
using snappy::RawCompress;

#define RETURN_NOT_ENOUGH(sz)                               \
  do {                                                      \
    if (body_.size() < (sz)) {                              \
      return STATUS(NetworkError, "Truncated CQL message"); \
    }                                                       \
  } while (0)

constexpr char CQLMessage::kCQLVersionOption[];
constexpr char CQLMessage::kCompressionOption[];
constexpr char CQLMessage::kNoCompactOption[];

constexpr char CQLMessage::kLZ4Compression[];
constexpr char CQLMessage::kSnappyCompression[];

constexpr char CQLMessage::kTopologyChangeEvent[];
constexpr char CQLMessage::kStatusChangeEvent[];
constexpr char CQLMessage::kSchemaChangeEvent[];

Status CQLMessage::QueryParameters::GetBindVariable(const std::string& name,
                                                    const int64_t pos,
                                                    const shared_ptr<QLType>& type,
                                                    QLValue* value) const {
  const Value* v = nullptr;
  if (!value_map.empty()) {
    const auto itr = value_map.find(name);
    if (itr == value_map.end()) {
      return STATUS_SUBSTITUTE(RuntimeError, "Bind variable \"$0\" not found", name);
    }
    v = &values.at(itr->second);
  } else {
    if (pos < 0 || pos >= values.size()) {
      // Return error with 1-based position.
      return STATUS_SUBSTITUTE(RuntimeError, "Bind variable at position $0 not found", pos + 1);
    }
    v = &values.at(pos);
  }
  switch (v->kind) {
    case Value::Kind::NOT_NULL: {
      if (v->value.empty()) {
        switch (type->main()) {
          case DataType::STRING:
            value->set_string_value("");
            return Status::OK();
          case DataType::BINARY:
            value->set_binary_value("");
            return Status::OK();
          case DataType::NULL_VALUE_TYPE: FALLTHROUGH_INTENDED;
          case DataType::INT8: FALLTHROUGH_INTENDED;
          case DataType::INT16: FALLTHROUGH_INTENDED;
          case DataType::INT32: FALLTHROUGH_INTENDED;
          case DataType::INT64: FALLTHROUGH_INTENDED;
          case DataType::BOOL: FALLTHROUGH_INTENDED;
          case DataType::FLOAT: FALLTHROUGH_INTENDED;
          case DataType::DOUBLE: FALLTHROUGH_INTENDED;
          case DataType::TIMESTAMP: FALLTHROUGH_INTENDED;
          case DataType::DECIMAL: FALLTHROUGH_INTENDED;
          case DataType::VARINT: FALLTHROUGH_INTENDED;
          case DataType::INET: FALLTHROUGH_INTENDED;
          case DataType::JSONB: FALLTHROUGH_INTENDED;
          case DataType::LIST: FALLTHROUGH_INTENDED;
          case DataType::MAP: FALLTHROUGH_INTENDED;
          case DataType::SET: FALLTHROUGH_INTENDED;
          case DataType::UUID: FALLTHROUGH_INTENDED;
          case DataType::TIMEUUID:
            value->SetNull();
            return Status::OK();
          case DataType::UNKNOWN_DATA: FALLTHROUGH_INTENDED;
          case DataType::TUPLE: FALLTHROUGH_INTENDED;
          case DataType::TYPEARGS: FALLTHROUGH_INTENDED;
          case DataType::USER_DEFINED_TYPE: FALLTHROUGH_INTENDED;
          case DataType::FROZEN: FALLTHROUGH_INTENDED;
          case DataType::DATE: FALLTHROUGH_INTENDED;
          case DataType::TIME: FALLTHROUGH_INTENDED;
          case DataType::UINT8: FALLTHROUGH_INTENDED;
          case DataType::UINT16: FALLTHROUGH_INTENDED;
          case DataType::UINT32: FALLTHROUGH_INTENDED;
          case DataType::UINT64:
            break;
        }
        return STATUS_SUBSTITUTE(
            NotSupported, "Unsupported datatype $0", static_cast<int>(type->main()));
      }
      Slice data(v->value);
      return value->Deserialize(type, YQL_CLIENT_CQL, &data);
    }
    case Value::Kind::IS_NULL:
      value->SetNull();
      return Status::OK();
    case Value::Kind::NOT_SET:
      // The RuntimeError status code will be mapped later into the non-retryable INVALID_REQUEST
      // error code (non-retryable due to not mapping into STALE_METADATA code later).
      return STATUS(RuntimeError, "Bind variable was not set");
  }
  return STATUS_SUBSTITUTE(
      RuntimeError, "Invalid bind variable kind $0", static_cast<int>(v->kind));
}

Status CQLMessage::QueryParameters::ValidateConsistency() {
  switch(consistency) {
    case Consistency::LOCAL_ONE: FALLTHROUGH_INTENDED;
    case Consistency::QUORUM: {
      // We are repurposing cassandra's "QUORUM" consistency level to indicate "STRONG"
      // consistency for YB. Although, by default the datastax client uses "LOCAL_ONE" as its
      // consistency level and since by default we want to support STRONG consistency, we use
      // STRONG consistency even for LOCAL_ONE. This way existing cassandra clients don't need
      // any changes.
      set_yb_consistency_level(YBConsistencyLevel::STRONG);
      break;
    }
    case Consistency::ONE: {
      // Here we repurpose cassandra's ONE consistency level to be CONSISTENT_PREFIX for us since
      // that seems to be the most appropriate.
      set_yb_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
      break;
    }
    default:
      YB_LOG_EVERY_N_SECS(WARNING, 10) << "Consistency level " << static_cast<uint16_t>(consistency)
                                       << " is not supported, defaulting to strong consistency";
      set_yb_consistency_level(YBConsistencyLevel::STRONG);
  }
  return Status::OK();
}

namespace {

template<class Type>
Type LoadByte(const Slice& slice, size_t offset) {
  return static_cast<Type>(Load8(slice.data() + offset));
}

template<class Type>
Type LoadShort(const Slice& slice, size_t offset) {
  return static_cast<Type>(NetworkByteOrder::Load16(slice.data() + offset));
}

template<class Type>
Type LoadInt(const Slice& slice, size_t offset) {
  return static_cast<Type>(NetworkByteOrder::Load32(slice.data() + offset));
}

} // namespace

// ------------------------------------ CQL request -----------------------------------
bool CQLRequest::ParseRequest(
  const Slice& mesg, const CompressionScheme compression_scheme,
  unique_ptr<CQLRequest>* request, unique_ptr<CQLResponse>* error_response) {

  *request = nullptr;
  *error_response = nullptr;

  if (mesg.size() < kMessageHeaderLength) {
    error_response->reset(
        new ErrorResponse(
            static_cast<StreamId>(0), ErrorResponse::Code::PROTOCOL_ERROR, "Incomplete header"));
    return false;
  }

  Header header(
      LoadByte<Version>(mesg, kHeaderPosVersion),
      LoadByte<Flags>(mesg, kHeaderPosFlags),
      ParseStreamId(mesg),
      LoadByte<Opcode>(mesg, kHeaderPosOpcode));

  uint32_t length = LoadInt<uint32_t>(mesg, kHeaderPosLength);
  DVLOG(4) << "CQL message "
           << "version 0x" << std::hex << static_cast<uint32_t>(header.version)   << " "
           << "flags 0x"   << std::hex << static_cast<uint32_t>(header.flags)     << " "
           << "stream id " << std::dec << static_cast<uint32_t>(header.stream_id) << " "
           << "opcode 0x"  << std::hex << static_cast<uint32_t>(header.opcode)    << " "
           << "length "    << std::dec << static_cast<uint32_t>(length);

  // Verify proper version that the response bit is not set in the request protocol version and
  // the protocol version is one we support.
  if (header.version & kResponseVersion) {
    error_response->reset(
        new ErrorResponse(
            header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR, "Not a request"));
    return false;
  }
  if (header.version < kMinimumVersion || header.version > kCurrentVersion) {
    error_response->reset(
        new ErrorResponse(
            header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR,
            Substitute("Invalid or unsupported protocol version $0. Supported versions are between "
                       "$1 and $2.", header.version, kMinimumVersion, kCurrentVersion)));
    return false;
  }

  size_t body_size = mesg.size() - kMessageHeaderLength;
  const uint8_t* body_data = body_size > 0 ? mesg.data() + kMessageHeaderLength : to_uchar_ptr("");
  unique_ptr<uint8_t[]> buffer;

  // If the message body is compressed, uncompress it.
  if (body_size > 0 && (header.flags & kCompressionFlag)) {
    if (header.opcode == Opcode::STARTUP) {
      error_response->reset(
          new ErrorResponse(
              header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR,
              "STARTUP request should not be compressed"));
      return false;
    }
    switch (compression_scheme) {
      case CompressionScheme::kLz4: {
        if (body_size < sizeof(uint32_t)) {
          error_response->reset(
              new ErrorResponse(
                  header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR,
                  "Insufficient compressed data"));
          return false;
        }

        const uint32_t uncomp_size = static_cast<uint32_t>(NetworkByteOrder::Load32(body_data));
        buffer = std::make_unique<uint8_t[]>(uncomp_size);
        body_data += sizeof(uncomp_size);
        body_size -= sizeof(uncomp_size);
        const int size = LZ4_decompress_safe(to_char_ptr(body_data), to_char_ptr(buffer.get()),
                                             body_size, uncomp_size);
        if (size < 0 || size != uncomp_size) {
          error_response->reset(
              new ErrorResponse(
                  header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR,
                  "Error occurred when uncompressing CQL message"));
          return false;
        }
        body_data = buffer.get();
        body_size = uncomp_size;
        break;
      }
      case CompressionScheme::kSnappy: {
        size_t uncomp_size = 0;
        if (GetUncompressedLength(to_char_ptr(body_data), body_size, &uncomp_size)) {
          buffer = std::make_unique<uint8_t[]>(uncomp_size);
          if (RawUncompress(to_char_ptr(body_data), body_size, to_char_ptr(buffer.get()))) {
            body_data = buffer.get();
            body_size = uncomp_size;
            break;
          }
        }
        error_response->reset(
            new ErrorResponse(
                header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR,
                "Error occurred when uncompressing CQL message"));
        break;
      }
      case CompressionScheme::kNone:
        error_response->reset(
            new ErrorResponse(
                header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR,
                "No compression scheme specified"));
        return false;
    }
  }

  const Slice body = (body_size == 0) ? Slice() : Slice(body_data, body_size);

  // Construct the skeleton request by the opcode
  switch (header.opcode) {
    case Opcode::STARTUP:
      request->reset(new StartupRequest(header, body));
      break;
    case Opcode::AUTH_RESPONSE:
      request->reset(new AuthResponseRequest(header, body));
      break;
    case Opcode::OPTIONS:
      request->reset(new OptionsRequest(header, body));
      break;
    case Opcode::QUERY:
      request->reset(new QueryRequest(header, body));
      break;
    case Opcode::PREPARE:
      request->reset(new PrepareRequest(header, body));
      break;
    case Opcode::EXECUTE:
      request->reset(new ExecuteRequest(header, body));
      break;
    case Opcode::BATCH:
      request->reset(new BatchRequest(header, body));
      break;
    case Opcode::REGISTER:
      request->reset(new RegisterRequest(header, body));
      break;

    // These are not request but response opcodes
    case Opcode::ERROR:
    case Opcode::READY:
    case Opcode::AUTHENTICATE:
    case Opcode::SUPPORTED:
    case Opcode::RESULT:
    case Opcode::EVENT:
    case Opcode::AUTH_CHALLENGE:
    case Opcode::AUTH_SUCCESS:
      error_response->reset(
          new ErrorResponse(
              header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR, "Not a request opcode"));
      return false;

    // default: -> fall through
  }

  if (*request == nullptr) {
    error_response->reset(
        new ErrorResponse(
            header.stream_id, ErrorResponse::Code::PROTOCOL_ERROR, "Unknown opcode"));
    return false;
  }

  // Parse the request body
  const Status status = (*request)->ParseBody();
  if (!status.ok()) {
    error_response->reset(
        new ErrorResponse(
            *(*request), ErrorResponse::Code::PROTOCOL_ERROR, status.message().ToString()));
  } else if (!(*request)->body_.empty()) {
    // Flag error when there are bytes remaining after we have parsed the whole request body
    // according to the protocol. Either the request's length field from the client is
    // wrong. Or we could have a bug in our parser.
    error_response->reset(
        new ErrorResponse(
            *(*request), ErrorResponse::Code::PROTOCOL_ERROR, "Request length too long"));
  }

  // If there is any error, free the partially parsed request and return.
  if (*error_response != nullptr) {
    *request = nullptr;
    return false;
  }

  // Clear and release the body after parsing.
  (*request)->body_.clear();

  return true;
}

CQLRequest::CQLRequest(const Header& header, const Slice& body) : CQLMessage(header), body_(body) {
}

CQLRequest::~CQLRequest() {
}

//----------------------------------------------------------------------------------------

Status CQLRequest::ParseUUID(string* value) {
  RETURN_NOT_ENOUGH(kUUIDSize);
  *value = string(to_char_ptr(body_.data()), kUUIDSize);
  body_.remove_prefix(kUUIDSize);
  DVLOG(4) << "CQL uuid " << *value;
  return Status::OK();
}

Status CQLRequest::ParseTimeUUID(string* value) {
  RETURN_NOT_ENOUGH(kUUIDSize);
  *value = string(to_char_ptr(body_.data()), kUUIDSize);
  body_.remove_prefix(kUUIDSize);
  DVLOG(4) << "CQL timeuuid " << *value;
  return Status::OK();
}

Status CQLRequest::ParseStringList(vector<string>* list) {
  DVLOG(4) << "CQL string list ...";
  uint16_t count = 0;
  RETURN_NOT_OK(ParseShort(&count));
  list->resize(count);
  for (uint16_t i = 0; i < count; ++i) {
    RETURN_NOT_OK(ParseString(&list->at(i)));
  }
  return Status::OK();
}

Status CQLRequest::ParseInet(Endpoint* value) {
  std::string ipaddr;
  int32_t port = 0;
  RETURN_NOT_OK(ParseBytes("CQL ipaddr", &CQLRequest::ParseByte, &ipaddr));
  RETURN_NOT_OK(ParseInt(&port));
  if (port < 0 || port > 65535) {
    return STATUS(NetworkError, "Invalid inet port");
  }
  IpAddress address;
  if (ipaddr.size() == boost::asio::ip::address_v4::bytes_type().size()) {
    boost::asio::ip::address_v4::bytes_type bytes;
    memcpy(bytes.data(), ipaddr.data(), ipaddr.size());
    address = boost::asio::ip::address_v4(bytes);
  } else if (ipaddr.size() == boost::asio::ip::address_v6::bytes_type().size()) {
    boost::asio::ip::address_v6::bytes_type bytes;
    memcpy(bytes.data(), ipaddr.data(), ipaddr.size());
    address = boost::asio::ip::address_v6(bytes);
  } else {
    return STATUS_SUBSTITUTE(NetworkError, "Invalid size of ipaddr: $0", ipaddr.size());
  }
  *value = Endpoint(address, port);
  DVLOG(4) << "CQL inet " << *value;
  return Status::OK();
}

Status CQLRequest::ParseStringMap(unordered_map<string, string>* map) {
  DVLOG(4) << "CQL string map ...";
  uint16_t count = 0;
  RETURN_NOT_OK(ParseShort(&count));
  for (uint16_t i = 0; i < count; ++i) {
    string name, value;
    RETURN_NOT_OK(ParseString(&name));
    RETURN_NOT_OK(ParseString(&value));
    (*map)[name] = value;
  }
  return Status::OK();
}

Status CQLRequest::ParseStringMultiMap(unordered_map<string, vector<string>>* map) {
  DVLOG(4) << "CQL string multimap ...";
  uint16_t count = 0;
  RETURN_NOT_OK(ParseShort(&count));
  for (uint16_t i = 0; i < count; ++i) {
    string name;
    vector<string> value;
    RETURN_NOT_OK(ParseString(&name));
    RETURN_NOT_OK(ParseStringList(&value));
    (*map)[name] = value;
  }
  return Status::OK();
}

Status CQLRequest::ParseBytesMap(unordered_map<string, string>* map) {
  DVLOG(4) << "CQL bytes map ...";
  uint16_t count = 0;
  RETURN_NOT_OK(ParseShort(&count));
  for (uint16_t i = 0; i < count; ++i) {
    string name, value;
    RETURN_NOT_OK(ParseString(&name));
    RETURN_NOT_OK(ParseBytes(&value));
    (*map)[name] = value;
  }
  return Status::OK();
}

Status CQLRequest::ParseValue(const bool with_name, Value* value) {
  DVLOG(4) << "CQL value ...";
  if (with_name) {
    RETURN_NOT_OK(ParseString(&value->name));
  }
  // Save data pointer to assign the value with length bytes below.
  const uint8_t* data = body_.data();
  int32_t length = 0;
  RETURN_NOT_OK(ParseInt(&length));
  if (length >= 0) {
    value->kind = Value::Kind::NOT_NULL;
    if (length > 0) {
      RETURN_NOT_ENOUGH(length);
      value->value.assign(to_char_ptr(data), kIntSize + length);
      body_.remove_prefix(length);
      DVLOG(4) << "CQL value bytes " << value->value;
    }
  } else if (VersionIsCompatible(kV4Version)) {
    switch (length) {
      case -1:
        value->kind = Value::Kind::IS_NULL;
        break;
      case -2:
        value->kind = Value::Kind::NOT_SET;
        break;
      default:
        return STATUS(NetworkError, "Invalid length in value");
        break;
    }
  } else {
    value->kind = Value::Kind::IS_NULL;
  }
  return Status::OK();
}

Status CQLRequest::ParseQueryParameters(QueryParameters* params) {
  DVLOG(4) << "CQL query parameters ...";
  RETURN_NOT_OK(ParseConsistency(&params->consistency));
  RETURN_NOT_OK(params->ValidateConsistency());
  RETURN_NOT_OK(ParseByte(&params->flags));
  params->set_request_id(RandomUniformInt<uint64_t>());
  if (params->flags & CQLMessage::QueryParameters::kWithValuesFlag) {
    const bool with_name = (params->flags & CQLMessage::QueryParameters::kWithNamesForValuesFlag);
    uint16_t count = 0;
    RETURN_NOT_OK(ParseShort(&count));
    params->values.resize(count);
    for (uint16_t i = 0; i < count; ++i) {
      Value& value = params->values[i];
      RETURN_NOT_OK(ParseValue(with_name, &value));
      if (with_name) {
        params->value_map[value.name] = i;
      }
    }
  }
  if (params->flags & CQLMessage::QueryParameters::kWithPageSizeFlag) {
    int32_t page_size = 0;
    RETURN_NOT_OK(ParseInt(&page_size));
    params->set_page_size(page_size);
  }
  if (params->flags & CQLMessage::QueryParameters::kWithPagingStateFlag) {
    string paging_state;
    RETURN_NOT_OK(ParseBytes(&paging_state));
    RETURN_NOT_OK(params->SetPagingState(paging_state));
  }
  if (params->flags & CQLMessage::QueryParameters::kWithSerialConsistencyFlag) {
    RETURN_NOT_OK(ParseConsistency(&params->serial_consistency));
  }
  if (params->flags & CQLMessage::QueryParameters::kWithDefaultTimestampFlag) {
    RETURN_NOT_OK(ParseLong(&params->default_timestamp));
  }
  return Status::OK();
}

// ------------------------------ Individual CQL requests -----------------------------------
StartupRequest::StartupRequest(const Header& header, const Slice& body)
    : CQLRequest(header, body) {
}

StartupRequest::~StartupRequest() {
}

Status StartupRequest::ParseBody() {
  return ParseStringMap(&options_);
}

//----------------------------------------------------------------------------------------
AuthResponseRequest::AuthResponseRequest(const Header& header, const Slice& body)
    : CQLRequest(header, body) {
}

AuthResponseRequest::~AuthResponseRequest() {
}

Status AuthResponseRequest::ParseBody() {
  RETURN_NOT_OK(ParseBytes(&token_));
  string error_msg;
  do {
    if (token_.empty()) {
      error_msg = "Invalid empty token!";
      break;
    }
    if (token_[0] != '\0') {
      error_msg = "Invalid format. Message must begin with \\0";
      break;
    }
    size_t next_delim = token_.find_first_of('\0', 1);
    if (next_delim == std::string::npos) {
      error_msg = "Invalid format. Message must contain \\0 after username";
      break;
    }
    // Start from token_[1], read all the username.
    params_.username = token_.substr(1, next_delim - 1);
    // Start from after the delimiter, go to the end.
    params_.password = token_.substr(next_delim + 1);
    return Status::OK();
  } while (0);
  return STATUS(InvalidArgument, error_msg);
}

CHECKED_STATUS AuthResponseRequest::AuthQueryParameters::GetBindVariable(
    const std::string& name,
    int64_t pos,
    const std::shared_ptr<QLType>& type,
    QLValue* value) const {
  if (pos == 0) {
    value->set_string_value(username);
    return Status::OK();
  } else {
    return STATUS(InvalidArgument, Substitute("Bind variable position $0 out of range: ", pos));
  }
}

//----------------------------------------------------------------------------------------
OptionsRequest::OptionsRequest(const Header& header, const Slice& body) : CQLRequest(header, body) {
}

OptionsRequest::~OptionsRequest() {
}

Status OptionsRequest::ParseBody() {
  // Options body is empty
  return Status::OK();
}

//----------------------------------------------------------------------------------------
QueryRequest::QueryRequest(const Header& header, const Slice& body) : CQLRequest(header, body) {
}

QueryRequest::~QueryRequest() {
}

Status QueryRequest::ParseBody() {
  RETURN_NOT_OK(ParseLongString(&query_));
  RETURN_NOT_OK(ParseQueryParameters(&params_));
  return Status::OK();
}

//----------------------------------------------------------------------------------------
PrepareRequest::PrepareRequest(const Header& header, const Slice& body) : CQLRequest(header, body) {
}

PrepareRequest::~PrepareRequest() {
}

Status PrepareRequest::ParseBody() {
  RETURN_NOT_OK(ParseLongString(&query_));
  return Status::OK();
}

//----------------------------------------------------------------------------------------
ExecuteRequest::ExecuteRequest(const Header& header, const Slice& body) : CQLRequest(header, body) {
}

ExecuteRequest::~ExecuteRequest() {
}

Status ExecuteRequest::ParseBody() {
  RETURN_NOT_OK(ParseShortBytes(&query_id_));
  RETURN_NOT_OK(ParseQueryParameters(&params_));
  return Status::OK();
}

//----------------------------------------------------------------------------------------
BatchRequest::BatchRequest(const Header& header, const Slice& body) : CQLRequest(header, body) {
}

BatchRequest::~BatchRequest() {
}

Status BatchRequest::ParseBody() {
  uint8_t type = 0;
  RETURN_NOT_OK(ParseByte(&type));
  type_ = static_cast<Type>(type);
  uint16_t query_count = 0;
  RETURN_NOT_OK(ParseShort(&query_count));
  queries_.resize(query_count);
  for (uint16_t i = 0; i < query_count; ++i) {
    Query& query = queries_[i];
    uint8_t is_prepared_query = 0;
    RETURN_NOT_OK(ParseByte(&is_prepared_query));
    switch (is_prepared_query) {
      case 0:
        query.is_prepared = false;
        RETURN_NOT_OK(ParseLongString(&query.query));
        break;
      case 1:
        query.is_prepared = true;
        RETURN_NOT_OK(ParseShortBytes(&query.query_id));
        break;
      default:
        return STATUS(NetworkError, "Invalid is_prepared_query byte in batch request");
        break;
    }
    uint16_t value_count = 0;
    RETURN_NOT_OK(ParseShort(&value_count));
    query.params.values.resize(value_count);
    for (uint16_t j = 0; j < value_count; ++j) {
      // with_name is not possible in the protocol due to a design flaw. See JIRA CASSANDRA-10246.
      RETURN_NOT_OK(ParseValue(false /* with_name */, &query.params.values[j]));
    }
  }

  Consistency consistency = Consistency::ANY;
  QueryParameters::Flags flags = 0;
  Consistency serial_consistency = Consistency::ANY;
  int64_t default_timestamp = 0;
  RETURN_NOT_OK(ParseConsistency(&consistency));
  RETURN_NOT_OK(ParseByte(&flags));
  if (flags & CQLMessage::QueryParameters::kWithSerialConsistencyFlag) {
    RETURN_NOT_OK(ParseConsistency(&serial_consistency));
  }
  if (flags & CQLMessage::QueryParameters::kWithDefaultTimestampFlag) {
    RETURN_NOT_OK(ParseLong(&default_timestamp));
  }

  for (Query& query : queries_) {
    QueryParameters& params = query.params;
    params.consistency = consistency;
    params.flags = flags;
    params.serial_consistency = serial_consistency;
    params.default_timestamp = default_timestamp;
  }
  return Status::OK();
}

//----------------------------------------------------------------------------------------
RegisterRequest::RegisterRequest(const Header& header, const Slice& body)
    : CQLRequest(header, body) {
}

RegisterRequest::~RegisterRequest() {
}

Status RegisterRequest::ParseBody() {
  vector<string> event_types;
  RETURN_NOT_OK(ParseStringList(&event_types));
  events_ = kNoEvents;

  for (const string& event_type : event_types) {
    if (event_type == kTopologyChangeEvent) {
      events_ |= kTopologyChange;
    } else if (event_type == kStatusChangeEvent) {
      events_ |= kStatusChange;
    } else if (event_type == kSchemaChangeEvent) {
      events_ |= kSchemaChange;
    } else {
      return STATUS(NetworkError, "Invalid event type in register request");
    }
  }

  return Status::OK();
}

// --------------------------- Serialization utility functions -------------------------------
namespace {

using yb::cqlserver::CQLMessage;

// Serialize a CQL number (8, 16, 32 and 64-bit integer). <num_type> is the integer type.
// <converter> converts the number from machine byte-order to network order and <data_type>
// is the coverter's return type. The converter's return type <data_type> is unsigned while
// <num_type> may be signed or unsigned.
template<typename num_type, typename data_type>
void SerializeNum(void (*converter)(void *, data_type), const num_type val, faststring* mesg) {
  data_type byte_value;
  (*converter)(&byte_value, static_cast<data_type>(val));
  mesg->append(&byte_value, sizeof(byte_value));
}

// Serialize a CQL byte stream (string or bytes). <len_type> is the length type.
// <len_serializer> serializes the byte length from machine byte-order to network order.
template<typename len_type>
inline void SerializeBytes(
    void (*len_serializer)(len_type, faststring* mesg), string val,
    faststring* mesg) {
  (*len_serializer)(static_cast<len_type>(val.size()), mesg);
  mesg->append(val);
}

inline void SerializeByte(const uint8_t value, faststring* mesg) {
  static_assert(sizeof(value) == CQLMessage::kByteSize, "inconsistent byte size");
  SerializeNum(Store8, value, mesg);
}

inline void SerializeShort(const uint16_t value, faststring* mesg) {
  static_assert(sizeof(value) == CQLMessage::kShortSize, "inconsistent short size");
  SerializeNum(NetworkByteOrder::Store16, value, mesg);
}

inline void SerializeInt(const int32_t value, faststring* mesg) {
  static_assert(sizeof(value) == CQLMessage::kIntSize, "inconsistent int size");
  SerializeNum(NetworkByteOrder::Store32, value, mesg);
}

#if 0 // Save this function for future use
inline void SerializeLong(const int64_t value, faststring* mesg) {
  static_assert(sizeof(value) == CQLMessage::kLongSize, "inconsistent long size");
  SerializeNum(NetworkByteOrder::Store64, value, mesg);
}
#endif

inline void SerializeString(const string& value, faststring* mesg) {
  SerializeBytes(&SerializeShort, value, mesg);
}

#if 0 // Save these functions for future use
inline void SerializeLongString(const string& value, faststring* mesg) {
  SerializeBytes(&SerializeInt, value, mesg);
}
#endif

inline void SerializeShortBytes(const string& value, faststring* mesg) {
  SerializeBytes(&SerializeShort, value, mesg);
}

inline void SerializeBytes(const string& value, faststring* mesg) {
  SerializeBytes(&SerializeInt, value, mesg);
}

#if 0 // Save these functions for future use
inline void SerializeConsistency(const CQLMessage::Consistency consistency, faststring* mesg) {
  static_assert(
      sizeof(consistency) == CQLMessage::kConsistencySize,
      "inconsistent consistency size");
  SerializeNum(NetworkByteOrder::Store16, consistency, mesg);
}

void SerializeUUID(const string& value, faststring* mesg) {
  if (value.size() == CQLMessage::kUUIDSize) {
    mesg->append(value);
  } else {
    LOG(ERROR) << "Internal error: inconsistent UUID size: " << value.size();
    uint8_t empty_uuid[CQLMessage::kUUIDSize] = {0};
    mesg->append(empty_uuid, sizeof(empty_uuid));
  }
}

void SerializeTimeUUID(const string& value, faststring* mesg) {
  if (value.size() == CQLMessage::kUUIDSize) {
    mesg->append(value);
  } else {
    LOG(ERROR) << "Internal error: inconsistent TimeUUID size: " << value.size();
    uint8_t empty_uuid[CQLMessage::kUUIDSize] = {0};
    mesg->append(empty_uuid, sizeof(empty_uuid));
  }
}
#endif

void SerializeStringList(const vector<string>& list, faststring* mesg) {
  SerializeShort(list.size(), mesg);
  for (int i = 0; i < list.size(); ++i) {
    SerializeString(list[i], mesg);
  }
}

void SerializeInet(const Endpoint& value, faststring* mesg) {
  auto address = value.address();
  if (address.is_v4()) {
    auto bytes = address.to_v4().to_bytes();
    SerializeByte(bytes.size(), mesg);
    mesg->append(bytes.data(), bytes.size());
  } else {
    auto bytes = address.to_v6().to_bytes();
    SerializeByte(bytes.size(), mesg);
    mesg->append(bytes.data(), bytes.size());
  }
  const uint16_t port = value.port();
  SerializeInt(port, mesg);
}

#if 0 // Save these functions for future use
void SerializeStringMap(const unordered_map<string, string>& map, faststring* mesg) {
  SerializeShort(map.size(), mesg);
  for (const auto& element : map) {
    SerializeString(element.first, mesg);
    SerializeString(element.second, mesg);
  }
}
#endif

void SerializeStringMultiMap(const unordered_map<string, vector<string>>& map, faststring* mesg) {
  SerializeShort(map.size(), mesg);
  for (const auto& element : map) {
    SerializeString(element.first, mesg);
    SerializeStringList(element.second, mesg);
  }
}

#if 0 // Save these functions for future use
void SerializeBytesMap(const unordered_map<string, string>& map, faststring* mesg) {
  SerializeShort(map.size(), mesg);
  for (const auto& element : map) {
    SerializeString(element.first, mesg);
    SerializeBytes(element.second, mesg);
  }
}

void SerializeValue(const CQLMessage::Value& value, faststring* mesg) {
  switch (value.kind) {
    case CQLMessage::Value::Kind::NOT_NULL:
      SerializeInt(value.value.size(), mesg);
      mesg->append(value.value);
      return;
    case CQLMessage::Value::Kind::IS_NULL:
      SerializeInt(-1, mesg);
      return;
    case CQLMessage::Value::Kind::NOT_SET: // NOT_SET value kind should appear in request msg only.
      break;
      // default: fall through
  }
  LOG(ERROR) << "Internal error: invalid/unknown value kind " << static_cast<uint32_t>(value.kind);
  SerializeInt(-1, mesg);
}
#endif

} // namespace

// ------------------------------------ CQL response -----------------------------------
CQLResponse::CQLResponse(const CQLRequest& request, const Opcode opcode)
    : CQLMessage(Header(request.version() | kResponseVersion, 0, request.stream_id(), opcode)) {
}

CQLResponse::CQLResponse(const StreamId stream_id, const Opcode opcode)
    : CQLMessage(Header(kCurrentVersion | kResponseVersion, 0, stream_id, opcode)) {
}

CQLResponse::~CQLResponse() {
}

// Short-hand macros for serializing fields from the message header
#define SERIALIZE_BYTE(buf, pos, value) \
  Store8(&(buf)[pos], static_cast<uint8_t>(value))
#define SERIALIZE_SHORT(buf, pos, value) \
  NetworkByteOrder::Store16(&(buf)[pos], static_cast<uint16_t>(value))
#define SERIALIZE_INT(buf, pos, value) \
  NetworkByteOrder::Store32(&(buf)[pos], static_cast<int32_t>(value))
#define SERIALIZE_LONG(buf, pos, value) \
  NetworkByteOrder::Store64(&(buf)[pos], static_cast<int64_t>(value))

void CQLResponse::Serialize(const CompressionScheme compression_scheme, faststring* mesg) const {
  const size_t start_pos = mesg->size(); // save the start position
  const bool compress = (compression_scheme != CQLMessage::CompressionScheme::kNone);
  SerializeHeader(compress, mesg);
  if (compress) {
    faststring body;
    SerializeBody(&body);
    switch (compression_scheme) {
      case CQLMessage::CompressionScheme::kLz4: {
        SerializeInt(static_cast<int32_t>(body.size()), mesg);
        const size_t curr_size = mesg->size();
        const int max_comp_size = LZ4_compressBound(body.size());
        mesg->resize(curr_size + max_comp_size);
        const int comp_size = LZ4_compress_default(to_char_ptr(body.data()),
                                                   to_char_ptr(mesg->data() + curr_size),
                                                   body.size(),
                                                   max_comp_size);
        CHECK_NE(comp_size, 0) << "LZ4 compression failed";
        mesg->resize(curr_size + comp_size);
        break;
      }
      case CQLMessage::CompressionScheme::kSnappy: {
        const size_t curr_size = mesg->size();
        const size_t max_comp_size = MaxCompressedLength(body.size());
        size_t comp_size = 0;
        mesg->resize(curr_size + max_comp_size);
        RawCompress(to_char_ptr(body.data()), body.size(),
                    to_char_ptr(mesg->data() + curr_size), &comp_size);
        mesg->resize(curr_size + comp_size);
        break;
      }
      case CQLMessage::CompressionScheme::kNone:
        LOG(FATAL) << "No compression scheme";
        break;
    }
  } else {
    SerializeBody(mesg);
  }
  SERIALIZE_INT(
      mesg->data(), start_pos + kHeaderPosLength, mesg->size() - start_pos - kMessageHeaderLength);
}

void CQLResponse::SerializeHeader(const bool compress, faststring* mesg) const {
  uint8_t buffer[kMessageHeaderLength];
  SERIALIZE_BYTE(buffer, kHeaderPosVersion, version());
  SERIALIZE_BYTE(buffer, kHeaderPosFlags, flags() | (compress ? kCompressionFlag : 0));
  SERIALIZE_SHORT(buffer, kHeaderPosStreamId, stream_id());
  SERIALIZE_INT(buffer, kHeaderPosLength, 0);
  SERIALIZE_BYTE(buffer, kHeaderPosOpcode, opcode());
  mesg->append(buffer, sizeof(buffer));
}

#undef SERIALIZE_BYTE
#undef SERIALIZE_SHORT
#undef SERIALIZE_INT
#undef SERIALIZE_LONG

// ------------------------------ Individual CQL responses -----------------------------------
ErrorResponse::ErrorResponse(const CQLRequest& request, const Code code, const string& message)
    : CQLResponse(request, Opcode::ERROR), code_(code), message_(message) {
}

ErrorResponse::ErrorResponse(const CQLRequest& request, const Code code, const Status& status)
    : CQLResponse(request, Opcode::ERROR), code_(code),
      message_(to_char_ptr(status.message().data())) {
}

ErrorResponse::ErrorResponse(const StreamId stream_id, const Code code, const string& message)
    : CQLResponse(stream_id, Opcode::ERROR), code_(code), message_(message) {
}

ErrorResponse::~ErrorResponse() {
}

void ErrorResponse::SerializeBody(faststring* mesg) const {
  SerializeInt(static_cast<int32_t>(code_), mesg);
  SerializeString(message_, mesg);
  SerializeErrorBody(mesg);
}

void ErrorResponse::SerializeErrorBody(faststring* mesg) const {
}

//----------------------------------------------------------------------------------------
UnpreparedErrorResponse::UnpreparedErrorResponse(const CQLRequest& request, const QueryId& query_id)
    : ErrorResponse(request, Code::UNPREPARED, "Unprepared query"), query_id_(query_id) {
}

UnpreparedErrorResponse::~UnpreparedErrorResponse() {
}

void UnpreparedErrorResponse::SerializeErrorBody(faststring* mesg) const {
  SerializeShortBytes(query_id_, mesg);
}

//----------------------------------------------------------------------------------------
ReadyResponse::ReadyResponse(const CQLRequest& request) : CQLResponse(request, Opcode::READY) {
}

ReadyResponse::~ReadyResponse() {
}

void ReadyResponse::SerializeBody(faststring* mesg) const {
  // Ready body is empty
}

//----------------------------------------------------------------------------------------
AuthenticateResponse::AuthenticateResponse(const CQLRequest& request, const string& authenticator)
    : CQLResponse(request, Opcode::AUTHENTICATE), authenticator_(authenticator) {
}

AuthenticateResponse::~AuthenticateResponse() {
}

void AuthenticateResponse::SerializeBody(faststring* mesg) const {
  SerializeString(authenticator_, mesg);
}

//----------------------------------------------------------------------------------------
SupportedResponse::SupportedResponse(const CQLRequest& request,
                                     const unordered_map<string, vector<string>>* options)
    : CQLResponse(request, Opcode::SUPPORTED), options_(options) {
}

SupportedResponse::~SupportedResponse() {
}

void SupportedResponse::SerializeBody(faststring* mesg) const {
  SerializeStringMultiMap(*options_, mesg);
}

//----------------------------------------------------------------------------------------
ResultResponse::ResultResponse(const CQLRequest& request, const Kind kind)
  : CQLResponse(request, Opcode::RESULT), kind_(kind) {
}

ResultResponse::~ResultResponse() {
}

ResultResponse::RowsMetadata::Type::Type(const Id id) : id(id) {
  switch (id) {
    // Verify that the type id is a primitive type indeed.
    case Id::ASCII:
    case Id::BIGINT:
    case Id::BLOB:
    case Id::BOOLEAN:
    case Id::COUNTER:
    case Id::DECIMAL:
    case Id::DOUBLE:
    case Id::FLOAT:
    case Id::INT:
    case Id::TIMESTAMP:
    case Id::UUID:
    case Id::VARCHAR:
    case Id::VARINT:
    case Id::TIMEUUID:
    case Id::INET:
    case Id::JSONB:
    case Id::DATE:
    case Id::TIME:
    case Id::SMALLINT:
    case Id::TINYINT:
      return;

    // Non-primitive types
    case Id::CUSTOM:
    case Id::LIST:
    case Id::MAP:
    case Id::SET:
    case Id::UDT:
    case Id::TUPLE:
      break;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: invalid/unknown primitive type id " << static_cast<uint32_t>(id);
}

// These union members in Type below are not initialized by default. They need to be explicitly
// initialized using the new() operator in the Type constructors.
ResultResponse::RowsMetadata::Type::Type(const string& custom_class_name) : id(Id::CUSTOM) {
  new(&this->custom_class_name) string(custom_class_name);
}

ResultResponse::RowsMetadata::Type::Type(const Id id, shared_ptr<const Type> element_type)
    : id(id) {
  switch (id) {
    case Id::LIST:
    case Id::SET:
      new(&this->element_type) shared_ptr<const Type>(element_type);
      return;

    // Not list nor map
    case Id::CUSTOM:
    case Id::ASCII:
    case Id::BIGINT:
    case Id::BLOB:
    case Id::BOOLEAN:
    case Id::COUNTER:
    case Id::DECIMAL:
    case Id::DOUBLE:
    case Id::FLOAT:
    case Id::INT:
    case Id::TIMESTAMP:
    case Id::UUID:
    case Id::VARCHAR:
    case Id::VARINT:
    case Id::TIMEUUID:
    case Id::INET:
    case Id::JSONB:
    case Id::DATE:
    case Id::TIME:
    case Id::SMALLINT:
    case Id::TINYINT:
    case Id::MAP:
    case Id::UDT:
    case Id::TUPLE:
      break;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: invalid/unknown list/map type id " << static_cast<uint32_t>(id);
}

ResultResponse::RowsMetadata::Type::Type(shared_ptr<const MapType> map_type) : id(Id::MAP) {
  new(&this->map_type) shared_ptr<const MapType>(map_type);
}

ResultResponse::RowsMetadata::Type::Type(shared_ptr<const UDTType> udt_type) : id(Id::UDT) {
  new(&this->udt_type) shared_ptr<const UDTType>(udt_type);
}

ResultResponse::RowsMetadata::Type::Type(
    shared_ptr<const TupleComponentTypes> tuple_component_types) : id(Id::TUPLE) {
  new(&this->tuple_component_types) shared_ptr<const TupleComponentTypes>(tuple_component_types);
}

ResultResponse::RowsMetadata::Type::Type(const Type& t) : id(t.id) {
  switch (id) {
    case Id::CUSTOM:
      new(&this->custom_class_name) string(t.custom_class_name);
      return;
    case Id::ASCII:
    case Id::BIGINT:
    case Id::BLOB:
    case Id::BOOLEAN:
    case Id::COUNTER:
    case Id::DECIMAL:
    case Id::DOUBLE:
    case Id::FLOAT:
    case Id::INT:
    case Id::TIMESTAMP:
    case Id::UUID:
    case Id::VARCHAR:
    case Id::VARINT:
    case Id::TIMEUUID:
    case Id::INET:
    case Id::JSONB:
    case Id::DATE:
    case Id::TIME:
    case Id::SMALLINT:
    case Id::TINYINT:
      return;
    case Id::LIST:
    case Id::SET:
      new(&element_type) shared_ptr<const Type>(t.element_type);
      return;
    case Id::MAP:
      new(&map_type) shared_ptr<const MapType>(t.map_type);
      return;
    case Id::UDT:
      new(&udt_type) shared_ptr<const UDTType>(t.udt_type);
      return;
    case Id::TUPLE:
      new(&tuple_component_types) shared_ptr<const TupleComponentTypes>(t.tuple_component_types);
      return;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: unknown type id " << static_cast<uint32_t>(id);
}

ResultResponse::RowsMetadata::Type::Type(const shared_ptr<QLType>& ql_type) {
  auto type = ql_type;
  if (type->IsFrozen()) {
    type = ql_type->param_type(0);
  }
  switch (type->main()) {
    case DataType::INT8:
      id = Id::TINYINT;
      return;
    case DataType::INT16:
      id = Id::SMALLINT;
      return;
    case DataType::INT32:
      id = Id::INT;
      return;
    case DataType::INT64:
      id = Id::BIGINT;
      return;
    case DataType::VARINT:
      id = Id::VARINT;
      return;
    case DataType::FLOAT:
      id = Id::FLOAT;
      return;
    case DataType::DOUBLE:
      id = Id::DOUBLE;
      return;
    case DataType::STRING:
      id = Id::VARCHAR;
      return;
    case DataType::BOOL:
      id = Id::BOOLEAN;
      return;
    case DataType::TIMESTAMP:
      id = Id::TIMESTAMP;
      return;
    case DataType::DATE:
      id = Id::DATE;
      return;
    case DataType::TIME:
      id = Id::TIME;
      return;
    case DataType::INET:
      id = Id::INET;
      return;
    case DataType::JSONB:
      id = Id::JSONB;
      return;
    case DataType::UUID:
      id = Id::UUID;
      return;
    case DataType::TIMEUUID:
      id = Id::TIMEUUID;
      return;
    case DataType::BINARY:
      id = Id::BLOB;
      return;
    case DataType::DECIMAL:
      id = Id::DECIMAL;
      return;
    case DataType::LIST:
      id = Id::LIST;
      new (&element_type) shared_ptr<const Type>(
          std::make_shared<const Type>(Type(type->param_type(0))));
      return;
    case DataType::SET:
      id = Id::SET;
      new (&element_type) shared_ptr<const Type>(
          std::make_shared<const Type>(Type(type->param_type(0))));
      return;
    case DataType::MAP: {
      id = Id::MAP;
      auto key = std::make_shared<const Type>(Type(type->param_type(0)));
      auto value = std::make_shared<const Type>(Type(type->param_type(1)));
      new (&map_type) shared_ptr<const MapType>(std::make_shared<MapType>(MapType{key, value}));
      return;
    }
    case DataType::USER_DEFINED_TYPE: {
      id = Id::UDT;
      std::vector<UDTType::Field> fields;
      for (int i = 0; i < type->params().size(); i++) {
        auto field_type = std::make_shared<const Type>(Type(type->param_type(i)));
        UDTType::Field field{type->udtype_field_name(i), field_type};
        fields.push_back(std::move(field));
      }
      new(&udt_type) shared_ptr<const UDTType>(std::make_shared<UDTType>(
          UDTType{type->udtype_keyspace_name(), type->udtype_name(), fields}));
      return;
    }
    case DataType::FROZEN: FALLTHROUGH_INTENDED;
    case DataType::NULL_VALUE_TYPE: FALLTHROUGH_INTENDED;
    case DataType::TUPLE: FALLTHROUGH_INTENDED;
    case DataType::TYPEARGS: FALLTHROUGH_INTENDED;

    case DataType::UINT8:  FALLTHROUGH_INTENDED;
    case DataType::UINT16: FALLTHROUGH_INTENDED;
    case DataType::UINT32: FALLTHROUGH_INTENDED;
    case DataType::UINT64: FALLTHROUGH_INTENDED;
    case DataType::UNKNOWN_DATA:
      break;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: invalid/unsupported type " << type->ToString();
}

ResultResponse::RowsMetadata::Type::~Type() {
  switch (id) {
    case Id::CUSTOM:
      custom_class_name.~basic_string();
      return;
    case Id::ASCII:
    case Id::BIGINT:
    case Id::BLOB:
    case Id::BOOLEAN:
    case Id::COUNTER:
    case Id::DECIMAL:
    case Id::DOUBLE:
    case Id::FLOAT:
    case Id::INT:
    case Id::TIMESTAMP:
    case Id::UUID:
    case Id::VARCHAR:
    case Id::VARINT:
    case Id::TIMEUUID:
    case Id::INET:
    case Id::JSONB:
    case Id::DATE:
    case Id::TIME:
    case Id::SMALLINT:
    case Id::TINYINT:
      return;
    case Id::LIST:
    case Id::SET:
      element_type.reset();
      return;
    case Id::MAP:
      map_type.reset();
      return;
    case Id::UDT:
      udt_type.reset();
      return;
    case Id::TUPLE:
      tuple_component_types.reset();
      return;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: unknown type id " << static_cast<uint32_t>(id);
}

ResultResponse::RowsMetadata::RowsMetadata()
    : flags(kNoMetadata),
      paging_state(""),
      global_table_spec("" /* keyspace */, "" /* table_name */),
      col_count(0) {
}

ResultResponse::RowsMetadata::RowsMetadata(const client::YBTableName& table_name,
                                           const vector<ColumnSchema>& columns,
                                           const string& paging_state,
                                           bool no_metadata)
    : flags((no_metadata ? kNoMetadata : kHasGlobalTableSpec) |
            (!paging_state.empty() ? kHasMorePages : 0)),
      paging_state(paging_state),
      global_table_spec(no_metadata ? "" : table_name.namespace_name(),
                        no_metadata ? "" : table_name.table_name()),
      col_count(columns.size()) {
  if (!no_metadata) {
    col_specs.reserve(col_count);
    for (const auto& column : columns) {
      col_specs.emplace_back(column.name(), Type(column.type()));
    }
  }
}

void ResultResponse::SerializeBody(faststring* mesg) const {
  SerializeInt(static_cast<int32_t>(kind_), mesg);
  SerializeResultBody(mesg);
}

void ResultResponse::SerializeType(const RowsMetadata::Type* type, faststring* mesg) const {
  SerializeShort(static_cast<uint16_t>(type->id), mesg);
  switch (type->id) {
    case RowsMetadata::Type::Id::CUSTOM:
      SerializeString(type->custom_class_name, mesg);
      return;
    case RowsMetadata::Type::Id::ASCII:
    case RowsMetadata::Type::Id::BIGINT:
    case RowsMetadata::Type::Id::BLOB:
    case RowsMetadata::Type::Id::BOOLEAN:
    case RowsMetadata::Type::Id::COUNTER:
    case RowsMetadata::Type::Id::DECIMAL:
    case RowsMetadata::Type::Id::DOUBLE:
    case RowsMetadata::Type::Id::FLOAT:
    case RowsMetadata::Type::Id::INT:
    case RowsMetadata::Type::Id::TIMESTAMP:
    case RowsMetadata::Type::Id::UUID:
    case RowsMetadata::Type::Id::VARCHAR:
    case RowsMetadata::Type::Id::VARINT:
    case RowsMetadata::Type::Id::TIMEUUID:
    case RowsMetadata::Type::Id::INET:
    case RowsMetadata::Type::Id::JSONB:
    case RowsMetadata::Type::Id::DATE:
    case RowsMetadata::Type::Id::TIME:
    case RowsMetadata::Type::Id::SMALLINT:
    case RowsMetadata::Type::Id::TINYINT:
      return;
    case RowsMetadata::Type::Id::LIST:
    case RowsMetadata::Type::Id::SET:
      SerializeType(type->element_type.get(), mesg);
      return;
    case RowsMetadata::Type::Id::MAP:
      SerializeType(type->map_type->key_type.get(), mesg);
      SerializeType(type->map_type->value_type.get(), mesg);
      return;
    case RowsMetadata::Type::Id::UDT:
      SerializeString(type->udt_type->keyspace, mesg);
      SerializeString(type->udt_type->name, mesg);
      SerializeShort(type->udt_type->fields.size(), mesg);
      for (const auto& field : type->udt_type->fields) {
        SerializeString(field.name, mesg);
        SerializeType(field.type.get(), mesg);
      }
      return;
    case RowsMetadata::Type::Id::TUPLE:
      SerializeShort(type->tuple_component_types->size(), mesg);
      for (const auto& component_type : *type->tuple_component_types) {
        SerializeType(component_type.get(), mesg);
      }
      return;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: unknown type id " << static_cast<uint32_t>(type->id);
}

void ResultResponse::SerializeColSpecs(
      const bool has_global_table_spec, const RowsMetadata::GlobalTableSpec& global_table_spec,
      const vector<RowsMetadata::ColSpec>& col_specs, faststring* mesg) const {
  if (has_global_table_spec) {
    SerializeString(global_table_spec.keyspace, mesg);
    SerializeString(global_table_spec.table, mesg);
  }
  for (const auto& col_spec : col_specs) {
    if (!has_global_table_spec) {
      SerializeString(col_spec.keyspace, mesg);
      SerializeString(col_spec.table, mesg);
    }
    SerializeString(col_spec.column, mesg);
    SerializeType(&col_spec.type, mesg);
  }
}

void ResultResponse::SerializeRowsMetadata(const RowsMetadata& metadata, faststring* mesg) const {
  SerializeInt(metadata.flags, mesg);
  SerializeInt(metadata.col_count, mesg);
  if (metadata.flags & RowsMetadata::kHasMorePages) {
    SerializeBytes(metadata.paging_state, mesg);
  }
  if (metadata.flags & RowsMetadata::kNoMetadata) {
    return;
  }
  CHECK_EQ(metadata.col_count, metadata.col_specs.size());
  SerializeColSpecs(
      metadata.flags & RowsMetadata::kHasGlobalTableSpec, metadata.global_table_spec,
      metadata.col_specs, mesg);
}

//----------------------------------------------------------------------------------------
VoidResultResponse::VoidResultResponse(const CQLRequest& request)
    : ResultResponse(request, Kind::VOID) {
}

VoidResultResponse::~VoidResultResponse() {
}

void VoidResultResponse::SerializeResultBody(faststring* mesg) const {
  // Void result response body is empty
}

//----------------------------------------------------------------------------------------
RowsResultResponse::RowsResultResponse(
    const QueryRequest& request, const ql::RowsResult::SharedPtr& result)
    : ResultResponse(request, Kind::ROWS), result_(result),
      skip_metadata_(request.params().flags & CQLMessage::QueryParameters::kSkipMetadataFlag) {
}

RowsResultResponse::RowsResultResponse(
    const ExecuteRequest& request, const ql::RowsResult::SharedPtr& result)
    : ResultResponse(request, Kind::ROWS), result_(result),
      skip_metadata_(request.params().flags & CQLMessage::QueryParameters::kSkipMetadataFlag) {
}

RowsResultResponse::RowsResultResponse(
    const BatchRequest& request, const ql::RowsResult::SharedPtr& result)
    : ResultResponse(request, Kind::ROWS), result_(result),
      skip_metadata_(false) { // Batches don't have the skip_metadata flag.
}

RowsResultResponse::~RowsResultResponse() {
}

void RowsResultResponse::SerializeResultBody(faststring* mesg) const {
  SerializeRowsMetadata(
      RowsMetadata(result_->table_name(), result_->column_schemas(),
                   result_->paging_state(), skip_metadata_), mesg);
  mesg->append(result_->rows_data());
}

//----------------------------------------------------------------------------------------
PreparedResultResponse::PreparedMetadata::PreparedMetadata() {
}

PreparedResultResponse::PreparedMetadata::PreparedMetadata(
    const client::YBTableName& table_name, const std::vector<int64_t>& hash_col_indices,
    const vector<client::YBTableName>& bind_table_names,
    const vector<ColumnSchema>& bind_variable_schemas)
    : flags(table_name.empty() ? 0 : kHasGlobalTableSpec),
      global_table_spec(table_name.namespace_name(), table_name.table_name()) {
  this->pk_indices.reserve(hash_col_indices.size());
  for (const size_t index : hash_col_indices) {
    this->pk_indices.emplace_back(static_cast<uint16_t>(index));
  }
  col_specs.reserve(bind_variable_schemas.size());
  for (int i = 0; i < bind_variable_schemas.size(); i++) {
    const ColumnSchema& var = bind_variable_schemas[i];
    if (flags & kHasGlobalTableSpec) {
      col_specs.emplace_back(var.name(), RowsMetadata::Type(var.type()));
    } else {
      col_specs.emplace_back(bind_table_names[i], var.name(), RowsMetadata::Type(var.type()));
    }
  }
}

PreparedResultResponse::PreparedResultResponse(const CQLRequest& request, const QueryId& query_id)
    : ResultResponse(request, Kind::PREPARED), query_id_(query_id) {
}

PreparedResultResponse::PreparedResultResponse(
    const CQLRequest& request, const QueryId& query_id, const ql::PreparedResult& result)
    : ResultResponse(request, Kind::PREPARED), query_id_(query_id),
      prepared_metadata_(result.table_name(), result.hash_col_indices(),
                         result.bind_table_names(), result.bind_variable_schemas()),
      rows_metadata_(!result.column_schemas().empty() ?
                     RowsMetadata(
                         result.table_name(), result.column_schemas(),
                         "" /* paging_state */, false /* no_metadata */) :
                     RowsMetadata()) {
}

PreparedResultResponse::~PreparedResultResponse() {
}

void PreparedResultResponse::SerializePreparedMetadata(
    const PreparedMetadata& metadata, faststring* mesg) const {
  SerializeInt(metadata.flags, mesg);
  SerializeInt(metadata.col_specs.size(), mesg);
  if (VersionIsCompatible(kV4Version)) {
    SerializeInt(metadata.pk_indices.size(), mesg);
    for (const auto& pk_index : metadata.pk_indices) {
      SerializeShort(pk_index, mesg);
    }
  }
  SerializeColSpecs(
      metadata.flags & PreparedMetadata::kHasGlobalTableSpec, metadata.global_table_spec,
      metadata.col_specs, mesg);
}

void PreparedResultResponse::SerializeResultBody(faststring* mesg) const {
  SerializeShortBytes(query_id_, mesg);
  SerializePreparedMetadata(prepared_metadata_, mesg);
  SerializeRowsMetadata(rows_metadata_, mesg);
}

//----------------------------------------------------------------------------------------
SetKeyspaceResultResponse::SetKeyspaceResultResponse(
    const CQLRequest& request, const ql::SetKeyspaceResult& result)
    : ResultResponse(request, Kind::SET_KEYSPACE), keyspace_(result.keyspace()) {
}

SetKeyspaceResultResponse::~SetKeyspaceResultResponse() {
}

void SetKeyspaceResultResponse::SerializeResultBody(faststring* mesg) const {
  SerializeString(keyspace_, mesg);
}

//----------------------------------------------------------------------------------------
SchemaChangeResultResponse::SchemaChangeResultResponse(
    const CQLRequest& request, const ql::SchemaChangeResult& result)
    : ResultResponse(request, Kind::SCHEMA_CHANGE),
      change_type_(result.change_type()), target_(result.object_type()),
      keyspace_(result.keyspace_name()), object_(result.object_name()) {
}

SchemaChangeResultResponse::~SchemaChangeResultResponse() {
}

void SchemaChangeResultResponse::Serialize(const CompressionScheme compression_scheme,
                                           faststring* mesg) const {
  ResultResponse::Serialize(compression_scheme, mesg);

  if (registered_events() & kSchemaChange) {
    // TODO: Replace this hack that piggybacks a SCHEMA_CHANGE event along a SCHEMA_CHANGE result
    // response with a formal event notification mechanism.
    SchemaChangeEventResponse event(change_type_, target_, keyspace_, object_, argument_types_);
    event.Serialize(compression_scheme, mesg);
  }
}

void SchemaChangeResultResponse::SerializeResultBody(faststring* mesg) const {
  SerializeString(change_type_, mesg);
  SerializeString(target_, mesg);
  if (target_ == "KEYSPACE") {
    SerializeString(keyspace_, mesg);
  } else if (target_ == "TABLE" || target_ == "TYPE") {
    SerializeString(keyspace_, mesg);
    SerializeString(object_, mesg);
  } else if (target_ == "FUNCTION" || target_ == "AGGREGATE") {
    SerializeString(keyspace_, mesg);
    SerializeString(object_, mesg);
    SerializeStringList(argument_types_, mesg);
  }
}

//----------------------------------------------------------------------------------------
EventResponse::EventResponse(const string& event_type)
    : CQLResponse(kEventStreamId, Opcode::EVENT), event_type_(event_type) {
}

EventResponse::~EventResponse() {
}

void EventResponse::SerializeBody(faststring* mesg) const {
  SerializeString(event_type_, mesg);
  SerializeEventBody(mesg);
}

std::string EventResponse::ToString() const {
  return event_type_ + ":" + BodyToString();
}

//----------------------------------------------------------------------------------------
TopologyChangeEventResponse::TopologyChangeEventResponse(const string& topology_change_type,
                                                         const Endpoint& node)
    : EventResponse(kTopologyChangeEvent), topology_change_type_(topology_change_type),
      node_(node) {
}

TopologyChangeEventResponse::~TopologyChangeEventResponse() {
}

void TopologyChangeEventResponse::SerializeEventBody(faststring* mesg) const {
  SerializeString(topology_change_type_, mesg);
  SerializeInet(node_, mesg);
}

std::string TopologyChangeEventResponse::BodyToString() const {
  return topology_change_type_;
}

//----------------------------------------------------------------------------------------
StatusChangeEventResponse::StatusChangeEventResponse(const string& status_change_type,
                                                     const Endpoint& node)
    : EventResponse(kStatusChangeEvent), status_change_type_(status_change_type),
      node_(node) {
}

StatusChangeEventResponse::~StatusChangeEventResponse() {
}

void StatusChangeEventResponse::SerializeEventBody(faststring* mesg) const {
  SerializeString(status_change_type_, mesg);
  SerializeInet(node_, mesg);
}

std::string StatusChangeEventResponse::BodyToString() const {
  return status_change_type_;
}

//----------------------------------------------------------------------------------------
const vector<string> SchemaChangeEventResponse::kEmptyArgumentTypes = {};

SchemaChangeEventResponse::SchemaChangeEventResponse(
    const string& change_type, const string& target,
    const string& keyspace, const string& object, const vector<string>& argument_types)
    : EventResponse(kSchemaChangeEvent), change_type_(change_type), target_(target),
      keyspace_(keyspace), object_(object), argument_types_(argument_types) {
}

SchemaChangeEventResponse::~SchemaChangeEventResponse() {
}

std::string SchemaChangeEventResponse::BodyToString() const {
  return change_type_;
}

void SchemaChangeEventResponse::SerializeEventBody(faststring* mesg) const {
  SerializeString(change_type_, mesg);
  SerializeString(target_, mesg);
  if (target_ == "KEYSPACE") {
    SerializeString(keyspace_, mesg);
  } else if (target_ == "TABLE" || target_ == "TYPE") {
    SerializeString(keyspace_, mesg);
    SerializeString(object_, mesg);
  } else if (target_ == "FUNCTION" || target_ == "AGGREGATE") {
    SerializeString(keyspace_, mesg);
    SerializeString(object_, mesg);
    SerializeStringList(argument_types_, mesg);
  }
}

//----------------------------------------------------------------------------------------
AuthChallengeResponse::AuthChallengeResponse(const CQLRequest& request, const string& token)
    : CQLResponse(request, Opcode::AUTH_CHALLENGE), token_(token) {
}

AuthChallengeResponse::~AuthChallengeResponse() {
}

void AuthChallengeResponse::SerializeBody(faststring* mesg) const {
  SerializeBytes(token_, mesg);
}

//----------------------------------------------------------------------------------------
AuthSuccessResponse::AuthSuccessResponse(const CQLRequest& request, const string& token)
    : CQLResponse(request, Opcode::AUTH_SUCCESS), token_(token) {
}

AuthSuccessResponse::~AuthSuccessResponse() {
}

void AuthSuccessResponse::SerializeBody(faststring* mesg) const {
  SerializeBytes(token_, mesg);
}

CQLServerEvent::CQLServerEvent(std::unique_ptr<EventResponse> event_response)
    : event_response_(std::move(event_response)) {
  CHECK_NOTNULL(event_response_.get());
  faststring temp;
  event_response_->Serialize(CQLMessage::CompressionScheme::kNone, &temp);
  serialized_response_ = RefCntBuffer(temp);
}

void CQLServerEvent::Serialize(boost::container::small_vector_base<RefCntBuffer>* output) const {
  output->push_back(serialized_response_);
}

std::string CQLServerEvent::ToString() const {
  return event_response_->ToString();
}

CQLServerEventList::CQLServerEventList() {
}

void CQLServerEventList::Transferred(const Status& status, rpc::Connection*) {
  if (!status.ok()) {
    LOG(WARNING) << "Transfer of CQL server event failed: " << status.ToString();
  }
}

void CQLServerEventList::Serialize(
    boost::container::small_vector_base<RefCntBuffer>* output) {
  for (const auto& cql_server_event : cql_server_events_) {
    cql_server_event->Serialize(output);
  }
}

std::string CQLServerEventList::ToString() const {
  std::string ret = "";
  for (const auto& cql_server_event : cql_server_events_) {
    if (!ret.empty()) {
      ret += ", ";
    }
    ret += cql_server_event->ToString();
  }
  return ret;
}

void CQLServerEventList::AddEvent(std::unique_ptr<CQLServerEvent> event) {
  cql_server_events_.push_back(std::move(event));
}

}  // namespace cqlserver
}  // namespace yb
