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
//
// This file contains the CQLRequest and CQLResponse classes that implement the CQL protocol for
// the CQL server. For the protocol spec, see https://github.com/apache/cassandra/tree/trunk/doc
//   - native_protocol_v3.spec
//   - native_protocol_v4.spec
//   - native_protocol_v5.spec

#ifndef YB_YQL_CQL_CQLSERVER_CQL_MESSAGE_H_
#define YB_YQL_CQL_CQLSERVER_CQL_MESSAGE_H_

#include <stdint.h>
#include <memory>
#include <set>
#include <unordered_map>

#include "yb/common/wire_protocol.h"
#include "yb/rpc/server_event.h"
#include "yb/yql/cql/ql/util/statement_params.h"
#include "yb/yql/cql/ql/util/statement_result.h"

#include "yb/util/memory/memory_usage.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"
#include "yb/util/net/sockaddr.h"

namespace yb {
namespace cqlserver {

class CQLRequest;
class CQLResponse;

// ---------------------------------- Generic CQL message ---------------------------------
class CQLMessage {

 public:
  //
  // Each CQL message consists of a 9-byte header:
  //   0         8        16        24        32         40
  //   +---------+---------+---------+---------+---------+
  //   | version |  flags  |      stream       | opcode  |
  //   +---------+---------+---------+---------+---------+
  //   |                length                 |
  //   +---------+---------+---------+---------+
  //   |            ...  body ...              |
  //   +----------------------------------------
  static constexpr int32_t kHeaderPosVersion    = 0;
  static constexpr int32_t kHeaderPosFlags      = 1;
  static constexpr int32_t kHeaderPosStreamId   = 2;
  static constexpr int32_t kHeaderPosOpcode     = 4;
  static constexpr int32_t kHeaderPosLength     = 5;
  static constexpr int32_t kMessageHeaderLength = 9;

  // Datatypes for CQL message header
  using Version = uint8_t;
  static constexpr Version kV3Version       = 0x03;
  static constexpr Version kV4Version       = 0x04; // Current Cassandra production version
  static constexpr Version kMinimumVersion  = kV3Version;
  static constexpr Version kCurrentVersion  = kV4Version;
  static constexpr Version kV5Version       = 0x05; // In development. See JIRA CASSANDRA-9362.
  static constexpr Version kResponseVersion = 0x80;

  using Flags = uint8_t;
  static constexpr Flags kCompressionFlag   = 0x01;
  static constexpr Flags kTracingFlag       = 0x02;
  static constexpr Flags kCustomPayloadFlag = 0x04; // Since V4
  static constexpr Flags kWarningFlag       = 0x08; // Since V4

  using StreamId = uint16_t;
  static constexpr StreamId kEventStreamId = 0xffff; // Special stream id for events.

  enum class Opcode : uint8_t {
    ERROR          = 0x00,
    STARTUP        = 0x01,
    READY          = 0x02,
    AUTHENTICATE   = 0x03,
    OPTIONS        = 0x05,
    SUPPORTED      = 0x06,
    QUERY          = 0x07,
    RESULT         = 0x08,
    PREPARE        = 0x09,
    EXECUTE        = 0x0A,
    REGISTER       = 0x0B,
    EVENT          = 0x0C,
    BATCH          = 0x0D,
    AUTH_CHALLENGE = 0x0E,
    AUTH_RESPONSE  = 0x0F,
    AUTH_SUCCESS   = 0x10
  };

  struct Header {
    const Version  version;
    const Flags    flags;
    const StreamId stream_id;
    const Opcode   opcode;

    Header(const Version version, const Flags flags, const StreamId stream_id, const Opcode opcode)
       : version(version), flags(flags), stream_id(stream_id), opcode(opcode) { }
    Header(const Version version, const StreamId stream_id, const Opcode opcode)
       : version(version), flags(0), stream_id(stream_id), opcode(opcode) { }
  };

  // STARTUP options.
  static constexpr char kCQLVersionOption[] = "CQL_VERSION";
  static constexpr char kCompressionOption[] = "COMPRESSION";
  static constexpr char kNoCompactOption[] = "NO_COMPACT";

