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
#ifndef YB_COMMON_SCHEMA_H
#define YB_COMMON_SCHEMA_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "yb/common/ql_type.h"
#include "yb/common/id_mapping.h"
#include "yb/common/key_encoder.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/transaction.h"
#include "yb/common/common.pb.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/strcat.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/enums.h"
#include "yb/util/status.h"

// Check that two schemas are equal, yielding a useful error message in the case that
// they are not.
#define DCHECK_SCHEMA_EQ(s1, s2) \
  do { \
    DCHECK((s1).Equals((s2))) << "Schema " << (s1).ToString() \
                              << " does not match " << (s2).ToString(); \
  } while (0);

#define DCHECK_KEY_PROJECTION_SCHEMA_EQ(s1, s2) \
  do { \
    DCHECK((s1).KeyEquals((s2))) << "Key-Projection Schema " \
                                 << (s1).ToString() << " does not match " \
                                 << (s2).ToString(); \
  } while (0);

namespace yb {

class DeletedColumnPB;

using std::vector;
using std::unordered_map;
using std::unordered_set;

template<char digit1, char... digits>
struct ColumnIdHelper {
  typedef ColumnIdHelper<digit1> Current;
  typedef ColumnIdHelper<digits...> Next;
  static constexpr int mul = Next::mul * 10;
  static constexpr int value = Current::value * mul + Next::value;
  static_assert(value <= std::numeric_limits<int32_t>::max(), "Too big constant");
};

template<char digit>
struct ColumnIdHelper<digit> {
  static_assert(digit >= '0' && digit <= '9', "Only digits is allowed");
  static constexpr int value = digit - '0';
  static constexpr int mul = 1;
};

// The ID of a column. Each column in a table has a unique ID.
typedef int32_t ColumnIdRep;
struct ColumnId {
  explicit ColumnId(ColumnIdRep t_) : t(t_) {
    DCHECK_GE(t_, 0);
  }
  template<char... digits>
  constexpr explicit ColumnId(ColumnIdHelper<digits...>) : t(ColumnIdHelper<digits...>::value) {}

  ColumnId() : t() {}
  constexpr ColumnId(const ColumnId& t_) : t(t_.t) {}
  ColumnId& operator=(const ColumnId& rhs) { t = rhs.t; return *this; }
  ColumnId& operator=(const ColumnIdRep& rhs) { DCHECK_GE(rhs, 0); t = rhs; return *this; }
  operator const ColumnIdRep() const { return t; }
  operator const strings::internal::SubstituteArg() const { return t; }
  operator const AlphaNum() const { return t; }
  ColumnIdRep rep() const { return t; }

  bool operator==(const ColumnId& rhs) const { return t == rhs.t; }
  bool operator!=(const ColumnId& rhs) const { return t != rhs.t; }
  bool operator<(const ColumnId& rhs) const { return t < rhs.t; }
  bool operator>(const ColumnId& rhs) const { return t > rhs.t; }

  friend std::ostream& operator<<(std::ostream& os, ColumnId column_id) {
    return os << column_id.t;
  }

  std::string ToString() const {
    return std::to_string(t);
  }

  uint64_t ToUint64() const {
    DCHECK_GE(t, 0);
    return static_cast<uint64_t>(t);
  }

  static CHECKED_STATUS FromInt64(int64_t value, ColumnId *column_id) {
    if (value > std::numeric_limits<ColumnIdRep>::max() || value < 0) {
      return STATUS(Corruption, strings::Substitute("$0 not valid for column id representation",
                                                    value));
    }
    column_id->t = static_cast<ColumnIdRep>(value);
    return Status::OK();
  }

 private:
  ColumnIdRep t;
};
static const ColumnId kInvalidColumnId = ColumnId(std::numeric_limits<ColumnIdRep>::max());
// In a new schema, we typically would start assigning column IDs at 0. However, this
// makes it likely that in many test cases, the column IDs and the column indexes are
// equal to each other, and it's easy to accidentally pass an index where we meant to pass
// an ID, without having any issues. So, in DEBUG builds, we start assigning columns at ID
// 10, ensuring that if we accidentally mix up IDs and indexes, we're likely to fire an
// assertion or bad memory access.
#ifdef NDEBUG
constexpr ColumnIdRep kFirstColumnIdRep = 0;
#else
constexpr ColumnIdRep kFirstColumnIdRep = 10;
#endif
const ColumnId kFirstColumnId(kFirstColumnIdRep);

template<char... digits>
ColumnId operator"" _ColId() {
  return ColumnId(ColumnIdHelper<digits...>());
}

// Struct for storing information about deleted columns for cleanup.
struct DeletedColumn {
  ColumnId id;
  HybridTime ht;

  DeletedColumn() { }

  DeletedColumn(ColumnId id, const HybridTime& ht) : id(id), ht(ht) { }

  static CHECKED_STATUS FromPB(const DeletedColumnPB& col, DeletedColumn* ret);
  void CopyToPB(DeletedColumnPB* pb) const;
};

typedef std::unordered_set<ColumnId> ColumnIds;
typedef std::shared_ptr<ColumnIds> ColumnIdsPtr;

// The schema for a given column.
//
// Holds the data type as well as information about nullability & column name.
// In the future, it may hold information about annotations, etc.
class ColumnSchema {
 public:
  // Component comparators for combining in custom comparators.
  static bool CompName(const ColumnSchema &a, const ColumnSchema &b) {
    return a.name_ == b.name_;
  }

  static bool CompNullable(const ColumnSchema &a, const ColumnSchema &b) {
    return a.is_nullable_ == b.is_nullable_;
  }

