//
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
// This module defines the schema that will be used when creating tables.
//
// Note on primary key definitions.
// - There are two different APIs to define primary key. They cannot be used together but can be
//   used interchangeably for the same purpose (This is different from Kudu's original design which
//   uses one API for single-column key and another for multi-column key).
// - First API:
//   Each column of a primary key can be specified as hash or regular primary key.
//   Function PrimaryKey()
//   Function HashPrimaryKey().
// - Second API:
//   All hash and regular primary columns can be specified together in a list.
//   Function YBSchemaBuilder::SetPrimaryKey().
#ifndef YB_CLIENT_SCHEMA_H
#define YB_CLIENT_SCHEMA_H

#include <string>
#include <vector>

#include "yb/client/client_fwd.h"
#include "yb/client/value.h"
#include "yb/common/schema.h"

#include "yb/util/status.h"

namespace yb {

// the types used internally and sent over the wire to the tserver
typedef QLValuePB::ValueCase InternalType;

class ColumnSchema;
class YBPartialRow;
class Schema;
class TableProperties;

namespace tools {
class TsAdminClient;
}

namespace client {

namespace internal {
class GetTableSchemaRpc;
class LookupRpc;
class WriteRpc;

const Schema& GetSchema(const YBSchema& schema);
Schema& GetSchema(YBSchema* schema);

} // namespace internal

class YBClient;
class YBSchema;
class YBSchemaBuilder;
class YBOperation;

class YBColumnSchema {
 public:
  static InternalType ToInternalDataType(const std::shared_ptr<QLType>& ql_type) {
    switch (ql_type->main()) {
      case INT8:
        return InternalType::kInt8Value;
      case INT16:
        return InternalType::kInt16Value;
      case INT32:
        return InternalType::kInt32Value;
      case INT64:
        return InternalType::kInt64Value;
      case UINT32:
        return InternalType::kUint32Value;
      case UINT64:
        return InternalType::kUint64Value;
      case FLOAT:
        return InternalType::kFloatValue;
      case DOUBLE:
        return InternalType::kDoubleValue;
      case DECIMAL:
        return InternalType::kDecimalValue;
      case STRING:
        return InternalType::kStringValue;
      case TIMESTAMP:
        return InternalType::kTimestampValue;
      case DATE:
        return InternalType::kDateValue;
      case TIME:
        return InternalType::kTimeValue;
      case INET:
        return InternalType::kInetaddressValue;
      case JSONB:
        return InternalType::kJsonbValue;
      case UUID:
        return InternalType::kUuidValue;
      case TIMEUUID:
        return InternalType::kTimeuuidValue;
      case BOOL:
        return InternalType::kBoolValue;
      case BINARY:
        return InternalType::kBinaryValue;
      case USER_DEFINED_TYPE: FALLTHROUGH_INTENDED;
      case MAP:
        return InternalType::kMapValue;
      case SET:
        return InternalType::kSetValue;
      case LIST:
        return InternalType::kListValue;
      case VARINT:
        return InternalType::kVarintValue;
      case FROZEN:
        return InternalType::kFrozenValue;

      case TUPLE: FALLTHROUGH_INTENDED; // TODO (mihnea) Tuple type not fully supported yet
      case NULL_VALUE_TYPE: FALLTHROUGH_INTENDED;
      case UNKNOWN_DATA:
        return InternalType::VALUE_NOT_SET;

      case TYPEARGS: FALLTHROUGH_INTENDED;
      case UINT8: FALLTHROUGH_INTENDED;
      case UINT16:
        break;
    }
    LOG(FATAL) << "Internal error: unsupported type " << ql_type->ToString();
    return InternalType::VALUE_NOT_SET;
  }

  static std::string DataTypeToString(DataType type);

  // DEPRECATED: use YBSchemaBuilder instead.
  // TODO(KUDU-809): make this hard-to-use constructor private. Clients should use
  // the Builder API. Currently only the Python API uses this old API.
  YBColumnSchema(const std::string &name,
                 const std::shared_ptr<QLType>& type,
                 bool is_nullable = false,
                 bool is_hash_key = false,
                 bool is_static = false,
                 bool is_counter = false,
                 int32_t order = 0,
                 ColumnSchema::SortingType sorting_type = ColumnSchema::SortingType::kNotSpecified);
  YBColumnSchema(const YBColumnSchema& other);
  ~YBColumnSchema();