  // Message body compression schemes.
  enum class CompressionScheme {
    kNone = 0,
    kLz4,
    kSnappy
  };
  static constexpr char kLZ4Compression[] = "lz4";
  static constexpr char kSnappyCompression[] = "snappy";

  // Supported events.
  static constexpr char kTopologyChangeEvent[] = "TOPOLOGY_CHANGE";
  static constexpr char kStatusChangeEvent[] = "STATUS_CHANGE";
  static constexpr char kSchemaChangeEvent[] = "SCHEMA_CHANGE";

  using Events = uint8_t;
  static constexpr Events kNoEvents       = 0x00;
  static constexpr Events kTopologyChange = 0x01;
  static constexpr Events kStatusChange   = 0x02;
  static constexpr Events kSchemaChange   = 0x04;
  static constexpr Events kAllEvents      = kTopologyChange | kStatusChange | kSchemaChange;

  // Basic datatype mapping for CQL message body:
  //   Int        -> int32_t
  //   Long       -> int64_t
  //   Byte       -> uint8_t
  //   Short      -> uint16_t
  //   String     -> std::string
  //   LongString -> std::string
  //   UUID       -> std::string
  //   TimeUUID   -> std::string
  //   StringList -> std::vector<String>
  //   Bytes      -> std::string
  //   ShortBytes -> std::string
  //   Inet       -> Endpoint
  //   Consistency    -> uint16_t
  //   StringMap      -> std::unordered_map<std::string, std::string>
  //   StringMultiMap -> std::unordered_map<std::string, std::vector<String>>
  //   BytesMap       -> std::unordered_map<std::string, std::string> (Since V4)
  static constexpr size_t kIntSize   = 4;
  static constexpr size_t kLongSize  = 8;
  static constexpr size_t kByteSize  = 1;
  static constexpr size_t kShortSize = 2;
  static constexpr size_t kUUIDSize  = 16;
  static constexpr size_t kConsistencySize = 2;
  enum class Consistency : uint16_t {
    ANY          = 0x0000,
    ONE          = 0x0001,
    TWO          = 0x0002,
    THREE        = 0x0003,
    QUORUM       = 0x0004,
    ALL          = 0x0005,
    LOCAL_QUORUM = 0x0006,
    EACH_QUORUM  = 0x0007,
    SERIAL       = 0x0008,
    LOCAL_SERIAL = 0x0009,
    LOCAL_ONE    = 0x000A
  };

  struct Value {
    enum class Kind {
      NOT_NULL =  0,
      IS_NULL  = -1, // Since V4
      NOT_SET  = -2  // Since V4
    };

    Kind kind = Kind::NOT_NULL;
    std::string name;
    std::string value; // As required by QLValue::Deserialize() for CQL, the value includes
                       // the 4-byte length header, i.e. "<4-byte-length><value>".
  };

  // Id of a prepared query for PREPARE, EXECUTE and BATCH requests.
  using QueryId = std::string;

  // Query parameters for QUERY, EXECUTE and BATCH requests
  struct QueryParameters : ql::StatementParameters {
    using Flags = uint8_t;
    static constexpr Flags kWithValuesFlag            = 0x01;
    static constexpr Flags kSkipMetadataFlag          = 0x02;
    static constexpr Flags kWithPageSizeFlag          = 0x04;
    static constexpr Flags kWithPagingStateFlag       = 0x08;
    static constexpr Flags kWithSerialConsistencyFlag = 0x10;
    static constexpr Flags kWithDefaultTimestampFlag  = 0x20;
    static constexpr Flags kWithNamesForValuesFlag    = 0x40;

    Consistency consistency = Consistency::ANY;
    Flags flags = 0;
    std::vector<Value> values;
    std::unordered_map<std::string, std::vector<Value>::size_type> value_map;
    Consistency serial_consistency = Consistency::ANY;
    int64_t default_timestamp = 0;

    QueryParameters() : ql::StatementParameters() { }

    virtual CHECKED_STATUS GetBindVariable(const std::string& name,
                                           int64_t pos,
                                           const std::shared_ptr<QLType>& type,
                                           QLValue* value) const override;

    CHECKED_STATUS ValidateConsistency();
  };

  // Accessors for header fields
  Version version()    const { return header_.version;   }
  Flags flags()        const { return header_.flags;     }
  StreamId stream_id() const { return header_.stream_id; }
  Opcode opcode()      const { return header_.opcode;    }