  static bool CompHashKey(const ColumnSchema &a, const ColumnSchema &b) {
    return a.is_hash_key_ == b.is_hash_key_;
  }

  static bool CompSortingType(const ColumnSchema &a, const ColumnSchema &b) {
    return a.sorting_type_ == b.sorting_type_;
  }

  static bool CompTypeInfo(const ColumnSchema &a, const ColumnSchema &b) {
    return a.type_info()->type() == b.type_info()->type();
  }

  static bool CompOrder(const ColumnSchema &a, const ColumnSchema &b) {
    return a.order_ == b.order_;
  }

  // Combined comparators.
  static bool CompareType(const ColumnSchema &a, const ColumnSchema &b) {
    return CompNullable(a, b) && CompHashKey(a, b) &&
        CompSortingType(a, b) && CompTypeInfo(a, b);
  }

  static bool CompareByDefault(const ColumnSchema &a, const ColumnSchema &b) {
    return CompareType(a, b) && CompName(a, b);
  }

  enum SortingType : uint8_t {
    kNotSpecified = 0,
    kAscending,          // ASC, NULLS FIRST
    kDescending,         // DESC, NULLS FIRST
    kAscendingNullsLast, // ASC, NULLS LAST
    kDescendingNullsLast // DESC, NULLS LAST
  };

  // name: column name
  // type: column type (e.g. UINT8, INT32, STRING, MAP<INT32, STRING> ...)
  // is_nullable: true if a row value can be null
  // is_hash_key: true if a column's hash value can be used for partitioning.
  //
  // Example:
  //   ColumnSchema col_a("a", UINT32)
  //   ColumnSchema col_b("b", STRING, true);
  //   uint32_t default_i32 = -15;
  //   ColumnSchema col_c("c", INT32, false, &default_i32);
  //   Slice default_str("Hello");
  //   ColumnSchema col_d("d", STRING, false, &default_str);
  ColumnSchema(string name,
               const std::shared_ptr<QLType>& type,
               bool is_nullable = false,
               bool is_hash_key = false,
               bool is_static = false,
               bool is_counter = false,
               int32_t order = 0,
               SortingType sorting_type = SortingType::kNotSpecified)
      : name_(std::move(name)),
        type_(type),
        is_nullable_(is_nullable),
        is_hash_key_(is_hash_key),
        is_static_(is_static),
        is_counter_(is_counter),
        order_(order),
        sorting_type_(sorting_type) {
  }

  // convenience constructor for creating columns with simple (non-parametric) data types
  ColumnSchema(string name,
               DataType type,
               bool is_nullable = false,
               bool is_hash_key = false,
               bool is_static = false,
               bool is_counter = false,
               int32_t order = 0,
               SortingType sorting_type = SortingType::kNotSpecified)
      : ColumnSchema(name, QLType::Create(type), is_nullable, is_hash_key, is_static, is_counter,
                     order, sorting_type) {
  }

  const std::shared_ptr<QLType>& type() const {
    return type_;
  }

  void set_type(const std::shared_ptr<QLType>& type) {
    type_ = type;
  }

  const TypeInfo* type_info() const {
    return type_->type_info();
  }

  bool is_nullable() const {
    return is_nullable_;
  }

  bool is_hash_key() const {
    return is_hash_key_;
  }

  bool is_static() const {
    return is_static_;
  }

  bool is_counter() const {
    return is_counter_;
  }

  int32_t order() const {
    return order_;
  }

  SortingType sorting_type() const {
    return sorting_type_;
  }

  void set_sorting_type(SortingType sorting_type) {
    sorting_type_ = sorting_type;
  }

  const std::string sorting_type_string() const {
    switch (sorting_type_) {
      case kNotSpecified:
        return "none";
      case kAscending:
        return "asc";
      case kDescending:
        return "desc";
      case kAscendingNullsLast:
        return "asc nulls last";
      case kDescendingNullsLast:
        return "desc nulls last";
    }
    LOG (FATAL) << "Invalid sorting type: " << sorting_type_;
  }

  const std::string &name() const {
    return name_;
  }

  // Return a string identifying this column, including its
  // name.
  std::string ToString() const;

  // Same as above, but only including the type information.
  // For example, "STRING NOT NULL".
  std::string TypeToString() const;

  template <typename Comparator>
  bool Equals(const ColumnSchema &other, Comparator comp) const {
    return comp(*this, other);
  }

  bool EqualsType(const ColumnSchema &other) const {
    return Equals(other, CompareType);
  }

  bool Equals(const ColumnSchema &other) const {
    return Equals(other, CompareByDefault);
  }

  int Compare(const void *lhs, const void *rhs) const {
    return type_info()->Compare(lhs, rhs);
  }

  // Stringify the given cell. This just stringifies the cell contents,
  // and doesn't include the column name or type.
  std::string Stringify(const void *cell) const {
    std::string ret;
    type_info()->AppendDebugStringForValue(cell, &ret);
    return ret;
  }

  // Append a debug string for this cell. This differs from Stringify above
  // in that it also includes the column info, for example 'STRING foo=bar'.
  template<class CellType>
  void DebugCellAppend(const CellType& cell, std::string* ret) const {
    ret->append(type_info()->name());
    ret->append(" ");
    ret->append(name_);
    ret->append("=");
    if (is_nullable_ && cell.is_null()) {
      ret->append("NULL");
    } else {
      type_info()->AppendDebugStringForValue(cell.ptr(), ret);
    }
  }

  // Returns the memory usage of this object without the object itself. Should
  // be used when embedded inside another object.
  size_t memory_footprint_excluding_this() const;

  // Returns the memory usage of this object including the object itself.
  // Should be used when allocated on the heap.
  size_t memory_footprint_including_this() const;