  YBColumnSchema& operator=(const YBColumnSchema& other);

  void CopyFrom(const YBColumnSchema& other);

  bool Equals(const YBColumnSchema& other) const;

  // Getters to expose column schema information.
  const std::string& name() const;
  const std::shared_ptr<QLType>& type() const;
  bool is_hash_key() const;
  bool is_nullable() const;
  bool is_static() const;
  bool is_counter() const;
  int32_t order() const;
  ColumnSchema::SortingType sorting_type() const;

 private:
  friend class YBColumnSpec;
  friend class YBSchema;
  friend class YBSchemaBuilder;
  // YBTableAlterer::Data needs to be a friend. Friending the parent class
  // is transitive to nested classes. See http://tiny.cloudera.com/jwtui
  friend class YBTableAlterer;

  YBColumnSchema();

  // Owned.
  ColumnSchema* col_;
};

// Builder API for specifying or altering a column within a table schema.
// This cannot be constructed directly, but rather is returned from
// YBSchemaBuilder::AddColumn() to specify a column within a Schema.
//
// TODO(KUDU-861): this API will also be used for an improved AlterTable API.
class YBColumnSpec {
 public:
  explicit YBColumnSpec(const std::string& col_name);

  ~YBColumnSpec();

  // Operations only relevant for Create Table
  // ------------------------------------------------------------

  // Set this column to be the primary key of the table.
  //
  // This may only be used to set non-composite primary keys. If a composite
  // key is desired, use YBSchemaBuilder::SetPrimaryKey(). This may not be
  // used in conjunction with YBSchemaBuilder::SetPrimaryKey().
  //
  // Only relevant for a CreateTable operation. Primary keys may not be changed
  // after a table is created.
  YBColumnSpec* PrimaryKey();

  // Set this column to be a hash primary key column of the table. A hash value of all hash columns
  // in the primary key will be used to determine what partition (tablet) a particular row falls in.
  YBColumnSpec* HashPrimaryKey();

  // Set this column to be static. A static column is a column whose value is shared among rows of
  // the same hash key.
  YBColumnSpec* StaticColumn();

  // Set this column to be not nullable.
  // Column nullability may not be changed once a table is created.
  YBColumnSpec* NotNull();

  // Set this column to be nullable (the default).
  // Column nullability may not be changed once a table is created.
  YBColumnSpec* Nullable();

  // Set the type of this column.
  // Column types may not be changed once a table is created.
  YBColumnSpec* Type(const std::shared_ptr<QLType>& type);

  // Convenience function for setting a simple (i.e. non-parametric) data type.
  YBColumnSpec* Type(DataType type) {
    return Type(QLType::Create(type));
  }

  // Specify the user-defined order of the column.
  YBColumnSpec* Order(int32_t order);

  // Specify the user-defined sorting direction.
  YBColumnSpec* SetSortingType(ColumnSchema::SortingType sorting_type);

  // Identify this column as counter.
  YBColumnSpec* Counter();

  // Add JSON operation.
  YBColumnSpec* JsonOp(JsonOperatorPB op, const std::string& str_value);
  YBColumnSpec* JsonOp(JsonOperatorPB op, int32_t int_value);

  // Operations only relevant for Alter Table
  // ------------------------------------------------------------

  // Rename this column.
  YBColumnSpec* RenameTo(const std::string& new_name);

 private:
  class Data;
  friend class YBSchemaBuilder;
  friend class YBTableAlterer;

  CHECKED_STATUS ToColumnSchema(YBColumnSchema* col) const;

  YBColumnSpec* JsonOp(JsonOperatorPB op, const QLValuePB& value);