 protected:
  explicit CQLMessage(const Header& header) : header_(header) { }
  virtual ~CQLMessage() { }

  bool VersionIsCompatible(const Version min_version) const {
    return (version() & ~kResponseVersion) >= min_version;
  }

  const Header header_;
};

// ------------------------------------ CQL request -----------------------------------
class CQLRequest : public CQLMessage {
 public:
  // "Factory" function to parse a CQL serlized request message and construct a request object.
  // Return true iff a request is parsed successfully without error. If an error occurs, an error
  // response will be returned instead and it should be sent back to the CQL client.
  static bool ParseRequest(
      const Slice& mesg, CompressionScheme compression_scheme,
      std::unique_ptr<CQLRequest>* request, std::unique_ptr<CQLResponse>* error_response);

  static StreamId ParseStreamId(const Slice& mesg) {
    return static_cast<StreamId>(NetworkByteOrder::Load16(mesg.data() + kHeaderPosStreamId));
  }

  virtual ~CQLRequest();

 protected:
  CQLRequest(const Header& header, const Slice& body);

  // Function to parse a request body that all CQLRequest subclasses need to implement
  virtual CHECKED_STATUS ParseBody() = 0;

  // Parse a CQL number (8, 16, 32 and 64-bit integer). <num_type> is the parsed integer type.
  // <converter> converts the number from network byte-order to machine order and <data_type>
  // is the coverter's return type. The converter's return type <data_type> is unsigned while
  // <num_type> may be signed or unsigned.
  template<typename num_type, typename data_type>
  inline CHECKED_STATUS ParseNum(
      const char* type_name, data_type (*converter)(const void*), num_type* val) {
    RETURN_NOT_OK(CQLDecodeNum(sizeof(num_type), converter, &body_, val));
    DVLOG(4) << type_name << " " << static_cast<int64_t>(*val);
    return Status::OK();
  }

  // Parse a CQL byte stream (string or bytes). <len_type> is the parsed length type.
  // <len_parser> parses the byte length from network byte-order to machine order.
  template<typename len_type>
  inline CHECKED_STATUS ParseBytes(
      const char* type_name, Status (CQLRequest::*len_parser)(len_type*), std::string* val) {
    len_type len = 0;
    RETURN_NOT_OK((this->*len_parser)(&len));
    RETURN_NOT_OK(CQLDecodeBytes(static_cast<size_t>(len), &body_, val));
    DVLOG(4) << type_name << " " << *val;
    return Status::OK();
  }

  inline CHECKED_STATUS ParseByte(uint8_t* value) {
    static_assert(sizeof(*value) == kByteSize, "inconsistent byte size");
    return ParseNum("CQL byte", Load8, value);
  }
  inline CHECKED_STATUS ParseShort(uint16_t* value) {
    static_assert(sizeof(*value) == kShortSize, "inconsistent short size");
    return ParseNum("CQL byte", NetworkByteOrder::Load16, value);
  }
  inline CHECKED_STATUS ParseInt(int32_t* value) {
    static_assert(sizeof(*value) == kIntSize, "inconsistent int size");
    return ParseNum("CQL int", NetworkByteOrder::Load32, value);
  }
  inline CHECKED_STATUS ParseLong(int64_t* value) {
    static_assert(sizeof(*value) == kLongSize, "inconsistent long size");
    return ParseNum("CQL long", NetworkByteOrder::Load64, value);
  }
  inline CHECKED_STATUS ParseString(std::string* value)  {
    return ParseBytes("CQL string", &CQLRequest::ParseShort, value);
  }
  inline CHECKED_STATUS ParseLongString(std::string* value)  {
    return ParseBytes("CQL long string", &CQLRequest::ParseInt, value);
  }
  inline CHECKED_STATUS ParseShortBytes(std::string* value) {
    return ParseBytes("CQL short bytes", &CQLRequest::ParseShort, value);
  }
  inline CHECKED_STATUS ParseBytes(std::string* value) {
    return ParseBytes("CQL bytes", &CQLRequest::ParseInt, value);
  }
  inline CHECKED_STATUS ParseConsistency(Consistency* consistency) {
    static_assert(sizeof(*consistency) == kConsistencySize, "inconsistent consistency size");
    return ParseNum("CQL consistency", NetworkByteOrder::Load16, consistency);
  }
  CHECKED_STATUS ParseUUID(std::string* value);
  CHECKED_STATUS ParseTimeUUID(std::string* value);
  CHECKED_STATUS ParseStringList(std::vector<std::string>* list);
  CHECKED_STATUS ParseInet(Endpoint* value);
  CHECKED_STATUS ParseStringMap(std::unordered_map<std::string, std::string>* map);
  CHECKED_STATUS ParseStringMultiMap(
      std::unordered_map<std::string, std::vector<std::string>>* map);
  CHECKED_STATUS ParseBytesMap(std::unordered_map<std::string, std::string>* map);
  CHECKED_STATUS ParseValue(bool with_name, Value* value);
  CHECKED_STATUS ParseQueryParameters(QueryParameters* params);