 private:
  friend class SchemaBuilder;

  void set_name(const std::string& name) {
    name_ = name;
  }

  std::string name_;
  std::shared_ptr<QLType> type_;
  bool is_nullable_;
  bool is_hash_key_;
  bool is_static_;
  bool is_counter_;
  int32_t order_;
  SortingType sorting_type_;
};

class ContiguousRow;
const TableId kNoCopartitionTableId = "";

class TableProperties {
 public:
  // Containing counters is a internal property instead of a user-defined property, so we don't use
  // it when comparing table properties.
  bool operator==(const TableProperties& other) const {
    if (!Equivalent(other)) {
      return false;
    }

    return default_time_to_live_ == other.default_time_to_live_ &&
           use_mangled_column_name_ == other.use_mangled_column_name_ &&
           contain_counters_ == other.contain_counters_;

    // Ignoring num_tablets_.
    // Ignoring retain_delete_markers_.
    // Ignoring wal_retention_secs_.
  }

  bool operator!=(const TableProperties& other) const {
    return !(*this == other);
  }

  bool Equivalent(const TableProperties& other) const {
    if (is_ysql_catalog_table_ != other.is_ysql_catalog_table_) {
      return false;
    }

    if (is_transactional_ != other.is_transactional_) {
      return false;
    }

    if (consistency_level_ != other.consistency_level_) {
      return false;
    }

    if ((copartition_table_id_ == kNoCopartitionTableId ||
         other.copartition_table_id_ == kNoCopartitionTableId) &&
        copartition_table_id_ != other.copartition_table_id_) {
      return false;
    }

    // Ignoring default_time_to_live_.
    // Ignoring num_tablets_.
    // Ignoring use_mangled_column_name_.
    // Ignoring contain_counters_.
    // Ignoring retain_delete_markers_.
    // Ignoring wal_retention_secs_.
    return true;
  }

  bool HasDefaultTimeToLive() const {
    return (default_time_to_live_ != kNoDefaultTtl);
  }

  void SetDefaultTimeToLive(uint64_t default_time_to_live) {
    default_time_to_live_ = default_time_to_live;
  }

  int64_t DefaultTimeToLive() const {
    return default_time_to_live_;
  }

  bool contain_counters() const {
    return contain_counters_;
  }

  bool is_transactional() const {
    return is_transactional_;
  }

  YBConsistencyLevel consistency_level() const {
    return consistency_level_;
  }

  void SetContainCounters(bool contain_counters) {
    contain_counters_ = contain_counters;
  }

  void SetTransactional(bool is_transactional) {
    is_transactional_ = is_transactional;
  }

  void SetConsistencyLevel(YBConsistencyLevel consistency_level) {
    consistency_level_ = consistency_level;
  }

  TableId CopartitionTableId() const {
    return copartition_table_id_;
  }

  bool HasCopartitionTableId() const {
    return copartition_table_id_ != kNoCopartitionTableId;
  }

  void SetCopartitionTableId(const TableId& copartition_table_id) {
    copartition_table_id_ = copartition_table_id;
  }

  void SetUseMangledColumnName(bool value) {
    use_mangled_column_name_ = value;
  }

  bool use_mangled_column_name() const {
    return use_mangled_column_name_;
  }

  void SetNumTablets(int num_tablets) {
    num_tablets_ = num_tablets;
  }

  bool HasNumTablets() const {
    return num_tablets_ > 0;
  }

  int num_tablets() const {
    return num_tablets_;
  }

  void set_is_ysql_catalog_table(bool is_ysql_catalog_table) {
    is_ysql_catalog_table_ = is_ysql_catalog_table;
  }

  bool is_ysql_catalog_table() const {
    return is_ysql_catalog_table_;
  }

  bool retain_delete_markers() const {
    return retain_delete_markers_;
  }

  void SetRetainDeleteMarkers(bool retain_delete_markers) {
    retain_delete_markers_ = retain_delete_markers;
  }

  void ToTablePropertiesPB(TablePropertiesPB *pb) const;

  static TableProperties FromTablePropertiesPB(const TablePropertiesPB& pb);

  void AlterFromTablePropertiesPB(const TablePropertiesPB& pb);

  void Reset();

  std::string ToString() const;

 private:
  // IMPORTANT: Every time a new property is added, we need to revisit
  // operator== and Equivalent methods to make sure that the new property
  // is being taken into consideration when deciding whether properties between
  // two different tables are equal or equivalent.
  static const int kNoDefaultTtl = -1;
  int64_t default_time_to_live_ = kNoDefaultTtl;
  bool contain_counters_ = false;
  bool is_transactional_ = false;
  bool retain_delete_markers_ = false;
  YBConsistencyLevel consistency_level_ = YBConsistencyLevel::STRONG;
  TableId copartition_table_id_ = kNoCopartitionTableId;
  boost::optional<uint32_t> wal_retention_secs_;
  bool use_mangled_column_name_ = false;
  int num_tablets_ = 0;
  bool is_ysql_catalog_table_ = false;
};

typedef uint32_t PgTableOid;

// The schema for a set of rows.
//
// A Schema is simply a set of columns, along with information about
// which prefix of columns makes up the primary key.
//
// Note that, while Schema is copyable and assignable, it is a complex
// object that is not inexpensive to copy. You should generally prefer
// passing by pointer or reference, and functions that create new
// Schemas should generally prefer taking a Schema pointer and using
// Schema::swap() or Schema::Reset() rather than returning by value.
class Schema {
 public:

  static const int kColumnNotFound = -1;