  // Owned.
  Data* data_;
};

// Builder API for constructing a YBSchema object.
// The API here is a "fluent" style of programming, such that the resulting code
// looks somewhat like a SQL "CREATE TABLE" statement. For example:
//
// SQL:
//   CREATE TABLE t (
//     my_key int not null primary key,
//     a float
//   );
//
// is represented as:
//
//   YBSchemaBuilder t;
//   t.AddColumn("my_key")->Type(YBColumnSchema::INT32)->NotNull()->PrimaryKey();
//   t.AddColumn("a")->Type(YBColumnSchema::FLOAT);
//   YBSchema schema;
//   t.Build(&schema);
class YBSchemaBuilder {
 public:
  YBSchemaBuilder();
  ~YBSchemaBuilder();

  // Return a YBColumnSpec for a new column within the Schema.
  // The returned object is owned by the YBSchemaBuilder.
  YBColumnSpec* AddColumn(const std::string& name);

  // Set the primary key of the new Schema based on the given column names. The first
  // 'key_hash_col_count' columns in the primary are hash columns whose values will be used for
  // table partitioning. This may be used to specify a compound primary key.
  YBSchemaBuilder* SetPrimaryKey(const std::vector<std::string>& key_col_names,
                                 int key_hash_col_count = 0);

  YBSchemaBuilder* SetTableProperties(const TableProperties& table_properties);

  // Resets 'schema' to the result of this builder.
  //
  // If the Schema is invalid for any reason (eg missing types, duplicate column names, etc)
  // a bad Status will be returned.
  CHECKED_STATUS Build(YBSchema* schema);

 private:
  class Data;
  // Owned.
  Data* data_;
};

class YBSchema {
 public:
  YBSchema();

  explicit YBSchema(const Schema& schema);

  YBSchema(const YBSchema& other);
  YBSchema(YBSchema&& other);
  ~YBSchema();

  YBSchema& operator=(const YBSchema& other);
  YBSchema& operator=(YBSchema&& other);
  void CopyFrom(const YBSchema& other);
  void MoveFrom(YBSchema&& other);

  // DEPRECATED: will be removed soon.
  CHECKED_STATUS Reset(const std::vector<YBColumnSchema>& columns, int key_columns,
                       const TableProperties& table_properties) WARN_UNUSED_RESULT;

  void Reset(std::unique_ptr<Schema> schema);

  bool Equals(const YBSchema& other) const;

  bool EquivalentForDataCopy(const YBSchema& other) const;

  Result<bool> Equals(const SchemaPB& pb_schema) const;

  // Two schemas are equivalent if it's possible to copy data from one table to the
  // other containing these two schemas.
  // For example, columns and columns types are the same, but table properties
  // might be different in areas that are not relevant (e.g. TTL).
  Result<bool> EquivalentForDataCopy(const SchemaPB& pb_schema) const;

  const TableProperties& table_properties() const;

  YBColumnSchema Column(size_t idx) const;
  YBColumnSchema ColumnById(int32_t id) const;

  // Returns column id provided its index.
  int32_t ColumnId(size_t idx) const;

  // Returns the number of columns in hash primary keys.
  size_t num_hash_key_columns() const;

  // Number of range key columns.
  size_t num_range_key_columns() const;

  // Returns the number of columns in primary keys.
  size_t num_key_columns() const;

  // Returns the total number of columns.
  size_t num_columns() const;

  uint32_t version() const;
  void set_version(uint32_t version);

  // Get the indexes of the primary key columns within this Schema.
  // In current versions of YB, these will always be contiguous column
  // indexes starting with 0. However, in future versions this assumption
  // may not hold, so callers should not assume it is the case.
  void GetPrimaryKeyColumnIndexes(std::vector<int>* indexes) const;

  // Create a new row corresponding to this schema.
  //
  // The new row refers to this YBSchema object, so must be destroyed before
  // the YBSchema object.
  //
  // The caller takes ownership of the created row.
  YBPartialRow* NewRow() const;

  const std::vector<ColumnSchema>& columns() const;

  int FindColumn(const GStringPiece& name) const {
    return schema_->find_column(name);
  }

  string ToString() const;

 private:
  friend YBSchema YBSchemaFromSchema(const Schema& schema);
  friend const Schema& internal::GetSchema(const YBSchema& schema);
  friend Schema& internal::GetSchema(YBSchema* schema);

  std::unique_ptr<Schema> schema_;
  uint32_t version_;
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_SCHEMA_H