 private:
  Slice body_;
};

// ------------------------------ Individual CQL requests -----------------------------------
class StartupRequest : public CQLRequest {
 public:
  StartupRequest(const Header& header, const Slice& body);
  virtual ~StartupRequest() override;

  const std::unordered_map<std::string, std::string>& options() const { return options_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:
  std::unordered_map<std::string, std::string> options_;
};

//------------------------------------------------------------
class AuthResponseRequest : public CQLRequest {
 public:
  class AuthQueryParameters : public ql::StatementParameters {
   public:
    AuthQueryParameters() : ql::StatementParameters() {}

    CHECKED_STATUS GetBindVariable(const std::string& name,
                                   int64_t pos,
                                   const std::shared_ptr<QLType>& type,
                                   QLValue* value) const override;
    std::string username;
    std::string password;
  };

  AuthResponseRequest(const Header& header, const Slice& body);
  virtual ~AuthResponseRequest() override;

  const std::string& token() const { return token_; }
  const AuthQueryParameters& params() const { return params_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:
  std::string token_;
  AuthQueryParameters params_;
};

//------------------------------------------------------------
class OptionsRequest : public CQLRequest {
 public:
  OptionsRequest(const Header& header, const Slice& body);
  virtual ~OptionsRequest() override;

 protected:
  virtual CHECKED_STATUS ParseBody() override;
};

//------------------------------------------------------------
class QueryRequest : public CQLRequest {
 public:
  QueryRequest(const Header& header, const Slice& body);
  virtual ~QueryRequest() override;

  const std::string& query() const { return query_; }
  const QueryParameters& params() const { return params_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:
  std::string query_;
  QueryParameters params_;
};

//------------------------------------------------------------
class PrepareRequest : public CQLRequest {
 public:
  PrepareRequest(const Header& header, const Slice& body);
  virtual ~PrepareRequest() override;

  const std::string& query() const { return query_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:
  std::string query_;
};

//------------------------------------------------------------
class ExecuteRequest : public CQLRequest {
 public:
  ExecuteRequest(const Header& header, const Slice& body);
  virtual ~ExecuteRequest() override;

  const QueryId& query_id() const { return query_id_; }
  const QueryParameters& params() const { return params_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:
  QueryId query_id_;
  QueryParameters params_;
};

//------------------------------------------------------------
class BatchRequest : public CQLRequest {
 public:
  enum class Type : uint8_t {
    LOGGED   = 0x00,
    UNLOGGED = 0x01,
    COUNTER  = 0x02
  };
  struct Query {
    bool is_prepared = false;
    QueryId query_id;
    std::string query;
    QueryParameters params;
  };

  BatchRequest(const Header& header, const Slice& body);
  virtual ~BatchRequest() override;

  const std::vector<Query>& queries() const { return queries_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:

  Type type_ = Type::LOGGED;
  std::vector<Query> queries_;
};

//------------------------------------------------------------
class RegisterRequest : public CQLRequest {
 public:
  RegisterRequest(const Header& header, const Slice& body);
  virtual ~RegisterRequest() override;

  Events events() const { return events_; }

 protected:
  virtual CHECKED_STATUS ParseBody() override;

 private:
  Events events_;
};

// ------------------------------------ CQL response -----------------------------------
class CQLResponse : public CQLMessage {
 public:
  virtual ~CQLResponse();
  virtual void Serialize(CompressionScheme compression_scheme, faststring* mesg) const;