  Schema()
    : num_key_columns_(0),
      num_hash_key_columns_(0),
      name_to_index_bytes_(0),
      // TODO: C++11 provides a single-arg constructor
      name_to_index_(10,
                     NameToIndexMap::hasher(),
                     NameToIndexMap::key_equal(),
                     NameToIndexMapAllocator(&name_to_index_bytes_)),
      has_nullables_(false),
      cotable_id_(boost::uuids::nil_uuid()),
      pgtable_id_(0) {
  }

  Schema(const Schema& other);
  Schema& operator=(const Schema& other);

  void swap(Schema& other); // NOLINT(build/include_what_you_use)

  void CopyFrom(const Schema& other);

  // Construct a schema with the given information.
  //
  // NOTE: if the schema is user-provided, it's better to construct an
  // empty schema and then use Reset(...)  so that errors can be
  // caught. If an invalid schema is passed to this constructor, an
  // assertion will be fired!
  Schema(const vector<ColumnSchema>& cols,
         int key_columns,
         const TableProperties& table_properties = TableProperties(),
         const Uuid& cotable_id = Uuid(boost::uuids::nil_uuid()),
         const PgTableOid pgtable_id = 0)
    : name_to_index_bytes_(0),
      // TODO: C++11 provides a single-arg constructor
      name_to_index_(10,
                     NameToIndexMap::hasher(),
                     NameToIndexMap::key_equal(),
                     NameToIndexMapAllocator(&name_to_index_bytes_)) {
    CHECK_OK(Reset(cols, key_columns, table_properties, cotable_id, pgtable_id));
  }

  // Construct a schema with the given information.
  //
  // NOTE: if the schema is user-provided, it's better to construct an
  // empty schema and then use Reset(...)  so that errors can be
  // caught. If an invalid schema is passed to this constructor, an
  // assertion will be fired!
  Schema(const vector<ColumnSchema>& cols,
         const vector<ColumnId>& ids,
         int key_columns,
         const TableProperties& table_properties = TableProperties(),
         const Uuid& cotable_id = Uuid(boost::uuids::nil_uuid()),
         const PgTableOid pgtable_id = 0)
    : name_to_index_bytes_(0),
      // TODO: C++11 provides a single-arg constructor
      name_to_index_(10,
                     NameToIndexMap::hasher(),
                     NameToIndexMap::key_equal(),
                     NameToIndexMapAllocator(&name_to_index_bytes_)) {
    CHECK_OK(Reset(cols, ids, key_columns, table_properties, cotable_id, pgtable_id));
  }

  // Reset this Schema object to the given schema.
  // If this fails, the Schema object is left in an inconsistent
  // state and may not be used.
  CHECKED_STATUS Reset(const vector<ColumnSchema>& cols, int key_columns,
                       const TableProperties& table_properties = TableProperties(),
                       const Uuid& cotable_id = Uuid(boost::uuids::nil_uuid()),
                       const PgTableOid pgtable_id = 0) {
    std::vector<ColumnId> ids;
    return Reset(cols, ids, key_columns, table_properties, cotable_id, pgtable_id);
  }

  // Reset this Schema object to the given schema.
  // If this fails, the Schema object is left in an inconsistent
  // state and may not be used.
  CHECKED_STATUS Reset(const vector<ColumnSchema>& cols,
                       const vector<ColumnId>& ids,
                       int key_columns,
                       const TableProperties& table_properties = TableProperties(),
                       const Uuid& cotable_id = Uuid(boost::uuids::nil_uuid()),
                       const PgTableOid pgtable_id = 0);

  // Return the number of bytes needed to represent a single row of this schema.
  //
  // This size does not include any indirected (variable length) data (eg strings)
  size_t byte_size() const {
    DCHECK(initialized());
    return col_offsets_.back();
  }

  // Return the number of bytes needed to represent
  // only the key portion of this schema.
  size_t key_byte_size() const {
    return col_offsets_[num_key_columns_];
  }

  // Return the number of columns in this schema
  size_t num_columns() const {
    return cols_.size();
  }

  // Return the length of the key prefix in this schema.
  size_t num_key_columns() const {
    return num_key_columns_;
  }

  // Number of hash key columns.
  size_t num_hash_key_columns() const {
    return num_hash_key_columns_;
  }

  // Number of range key columns.
  size_t num_range_key_columns() const {
    return num_key_columns_ - num_hash_key_columns_;
  }

  // Return the byte offset within the row for the given column index.
  size_t column_offset(size_t col_idx) const {
    DCHECK_LT(col_idx, cols_.size());
    return col_offsets_[col_idx];
  }

  // Return the ColumnSchema corresponding to the given column index.
  inline const ColumnSchema &column(size_t idx) const {
    DCHECK_LT(idx, cols_.size());
    return cols_[idx];
  }

  // Return the ColumnSchema corresponding to the given column ID.
  inline Result<const ColumnSchema&> column_by_id(ColumnId id) const {
    int idx = find_column_by_id(id);
    if (idx < 0) {
      return STATUS_FORMAT(InvalidArgument, "Column id $0 not found", id.ToString());
    }
    return cols_[idx];
  }

  // Return the column ID corresponding to the given column index
  ColumnId column_id(size_t idx) const {
    DCHECK(has_column_ids());
    DCHECK_LT(idx, cols_.size());
    return col_ids_[idx];
  }

  // Return true if the schema contains an ID mapping for its columns.
  // In the case of an empty schema, this is false.
  bool has_column_ids() const {
    return !col_ids_.empty();
  }

  const std::vector<ColumnSchema>& columns() const {
    return cols_;
  }

  const std::vector<ColumnId>& column_ids() const {
    return col_ids_;
  }

  const std::vector<string> column_names() const {
    vector<string> column_names;
    for (const auto& col : cols_) {
      column_names.push_back(col.name());
    }
    return column_names;
  }

  const TableProperties& table_properties() const {
    return table_properties_;
  }

  TableProperties* mutable_table_properties() {
    return &table_properties_;
  }

  void SetDefaultTimeToLive(const uint64_t& ttl_msec) {
    table_properties_.SetDefaultTimeToLive(ttl_msec);
  }

  void SetCopartitionTableId(const TableId& copartition_table_id) {
    table_properties_.SetCopartitionTableId(copartition_table_id);
  }

  void SetTransactional(bool is_transactional) {
    table_properties_.SetTransactional(is_transactional);
  }

  void SetRetainDeleteMarkers(bool retain_delete_markers) {
    table_properties_.SetRetainDeleteMarkers(retain_delete_markers);
  }

  // Return the column index corresponding to the given column,
  // or kColumnNotFound if the column is not in this schema.
  int find_column(const GStringPiece col_name) const {
    auto iter = name_to_index_.find(col_name);
    if (PREDICT_FALSE(iter == name_to_index_.end())) {
      return kColumnNotFound;
    } else {
      return (*iter).second;
    }
  }

  Result<ColumnId> ColumnIdByName(const std::string& name) const;

  Result<int> ColumnIndexByName(GStringPiece col_name) const;

  // Returns true if the schema contains nullable columns
  bool has_nullables() const {
    return has_nullables_;
  }

  // Returns true if the schema contains static columns
  bool has_statics() const {
    return has_statics_;
  }

  // Returns true if the specified column (by index) is a key
  bool is_key_column(size_t idx) const {
    return idx < num_key_columns_;
  }

  // Returns true if the specified column (by column id) is a key
  bool is_key_column(ColumnId column_id) const {
    return is_key_column(find_column_by_id(column_id));
  }

  // Returns true if the specified column (by name) is a key
  bool is_key_column(const GStringPiece col_name) const {
    return is_key_column(find_column(col_name));
  }

  // Returns true if the specified column (by index) is a hash key
  bool is_hash_key_column(size_t idx) const {
    return idx < num_hash_key_columns_;
  }

  // Returns true if the specified column (by column id) is a hash key
  bool is_hash_key_column(ColumnId column_id) const {
    return is_hash_key_column(find_column_by_id(column_id));
  }

  // Returns true if the specified column (by name) is a hash key
  bool is_hash_key_column(const GStringPiece col_name) const {
    return is_hash_key_column(find_column(col_name));
  }

  // Returns true if the specified column (by index) is a range column
  bool is_range_column(size_t idx) const {
    return is_key_column(idx) && !is_hash_key_column(idx);
  }

  // Returns true if the specified column (by column id) is a range column
  bool is_range_column(ColumnId column_id) const {
    return is_range_column(find_column_by_id(column_id));
  }

  // Returns true if the specified column (by name) is a range column
  bool is_range_column(const GStringPiece col_name) const {
    return is_range_column(find_column(col_name));
  }

  // Return true if this Schema is initialized and valid.
  bool initialized() const {
    return !col_offsets_.empty();
  }

  // Returns the highest column id in this Schema.
  ColumnId max_col_id() const {
    return max_col_id_;
  }

  // Gets and sets the uuid of the non-primary table this schema belongs to co-located in a tablet.
  const Uuid& cotable_id() const {
    return cotable_id_;
  }

  bool has_cotable_id() const {
    return !cotable_id_.IsNil();
  }

  void set_cotable_id(const Uuid& cotable_id) {
    if (!cotable_id.IsNil()) {
      DCHECK_EQ(pgtable_id_, 0);
    }
    cotable_id_ = cotable_id;
  }

  // Gets and sets the PG table OID of the non-primary table this schema belongs to in a tablet
  // with colocated tables.
  const PgTableOid pgtable_id() const {
    return pgtable_id_;
  }

  bool has_pgtable_id() const {
    return pgtable_id_ > 0;
  }

  void set_pgtable_id(const PgTableOid pgtable_id) {
    if (pgtable_id > 0) {
      DCHECK(cotable_id_.IsNil());
    }
    pgtable_id_ = pgtable_id;
  }

  // Extract a given column from a row where the type is
  // known at compile-time. The type is checked with a debug
  // assertion -- but if the wrong type is used and these assertions
  // are off, incorrect data may result.
  //
  // This is mostly useful for tests at this point.
  // TODO: consider removing it.
  template<DataType Type, class RowType>
  const typename DataTypeTraits<Type>::cpp_type *
  ExtractColumnFromRow(const RowType& row, size_t idx) const {
    DCHECK_SCHEMA_EQ(*this, *row.schema());
    const ColumnSchema& col_schema = cols_[idx];
    DCHECK_LT(idx, cols_.size());
    DCHECK_EQ(col_schema.type_info()->type(), Type);

    const void *val;
    if (col_schema.is_nullable()) {
      val = row.nullable_cell_ptr(idx);
    } else {
      val = row.cell_ptr(idx);
    }

    return reinterpret_cast<const typename DataTypeTraits<Type>::cpp_type *>(val);
  }

  // Stringify the given row, which conforms to this schema,
  // in a way suitable for debugging. This isn't currently optimized
  // so should be avoided in hot paths.
  template<class RowType>
  std::string DebugRow(const RowType& row) const {
    DCHECK_SCHEMA_EQ(*this, *row.schema());
    return DebugRowColumns(row, num_columns());
  }

  // Stringify the given row, which must have a schema which is
  // key-compatible with this one. Per above, this is not for use in
  // hot paths.
  template<class RowType>
  std::string DebugRowKey(const RowType& row) const {
    DCHECK_KEY_PROJECTION_SCHEMA_EQ(*this, *row.schema());
    return DebugRowColumns(row, num_key_columns());
  }