  Events registered_events() const { return registered_events_; }
  void set_registered_events(Events events) { registered_events_ = events; }

 protected:
  CQLResponse(const CQLRequest& request, Opcode opcode);
  CQLResponse(StreamId stream_id, Opcode opcode);
  void SerializeHeader(bool compress, faststring* mesg) const;

  // Function to serialize a response body that all CQLResponse subclasses need to implement
  virtual void SerializeBody(faststring* mesg) const = 0;

 private:
  Events registered_events_ = kNoEvents;
};

// ------------------------------ Individual CQL responses -----------------------------------
class ErrorResponse : public CQLResponse {
 public:
  enum class Code : int32_t {
    SERVER_ERROR     = 0x0000,
    PROTOCOL_ERROR   = 0x000A,
    BAD_CREDENTIALS  = 0x0100,
    UNAVAILABLE      = 0x1000,
    OVERLOADED       = 0x1001,
    IS_BOOTSTRAPPING = 0x1002,
    TRUNCATE_ERROR   = 0x1003,
    WRITE_TIMEOUT    = 0x1100,
    READ_TIMEOUT     = 0x1200,
    READ_FAILURE     = 0x1300, // Since V4
    FUNCTION_FAILURE = 0x1400, // Since V4
    WRITE_FAILURE    = 0x1500, // Since V4
    SYNTAX_ERROR     = 0x2000,
    UNAUTHORIZED     = 0x2100,
    INVALID          = 0x2200,
    CONFIG_ERROR     = 0x2300,
    ALREADY_EXISTS   = 0x2400,
    UNPREPARED       = 0x2500,
  };

  // Construct an error response for the request, or by just the stream id if the request object
  // is not constructed (e.g. when the request opcode is invalid).
  ErrorResponse(const CQLRequest& request, Code code, const std::string& message);
  ErrorResponse(const CQLRequest& request, Code code, const Status& status);
  ErrorResponse(StreamId stream_id, Code code, const std::string& message);

  virtual ~ErrorResponse() override;

  const std::string& message() const {
    return message_;
  }

 protected:
  virtual void SerializeBody(faststring* mesg) const override;
  virtual void SerializeErrorBody(faststring* mesg) const;

 private:
  const Code code_;
  const std::string message_;
};

//------------------------------------------------------------
class UnpreparedErrorResponse : public ErrorResponse {
 public:
  explicit UnpreparedErrorResponse(const CQLRequest& request, const QueryId& query_id);
  virtual ~UnpreparedErrorResponse() override;

 protected:
  virtual void SerializeErrorBody(faststring* mesg) const override;

 private:
  const QueryId query_id_;
};

//------------------------------------------------------------
class ReadyResponse : public CQLResponse {
 public:
  explicit ReadyResponse(const CQLRequest& request);
  virtual ~ReadyResponse() override;

 protected:
  virtual void SerializeBody(faststring* mesg) const override;
};

//------------------------------------------------------------
class AuthenticateResponse : public CQLResponse {
 public:
  AuthenticateResponse(const CQLRequest& request, const std::string& authenticator);
  virtual ~AuthenticateResponse() override;

 protected:
  virtual void SerializeBody(faststring* mesg) const override;

 private:
  const std::string authenticator_;
};

//------------------------------------------------------------
class SupportedResponse : public CQLResponse {
 public:
  explicit SupportedResponse(
      const CQLRequest& request,
      const std::unordered_map<std::string, std::vector<std::string>>* options);
  virtual ~SupportedResponse() override;

 protected:
  virtual void SerializeBody(faststring* mesg) const override;

 private:
  const std::unordered_map<std::string, std::vector<std::string>>* options_;
};

//------------------------------------------------------------
class ResultResponse : public CQLResponse {
 public:
  virtual ~ResultResponse() override;

 protected:
  enum class Kind : int32_t {
    VOID          = 0x00000001,
    ROWS          = 0x00000002,
    SET_KEYSPACE  = 0x00000003,
    PREPARED      = 0x00000004,
    SCHEMA_CHANGE = 0x00000005
  };