  // Decode the specified encoded key into the given 'buffer', which
  // must be at least as large as this->key_byte_size().
  //
  // 'arena' is used for allocating indirect strings, but is unused
  // for other datatypes.
  CHECKED_STATUS DecodeRowKey(Slice encoded_key, uint8_t* buffer,
                      Arena* arena) const WARN_UNUSED_RESULT;

  // Decode and stringify the given contiguous encoded row key in
  // order to, e.g., provide print human-readable information about a
  // tablet's start and end keys.
  //
  // If the encoded key is empty then '<start of table>' or '<end of table>'
  // will be returned based on the value of 'start_or_end'.
  //
  // See also: DebugRowKey, DecodeRowKey.
  enum StartOrEnd {
    START_KEY,
    END_KEY
  };
  std::string DebugEncodedRowKey(Slice encoded_key, StartOrEnd start_or_end) const;

  // Compare two rows of this schema.
  template<class RowTypeA, class RowTypeB>
  int Compare(const RowTypeA& lhs, const RowTypeB& rhs) const {
    DCHECK(KeyEquals(*lhs.schema()) && KeyEquals(*rhs.schema()));

    for (size_t col = 0; col < num_key_columns_; col++) {
      int col_compare = column(col).Compare(lhs.cell_ptr(col), rhs.cell_ptr(col));
      if (col_compare != 0) {
        return col_compare;
      }
    }
    return 0;
  }

  // Return the projection of this schema which contains only
  // the key columns.
  // TODO: this should take a Schema* out-parameter to avoid an
  // extra copy of the ColumnSchemas.
  // TODO this should probably be cached since the key projection
  // is not supposed to change, for a single schema.
  Schema CreateKeyProjection() const {
    vector<ColumnSchema> key_cols(cols_.begin(),
                                  cols_.begin() + num_key_columns_);
    vector<ColumnId> col_ids;
    if (!col_ids_.empty()) {
      col_ids.assign(col_ids_.begin(), col_ids_.begin() + num_key_columns_);
    }

    return Schema(key_cols, col_ids, num_key_columns_);
  }

  // Initialize column IDs by default values.
  // Requires that this schema has no column IDs.
  void InitColumnIdsByDefault();

  // Return a new Schema which is the same as this one, but without any column
  // IDs assigned.
  //
  // Requires that this schema has column IDs.
  Schema CopyWithoutColumnIds() const;

  // Create a new schema containing only the selected columns.
  // The resulting schema will have no key columns defined.
  // If this schema has IDs, the resulting schema will as well.
  CHECKED_STATUS CreateProjectionByNames(const std::vector<GStringPiece>& col_names,
                                         Schema* out, size_t num_key_columns = 0) const;

  // Create a new schema containing only the selected column IDs.
  //
  // If any column IDs are invalid, then they will be ignored and the
  // result will have fewer columns than requested.
  //
  // The resulting schema will have no key columns defined.
  CHECKED_STATUS CreateProjectionByIdsIgnoreMissing(const std::vector<ColumnId>& col_ids,
                                                    Schema* out) const;

  // Encode the key portion of the given row into a buffer
  // such that the buffer's lexicographic comparison represents
  // the proper comparison order of the underlying types.
  //
  // The key is encoded into the given buffer, replacing its current
  // contents.
  // Returns the encoded key.
  template <class RowType>
  Slice EncodeComparableKey(const RowType& row, faststring *dst) const {
    DCHECK_KEY_PROJECTION_SCHEMA_EQ(*this, *row.schema());

    dst->clear();
    for (size_t i = 0; i < num_key_columns_; i++) {
      DCHECK(!cols_[i].is_nullable());
      const TypeInfo* ti = cols_[i].type_info();
      bool is_last = i == num_key_columns_ - 1;
      GetKeyEncoder<faststring>(ti).Encode(row.cell_ptr(i), is_last, dst);
    }
    return Slice(*dst);
  }

  // Stringify this Schema. This is not particularly efficient,
  // so should only be used when necessary for output.
  std::string ToString() const;

  // Return true if the schemas have exactly the same set of columns
  // and respective types, and the same table properties.
  template <typename ColumnComparator>
  bool Equals(const Schema &other, ColumnComparator comp) const {
    if (this == &other) return true;
    if (this->num_key_columns_ != other.num_key_columns_) return false;
    if (this->table_properties_ != other.table_properties_) return false;
    if (this->cols_.size() != other.cols_.size()) return false;

    for (size_t i = 0; i < other.cols_.size(); i++) {
      if (!this->cols_[i].Equals(other.cols_[i], comp)) return false;
    }

    return true;
  }

  bool Equals(const Schema &other) const {
    return Equals(other, ColumnSchema::CompareByDefault);
  }

  // Return true if the schemas have exactly the same set of columns
  // and respective types, and equivalent properties.
  // For example, one table property could have different properties for wal_retention_secs_ and
  // retain_delete_markers_ but still be equivalent.
  bool EquivalentForDataCopy(const Schema& other) const {
    if (this == &other) return true;
    if (this->num_key_columns_ != other.num_key_columns_) return false;
    if (!this->table_properties_.Equivalent(other.table_properties_)) return false;
    if (this->cols_.size() != other.cols_.size()) return false;

    for (size_t i = 0; i < other.cols_.size(); i++) {
      if (!this->cols_[i].Equals(other.cols_[i])) return false;
    }

    return true;
  }