  // Metadata for ROWS resultset
  struct RowsMetadata {
    using Flags = int32_t;
    static constexpr Flags kHasGlobalTableSpec = 0x00000001;
    static constexpr Flags kHasMorePages       = 0x00000002;
    static constexpr Flags kNoMetadata         = 0x00000004;

    Flags flags = 0;
    std::string paging_state;

    struct GlobalTableSpec {
      std::string keyspace;
      std::string table;

      GlobalTableSpec() { }
      GlobalTableSpec(const std::string& keyspace, const std::string& table)
          : keyspace(keyspace), table(table) { }
    };
    GlobalTableSpec global_table_spec;

    struct Type {
      enum class Id : uint16_t {
        CUSTOM    = 0x0000,
        ASCII     = 0x0001,
        BIGINT    = 0x0002,
        BLOB      = 0x0003,
        BOOLEAN   = 0x0004,
        COUNTER   = 0x0005,
        DECIMAL   = 0x0006,
        DOUBLE    = 0x0007,
        FLOAT     = 0x0008,
        INT       = 0x0009,
        TIMESTAMP = 0x000B,
        UUID      = 0x000C,
        VARCHAR   = 0x000D,
        VARINT    = 0x000E,
        TIMEUUID  = 0x000F,
        INET      = 0x0010,
        DATE      = 0x0011, // Since V4
        TIME      = 0x0012, // Since V4
        SMALLINT  = 0x0013, // Since V4
        TINYINT   = 0x0014, // Since V4
        LIST      = 0x0020,
        MAP       = 0x0021,
        SET       = 0x0022,
        UDT       = 0x0030,
        TUPLE     = 0x0031,
        JSONB     = 0x0080  // Yugabyte specific types start here
      };

      Id id = Id::CUSTOM;

      struct MapType {
        std::shared_ptr<const Type> key_type;
        std::shared_ptr<const Type> value_type;
      };

      struct UDTType {
        std::string keyspace;
        std::string name;
        struct Field {
          std::string name;
          std::shared_ptr<const Type> type;
        };
        std::vector<Field> fields;
      };

      typedef std::vector<std::shared_ptr<const Type>> TupleComponentTypes;

      // The use of union to store strings and shared pointers needs to be done with great care
      // since constructors and destructors won't be called automatically.
      // TODO(Robert): an alternate idea will be to create a type hierarchy of individual types
      // and allocate it out of a memory pool to avoid allocation overhead and memory
      // fragmentation. Revisit it when we return real query results from tserver.
      union {
        std::string custom_class_name;
        std::shared_ptr<const Type> element_type; // for list or set
        std::shared_ptr<const MapType> map_type;
        std::shared_ptr<const UDTType> udt_type;
        std::shared_ptr<const TupleComponentTypes> tuple_component_types;
      };

      explicit Type(Id id); // for primitive type
      explicit Type(const std::string& custom_class_name);
               Type(Id id, std::shared_ptr<const Type> element_type); // for list or set
      explicit Type(std::shared_ptr<const MapType> map_type);
      explicit Type(std::shared_ptr<const UDTType> udt_type);
      explicit Type(std::shared_ptr<const TupleComponentTypes> tuple_component_types);
      explicit Type(const Type& t);
      explicit Type(const std::shared_ptr<QLType>& type);
      ~Type();
    };
    struct ColSpec {
      std::string keyspace;
      std::string table;
      std::string column;
      Type type;

      ColSpec(const client::YBTableName& table_name, const std::string& column, const Type& type)
          : keyspace(table_name.namespace_name()), table(table_name.table_name()),
            column(column), type(type) {}
      ColSpec(const std::string& column, const Type& type) : column(column), type(type) {}
    };
    int32_t col_count;
    std::vector<ColSpec> col_specs;

    RowsMetadata();
    RowsMetadata(const client::YBTableName& table_name,
                 const std::vector<ColumnSchema>& columns,
                 const std::string& paging_state,
                 bool no_metadata);
  };

  ResultResponse(const CQLRequest& request, Kind kind);
  virtual void SerializeBody(faststring* mesg) const override;

  // Function to serialize a result body that all ResultResponse subclasses need to implement
  virtual void SerializeResultBody(faststring* mesg) const = 0;

  // Helper serialize functions
  void SerializeType(const RowsMetadata::Type* type, faststring* mesg) const;
  void SerializeColSpecs(
      bool has_global_table_spec, const RowsMetadata::GlobalTableSpec& global_table_spec,
      const std::vector<RowsMetadata::ColSpec>& col_specs, faststring* mesg) const;
  void SerializeRowsMetadata(const RowsMetadata& metadata, faststring* mesg) const;

 private:
  const Kind kind_;
};

//------------------------------------------------------------
class VoidResultResponse : public ResultResponse {
 public:
  explicit VoidResultResponse(const CQLRequest& request);
  virtual ~VoidResultResponse() override;

 protected:
  virtual void SerializeResultBody(faststring* mesg) const override;
};

//------------------------------------------------------------
class RowsResultResponse : public ResultResponse {
 public:
  RowsResultResponse(const QueryRequest& request, const ql::RowsResult::SharedPtr& result);
  RowsResultResponse(const ExecuteRequest& request, const ql::RowsResult::SharedPtr& result);
  RowsResultResponse(const BatchRequest& request, const ql::RowsResult::SharedPtr& result);

  virtual ~RowsResultResponse() override;

 protected:
  virtual void SerializeResultBody(faststring* mesg) const override;

 private:
  const ql::RowsResult::SharedPtr result_;
  const bool skip_metadata_;
};

//------------------------------------------------------------
class SetKeyspaceResultResponse : public ResultResponse {
 public:
  SetKeyspaceResultResponse(const CQLRequest& request, const ql::SetKeyspaceResult& result);
  virtual ~SetKeyspaceResultResponse() override;
 protected:
  virtual void SerializeResultBody(faststring* mesg) const override;
 private:
  const std::string keyspace_;
};

//------------------------------------------------------------
class PreparedResultResponse : public ResultResponse {
 public:
  PreparedResultResponse(const CQLRequest& request, const QueryId& query_id);
  PreparedResultResponse(
      const CQLRequest& request, const QueryId& query_id, const ql::PreparedResult& result);
  virtual ~PreparedResultResponse() override;

 protected:
  virtual void SerializeResultBody(faststring* mesg) const override;

 private:
  struct PreparedMetadata {
    using Flags = int32_t;
    static constexpr Flags kHasGlobalTableSpec = 0x00000001;

    Flags flags = 0;
    std::vector<uint16_t> pk_indices;
    RowsMetadata::GlobalTableSpec global_table_spec;
    std::vector<RowsMetadata::ColSpec> col_specs;

    PreparedMetadata();
    PreparedMetadata(
        const client::YBTableName& table_name, const std::vector<int64_t>& hash_col_indices,
        const std::vector<client::YBTableName>& bind_table_names,
        const std::vector<ColumnSchema>& bind_variable_schemas);
  };

  void SerializePreparedMetadata(const PreparedMetadata& metadata, faststring* mesg) const;

  const QueryId query_id_;
  const PreparedMetadata prepared_metadata_;
  const RowsMetadata rows_metadata_;
};

//------------------------------------------------------------
class SchemaChangeResultResponse : public ResultResponse {
 public:
  SchemaChangeResultResponse(const CQLRequest& request, const ql::SchemaChangeResult& result);
  virtual ~SchemaChangeResultResponse() override;

  void Serialize(CompressionScheme compression_scheme, faststring* mesg) const override;

 protected:
  virtual void SerializeResultBody(faststring* mesg) const override;

 private:
  const std::string change_type_;
  const std::string target_;
  const std::string keyspace_;
  const std::string object_;
  const std::vector<std::string> argument_types_;
};

//------------------------------------------------------------
class EventResponse : public CQLResponse {
 public:
  virtual std::string ToString() const;
  virtual ~EventResponse() override;
  virtual size_t ObjectSize() const = 0;
  virtual size_t DynamicMemoryUsage() const = 0;

 protected:
  explicit EventResponse(const std::string& event_type);
  virtual void SerializeBody(faststring* mesg) const override;
  virtual void SerializeEventBody(faststring* mesg) const = 0;
  virtual std::string BodyToString() const = 0;
 private:
  const std::string event_type_;
};

//------------------------------------------------------------
class TopologyChangeEventResponse : public EventResponse {
 public:
  static constexpr const char* const kMovedNode = "MOVED_NODE";
  static constexpr const char* const kNewNode = "NEW_NODE";
  virtual ~TopologyChangeEventResponse() override;
  TopologyChangeEventResponse(const std::string& topology_change_type, const Endpoint& node);
  size_t ObjectSize() const override { return sizeof(*this); }
  size_t DynamicMemoryUsage() const override { return DynamicMemoryUsageOf(topology_change_type_); }