  // Return true if the key projection schemas have exactly the same set of
  // columns and respective types. Doesn't check column names.
  bool KeyEquals(const Schema& other) const {
    if (this->num_key_columns_ != other.num_key_columns_) return false;
    for (size_t i = 0; i < this->num_key_columns_; i++) {
      if (!this->cols_[i].EqualsType(other.cols_[i])) return false;
    }
    return true;
  }

  // Return a non-OK status if the project is not compatible with the current schema
  // - User columns non present in the tablet are considered errors
  // - Matching columns with different types, at the moment, are considered errors
  CHECKED_STATUS VerifyProjectionCompatibility(const Schema& projection) const;

  // Returns the projection schema mapped on the current one
  // If the project is invalid, return a non-OK status.
  CHECKED_STATUS GetMappedReadProjection(const Schema& projection,
                                 Schema *mapped_projection) const;

  // Loops through this schema (the projection) and calls the projector methods once for
  // each column.
  //
  // - CHECKED_STATUS ProjectBaseColumn(size_t proj_col_idx, size_t base_col_idx)
  //
  //     Called if the column in this schema matches one of the columns in 'base_schema'.
  //     The column type must match exactly.
  //
  // - CHECKED_STATUS ProjectDefaultColumn(size_t proj_idx)
  //
  //     Called if the column in this schema does not match any column in 'base_schema',
  //     but has a default or is nullable.
  //
  // - CHECKED_STATUS ProjectExtraColumn(size_t proj_idx, const ColumnSchema& col)
  //
  //     Called if the column in this schema does not match any column in 'base_schema',
  //     and does not have a default, and is not nullable.
  //
  // If both schemas have column IDs, then the matching is done by ID. Otherwise, it is
  // done by name.
  //
  // TODO(MAYBE): Pass the ColumnSchema and not only the column index?
  template <class Projector>
  CHECKED_STATUS GetProjectionMapping(const Schema& base_schema, Projector *projector) const {
    const bool use_column_ids = base_schema.has_column_ids() && has_column_ids();

    int proj_idx = 0;
    for (int i = 0; i < cols_.size(); ++i) {
      const ColumnSchema& col_schema = cols_[i];

      // try to lookup the column by ID if present or just by name.
      // Unit tests and Iter-Projections are probably always using the
      // lookup by name. The IDs are generally set by the server on AlterTable().
      int base_idx;
      if (use_column_ids) {
        base_idx = base_schema.find_column_by_id(col_ids_[i]);
      } else {
        base_idx = base_schema.find_column(col_schema.name());
      }

      if (base_idx >= 0) {
        const ColumnSchema& base_col_schema = base_schema.column(base_idx);
        // Column present in the Base Schema...
        if (!col_schema.EqualsType(base_col_schema)) {
          // ...but with a different type, (TODO: try with an adaptor)
          return STATUS(InvalidArgument, "The column '" + col_schema.name() +
                                         "' must have type " +
                                         base_col_schema.TypeToString() +
                                         " found " + col_schema.TypeToString());
        } else {
          RETURN_NOT_OK(projector->ProjectBaseColumn(proj_idx, base_idx));
        }
      } else {
        if (!col_schema.is_nullable()) {
          RETURN_NOT_OK(projector->ProjectExtraColumn(proj_idx));
        }
      }
      proj_idx++;
    }
    return Status::OK();
  }

  // Returns the column index given the column ID.
  // If no such column exists, returns kColumnNotFound.
  int find_column_by_id(ColumnId id) const {
    DCHECK(cols_.empty() || has_column_ids());
    int ret = id_to_index_[id];
    if (ret == -1) {
      return kColumnNotFound;
    }
    return ret;
  }

  // Returns the memory usage of this object without the object itself. Should
  // be used when embedded inside another object.
  size_t memory_footprint_excluding_this() const;

  // Returns the memory usage of this object including the object itself.
  // Should be used when allocated on the heap.
  size_t memory_footprint_including_this() const;

  static ColumnId first_column_id();

 private:

  void ResetColumnIds(const vector<ColumnId>& ids);

  // Return a stringified version of the first 'num_columns' columns of the
  // row.
  template<class RowType>
  std::string DebugRowColumns(const RowType& row, int num_columns) const {
    std::string ret;
    ret.append("(");

    for (size_t col_idx = 0; col_idx < num_columns; col_idx++) {
      if (col_idx > 0) {
        ret.append(", ");
      }
      const ColumnSchema& col = cols_[col_idx];
      col.DebugCellAppend(row.cell(col_idx), &ret);
    }
    ret.append(")");
    return ret;
  }

  friend class SchemaBuilder;

  vector<ColumnSchema> cols_;
  size_t num_key_columns_;
  size_t num_hash_key_columns_;
  ColumnId max_col_id_;
  vector<ColumnId> col_ids_;
  vector<size_t> col_offsets_;

  // The keys of this map are GStringPiece references to the actual name members of the
  // ColumnSchema objects inside cols_. This avoids an extra copy of those strings,
  // and also allows us to do lookups on the map using GStringPiece keys, sometimes
  // avoiding copies.
  //
  // The map is instrumented with a counting allocator so that we can accurately
  // measure its memory footprint.
  int64_t name_to_index_bytes_;
  typedef STLCountingAllocator<std::pair<const GStringPiece, size_t> > NameToIndexMapAllocator;
  typedef unordered_map<
      GStringPiece,
      size_t,
      std::hash<GStringPiece>,
      std::equal_to<GStringPiece>,
      NameToIndexMapAllocator> NameToIndexMap;
  NameToIndexMap name_to_index_;

  IdMapping id_to_index_;

  // Cached indicator whether any columns are nullable.
  bool has_nullables_;

  // Cached indicator whether any columns are static.
  bool has_statics_ = false;

  TableProperties table_properties_;

  // Uuid of the non-primary table this schema belongs to co-located in a tablet. Nil for the
  // primary or single-tenant table.
  Uuid cotable_id_;

  // PG table OID of the non-primary table this schema belongs to in a tablet with colocated
  // tables. Nil for the primary or single-tenant table.
  PgTableOid pgtable_id_;

  // NOTE: if you add more members, make sure to add the appropriate
  // code to swap() and CopyFrom() as well to prevent subtle bugs.
};

// Helper used for schema creation/editing.
//
// Example:
//   Status s;
//   SchemaBuilder builder(base_schema);
//   s = builder.RemoveColumn("value");
//   s = builder.AddKeyColumn("key2", STRING);
//   s = builder.AddColumn("new_c1", UINT32);
//   ...
//   Schema new_schema = builder.Build();
//
// TODO(neil): Must introduce hash_key in this builder. Currently, only YBSchemaBuilder support
// hash key, and YBSchemaBuilder don't use this builder.
class SchemaBuilder {
 public:
  SchemaBuilder() { Reset(); }
  explicit SchemaBuilder(const Schema& schema) { Reset(schema); }

  void Reset();
  void Reset(const Schema& schema);

  bool is_valid() const { return cols_.size() > 0; }

  // Set the next column ID to be assigned to columns added with
  // AddColumn.
  void set_next_column_id(ColumnId next_id) {
    DCHECK_GE(next_id, ColumnId(0));
    next_id_ = next_id;
  }

  // Return the next column ID that would be assigned with AddColumn.
  ColumnId next_column_id() const {
    return next_id_;
  }

  void set_pgtable_id(PgTableOid pgtable_id) {
    pgtable_id_ = pgtable_id;
  }

  PgTableOid pgtable_id() const {
    return pgtable_id_;
  }

  void set_cotable_id(Uuid cotable_id) {
    cotable_id_ = cotable_id;
  }

  Uuid cotable_id() const {
    return cotable_id_;
  }

  Schema Build() const {
    return Schema(cols_, col_ids_, num_key_columns_, table_properties_, cotable_id_, pgtable_id_);
  }
  Schema BuildWithoutIds() const {
    return Schema(cols_, num_key_columns_, table_properties_, cotable_id_,  pgtable_id_);
  }

  // assumes type is allowed in primary key -- this should be checked before getting here
  // using DataType (not QLType) since primary key columns only support elementary types
  CHECKED_STATUS AddKeyColumn(const std::string& name, const std::shared_ptr<QLType>& type);
  CHECKED_STATUS AddKeyColumn(const std::string& name, DataType type);

  // assumes type is allowed in hash key -- this should be checked before getting here
  // using DataType (not QLType) since hash key columns only support elementary types
  CHECKED_STATUS AddHashKeyColumn(const std::string& name, const std::shared_ptr<QLType>& type);
  CHECKED_STATUS AddHashKeyColumn(const std::string& name, DataType type);

  CHECKED_STATUS AddColumn(const ColumnSchema& column, bool is_key);

  CHECKED_STATUS AddColumn(const std::string& name, const std::shared_ptr<QLType>& type) {
    return AddColumn(name, type, false, false, false, false, 0,
                     ColumnSchema::SortingType::kNotSpecified);
  }

  // convenience function for adding columns with simple (non-parametric) data types
  CHECKED_STATUS AddColumn(const std::string& name, DataType type) {
    return AddColumn(name, QLType::Create(type));
  }

  CHECKED_STATUS AddNullableColumn(const std::string& name, const std::shared_ptr<QLType>& type) {
    return AddColumn(name, type, true, false, false, false, 0,
                     ColumnSchema::SortingType::kNotSpecified);
  }

  // convenience function for adding columns with simple (non-parametric) data types
  CHECKED_STATUS AddNullableColumn(const std::string& name, DataType type) {
    return AddNullableColumn(name, QLType::Create(type));
  }

  CHECKED_STATUS AddColumn(const std::string& name,
                           const std::shared_ptr<QLType>& type,
                           bool is_nullable,
                           bool is_hash_key,
                           bool is_static,
                           bool is_counter,
                           int32_t order,
                           yb::ColumnSchema::SortingType sorting_type);

  // convenience function for adding columns with simple (non-parametric) data types
  CHECKED_STATUS AddColumn(const std::string& name,
                           DataType type,
                           bool is_nullable,
                           bool is_hash_key,
                           bool is_static,
                           bool is_counter,
                           int32_t order,
                           yb::ColumnSchema::SortingType sorting_type) {
    return AddColumn(name, QLType::Create(type), is_nullable, is_hash_key, is_static, is_counter,
                     order, sorting_type);
  }

  CHECKED_STATUS RemoveColumn(const std::string& name);
  CHECKED_STATUS RenameColumn(const std::string& old_name, const std::string& new_name);
  CHECKED_STATUS AlterProperties(const TablePropertiesPB& pb);

 private:

  ColumnId next_id_;
  vector<ColumnId> col_ids_;
  vector<ColumnSchema> cols_;
  unordered_set<string> col_names_;
  size_t num_key_columns_;
  TableProperties table_properties_;
  PgTableOid pgtable_id_ = 0;
  Uuid cotable_id_ = Uuid(boost::uuids::nil_uuid());

  DISALLOW_COPY_AND_ASSIGN(SchemaBuilder);
};
} // namespace yb

// Specialize std::hash for ColumnId
namespace std {
template<>
struct hash<yb::ColumnId> {
  int operator()(const yb::ColumnId& col_id) const {
    return col_id;
  }
};
} // namespace std

#endif  // YB_COMMON_SCHEMA_H