 protected:
  virtual void SerializeEventBody(faststring* mesg) const override;
  std::string BodyToString() const override;

 private:
  const std::string topology_change_type_;
  const Endpoint node_;
};

//------------------------------------------------------------
class StatusChangeEventResponse : public EventResponse {
 public:
  virtual ~StatusChangeEventResponse() override;
  size_t ObjectSize() const override { return sizeof(*this); }
  size_t DynamicMemoryUsage() const override { return DynamicMemoryUsageOf(status_change_type_); }

 protected:
  virtual void SerializeEventBody(faststring* mesg) const override;
  std::string BodyToString() const override;

 private:
  StatusChangeEventResponse(const std::string& status_change_type, const Endpoint& node);

  const std::string status_change_type_;
  const Endpoint node_;
};

//------------------------------------------------------------
class SchemaChangeEventResponse : public EventResponse {
 public:
  SchemaChangeEventResponse(
      const std::string& change_type, const std::string& target,
      const std::string& keyspace, const std::string& object = "",
      const std::vector<std::string>& argument_types = kEmptyArgumentTypes);
  virtual ~SchemaChangeEventResponse() override;
  size_t ObjectSize() const override { return sizeof(*this); }
  size_t DynamicMemoryUsage() const override {
    return DynamicMemoryUsageOf(change_type_, target_, keyspace_, object_, argument_types_);
  }

 protected:
  virtual void SerializeEventBody(faststring* mesg) const override;
  std::string BodyToString() const override;

 private:
  static const std::vector<std::string> kEmptyArgumentTypes;

  const std::string change_type_;
  const std::string target_;
  const std::string keyspace_;
  const std::string object_;
  const std::vector<std::string> argument_types_;
};

//------------------------------------------------------------
class AuthChallengeResponse : public CQLResponse {
 public:
  AuthChallengeResponse(const CQLRequest& request, const std::string& token);
  virtual ~AuthChallengeResponse() override;

 protected:
  virtual void SerializeBody(faststring* mesg) const override;

 private:

  const std::string token_;
};

//------------------------------------------------------------
class AuthSuccessResponse : public CQLResponse {
 public:
  AuthSuccessResponse(const CQLRequest& request, const std::string& token);
  virtual ~AuthSuccessResponse() override;

 protected:
  virtual void SerializeBody(faststring* mesg) const override;

 private:
  const std::string token_;
};

//------------------------------------------------------------
class CQLServerEvent : public rpc::ServerEvent {
 public:
  explicit CQLServerEvent(std::unique_ptr<EventResponse> event_response);
  void Serialize(boost::container::small_vector_base<RefCntBuffer>* output) const override;
  std::string ToString() const override;
  size_t ObjectSize() const { return sizeof(*this); }
  size_t DynamicMemoryUsage() const {
    return DynamicMemoryUsageOf(event_response_) + DynamicMemoryUsageOf(serialized_response_);
  }

 private:

  std::unique_ptr<EventResponse> event_response_;
  // Need to keep the serialized response around since we return a reference to it via Slice in
  // Serialize().
  RefCntBuffer serialized_response_;
};

//------------------------------------------------------------
class CQLServerEventList : public rpc::ServerEventList {
 public:
  CQLServerEventList();
  void AddEvent(std::unique_ptr<CQLServerEvent> event);
  void Serialize(boost::container::small_vector_base<RefCntBuffer>* output) override;
  std::string ToString() const override;

  size_t ObjectSize() const override { return sizeof(CQLServerEventList); }

  size_t DynamicMemoryUsage() const override { return DynamicMemoryUsageOf(cql_server_events_); }

 private:
  void Transferred(const Status& status, rpc::Connection*) override;
  std::vector<std::unique_ptr<CQLServerEvent>> cql_server_events_;
};

}  // namespace cqlserver
}  // namespace yb

#endif // YB_YQL_CQL_CQLSERVER_CQL_MESSAGE_H_
