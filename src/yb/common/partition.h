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
#ifndef YB_COMMON_PARTITION_H
#define YB_COMMON_PARTITION_H

#include <algorithm>
#include <string>
#include <vector>

#include "yb/common/common.pb.h"
#include "yb/common/key_encoder.h"
#include "yb/common/partial_row.h"
#include "yb/common/ql_protocol.pb.h"
#include "yb/common/pgsql_protocol.pb.h"
#include "yb/common/row.h"
#include "yb/common/schema.h"
#include "yb/gutil/ref_counted.h"
#include "yb/util/status.h"

namespace yb {

class ConstContiguousRow;
class YBPartialRow;
class PartitionSchemaPB;
class TypeInfo;

enum YBHashSchema {
  kMultiColumnHash = 1, // YQL default hashing.
  kRedisHash = 2, // Redis default hashing.
  kPgsqlHash = 3 // PGSQL default hashing.
};

// A Partition describes the set of rows that a Tablet is responsible for
// serving. Each tablet is assigned a single Partition.
//
// Partitions consist primarily of a start and end partition key. Every row with
// a partition key that falls in a Tablet's Partition will be served by that
// tablet.
//
// In addition to the start and end partition keys, a Partition holds metadata
// to determine if a scan can prune, or skip, a partition based on the scan's
// start and end primary keys, and predicates.
class Partition {
 public:

  const std::vector<int32_t>& hash_buckets() const {
    return hash_buckets_;
  }

  Slice range_key_start() const;

  Slice range_key_end() const;

  const std::string& partition_key_start() const {
    return partition_key_start_;
  }

  const std::string& partition_key_end() const {
    return partition_key_end_;
  }

  void set_partition_key_start(const std::string& partition_key_start) {
    partition_key_start_ = partition_key_start;
  }

  void set_partition_key_end(const std::string& partition_key_end) {
    partition_key_end_ = partition_key_end;
  }

  // Serializes a partition into a protobuf message.
  void ToPB(PartitionPB* pb) const;

  // Deserializes a protobuf message into a partition.
  //
  // The protobuf message is not validated, since partitions are only expected
  // to be created by the master process.
  static void FromPB(const PartitionPB& pb, Partition* partition);

  bool ContainsKey(const std::string& partition_key) const {
    return partition_key >= partition_key_start() &&
        (partition_key_end().empty() || partition_key < partition_key_end());
  }

  // Checks if this partition is a superset or is exactly the same as another partition.
  template <class T>
  bool ContainsPartition(const T& other) const {
    return other.partition_key_start() >= partition_key_start() &&
           (partition_key_end().empty() || (!other.partition_key_end().empty() &&
                                            other.partition_key_end() <= partition_key_end()));
  }

  // Checks if this and another partitions have equal bounds.
  template <class T>
  bool BoundsEqualToPartition(const T& other) const {
    return partition_key_start() == other.partition_key_start() &&
           partition_key_end() == other.partition_key_end();
  }

  // Checks if this partition is a strict superset of another partition (shouldn't be exactly the
  // same).
  template <class T>
  bool ContainsPartitionStrict(const T& other) const {
    return ContainsPartition(other) && !BoundsEqualToPartition(other);
  }

  std::string ToString() const {
    return Format(
        "{ partition_key_start: $0 partition_key_end: $1 hash_buckets: $2 }",
        Slice(partition_key_start_).ToDebugString(),
        Slice(partition_key_end_).ToDebugString(),
        hash_buckets_);
  }

 private:
  friend class PartitionSchema;

  // Helper function for accessing the range key portion of a partition key.
  Slice range_key(const std::string& partition_key) const;

  std::vector<int32_t> hash_buckets_;

  std::string partition_key_start_;
  std::string partition_key_end_;
};

// A partition schema describes how the rows of a table are distributed among
// tablets.
//
// Primarily, a table's partition schema is responsible for translating the
// primary key column values of a row into a partition key that can be used to
// determine the tablet containing the key.
//
// The partition schema is made up of zero or more hash bucket components,
// followed by a single range component.
//
// Each hash bucket component includes one or more columns from the primary key
// column set, with the restriction that an individual primary key column may
// only be included in a single hash component.
//
// To determine the hash bucket of an individual row, the values of the columns
// of the hash component are encoded into bytes (in PK or lexicographic
// preserving encoding), then hashed into a u64, then modded into an i32. When
// constructing a partition key from a row, the buckets of the row are simply
// encoded into the partition key in order (again in PK or lexicographic
// preserving encoding).
//
// The range component contains a (possibly full or empty) subset of the primary
// key columns. When encoding the partition key, the columns of the partition
// component are encoded in order.
//
// The above is true of the relationship between rows and partition keys. It
// gets trickier with partitions (tablet partition key boundaries), because the
// boundaries of tablets do not necessarily align to rows. For instance,
// currently the absolute-start and absolute-end primary keys of a table
// represented as an empty key, but do not have a corresponding row. Partitions
// are similar, but instead of having just one absolute-start and absolute-end,
// each component of a partition schema has an absolute-start and absolute-end.
// When creating the initial set of partitions during table creation, we deal
// with this by "carrying through" absolute-start or absolute-ends into lower
// significance components.
class PartitionSchema {
 public:

  static constexpr int32_t kPartitionKeySize = 2;
  static constexpr int32_t kMaxPartitionKey = std::numeric_limits<uint16_t>::max();

  // Deserializes a protobuf message into a partition schema.
  static CHECKED_STATUS FromPB(const PartitionSchemaPB& pb,
                               const Schema& schema,
                               PartitionSchema* partition_schema) WARN_UNUSED_RESULT;

  // Serializes a partition schema into a protobuf message.
  void ToPB(PartitionSchemaPB* pb) const;

  CHECKED_STATUS EncodeRedisKey(const YBPartialRow& row, std::string* buf) const WARN_UNUSED_RESULT;

  CHECKED_STATUS EncodeRedisKey(const ConstContiguousRow& row,
                                std::string* buf) const WARN_UNUSED_RESULT;

  CHECKED_STATUS EncodeRedisKey(const Slice& slice, std::string* buf) const WARN_UNUSED_RESULT;

  CHECKED_STATUS EncodeKey(const google::protobuf::RepeatedPtrField<QLExpressionPB>& hash_values,
                           std::string* buf) const WARN_UNUSED_RESULT;

  CHECKED_STATUS EncodeKey(const google::protobuf::RepeatedPtrField<PgsqlExpressionPB>& hash_values,
                           std::string* buf) const WARN_UNUSED_RESULT;

  // Appends the row's encoded partition key into the provided buffer.
  // On failure, the buffer may have data partially appended.
  CHECKED_STATUS EncodeKey(const YBPartialRow& row, std::string* buf) const WARN_UNUSED_RESULT;

  // Appends the row's encoded partition key into the provided buffer.
  // On failure, the buffer may have data partially appended.
  CHECKED_STATUS EncodeKey(const ConstContiguousRow& row, std::string* buf) const
    WARN_UNUSED_RESULT;

  bool IsHashPartitioning() const {
    return hash_schema_ != boost::none;
  }

  YBHashSchema hash_schema() const {
    CHECK(hash_schema_);
    return *hash_schema_;
  }

  bool IsRangePartitioning() const {
    return range_schema_.column_ids.size() > 0;
  }

  // Encodes the given uint16 value into a 2 byte string.
  static std::string EncodeMultiColumnHashValue(uint16_t hash_value);

  // Decode the given partition_key to a 2-byte integer.
  static uint16_t DecodeMultiColumnHashValue(const string& partition_key);

  // Does [partition_key_start, partition_key_end] form a valid range.
  static CHECKED_STATUS IsValidHashPartitionRange(const string& partition_key_start,
                                                  const string& partition_key_end);

  static bool IsValidHashPartitionKeyBound(const string& partition_key);

  // YugaByte partition creation
  // Creates the set of table partitions using multi column hash schema. In this schema, we divide
  // the [0, max_partition_key] range into the requested number of intervals.
  // - Inputs are from SQL and CQL.
  // - YugaByte methods are used for hash and range partitioning to create tablets.
  //
  // TODO(neil) Investigate partitions to support both hash and range schema.
  // - First, use range schema to split the table.
  // - Second, use hash schema to partition each split.
  CHECKED_STATUS CreatePartitions(int32_t num_tablets, std::vector<Partition>* partitions) const;

  // Kudu partition creation
  // NOTE: The following function from Kudu is to support a C++ API instead of SQL or CQL. They
  // also create partitions differently. There are code in this function that shouldn't be apply
  // to YugaByte's database except for metadata in master, which is using Kudu's DB.
  //
  // Creates the set of table partitions for a partition schema and collection
  // of split rows.
  //
  // The number of resulting partitions is the product of the number of hash
  // buckets for each hash bucket component, multiplied by
  // (split_rows.size() + 1).
  CHECKED_STATUS CreatePartitions(const std::vector<YBPartialRow>& split_rows,
                                  const Schema& schema,
                                  std::vector<Partition>* partitions) const WARN_UNUSED_RESULT;

  // Tests if the partition contains the row.
  CHECKED_STATUS PartitionContainsRow(const Partition& partition,
                                      const YBPartialRow& row,
                                      bool* contains) const WARN_UNUSED_RESULT;

  // Tests if the partition contains the row.
  CHECKED_STATUS PartitionContainsRow(const Partition& partition,
                                      const ConstContiguousRow& row,
                                      bool* contains) const WARN_UNUSED_RESULT;

  // Returns a text description of the partition suitable for debug printing.
  std::string PartitionDebugString(const Partition& partition, const Schema& schema) const;

  // Returns a text description of a range partition suitable for debug printing.
  std::string RangePartitionDebugString(const Partition& partition, const Schema& schema) const;

  // Returns a text description of the partial row's partition key suitable for debug printing.
  std::string RowDebugString(const YBPartialRow& row) const;

  // Returns a text description of the row's partition key suitable for debug printing.
  std::string RowDebugString(const ConstContiguousRow& row) const;

  // Returns a text description of the encoded partition key suitable for debug printing.
  std::string PartitionKeyDebugString(const std::string& key, const Schema& schema) const;

  // Returns a text description of this partition schema suitable for debug printing.
  std::string DebugString(const Schema& schema) const;

  // Returns true if the other partition schema is equivalent to this one.
  bool Equals(const PartitionSchema& other) const;

  // Return true if the partitioning scheme simply range-partitions on the full primary key,
  // with no bucketing components, etc.
  bool IsSimplePKRangePartitioning(const Schema& schema) const;

 private:

  struct RangeSplit {
    explicit RangeSplit(const std::string& bounds) : column_bounds(bounds) {}

    std::string column_bounds;
  };

  struct RangeSchema {
    std::vector<ColumnId> column_ids;
    std::vector<RangeSplit> splits;
  };

  struct HashBucketSchema {
    std::vector<ColumnId> column_ids;
    int32_t num_buckets;
    uint32_t seed;
  };

  // Convertion between PB and partition schema.
  static CHECKED_STATUS KuduFromPB(const PartitionSchemaPB& pb,
                                 const Schema& schema,
                                 PartitionSchema* partition_schema);
  void KuduToPB(PartitionSchemaPB* pb) const;

  // Creates the set of table partitions using multi column hash schema. In this schema, we divide
  // the [ hash(0), hash(max_partition_key) ] range equally into the requested number of intervals.
  CHECKED_STATUS CreateHashPartitions(int32_t num_tablets,
                                      std::vector<Partition>* partitions,
                                      int32_t max_partition_key = kMaxPartitionKey) const;

  // Creates the set of table partitions using primary-key range schema. In this schema, we divide
  // the table by given ranges in the partitions vector.
  CHECKED_STATUS CreateRangePartitions(std::vector<Partition>* partitions) const;

  // Encodes the specified columns of a row into lexicographic sort-order preserving format.
  static CHECKED_STATUS EncodeColumns(const YBPartialRow& row,
                                      const std::vector<ColumnId>& column_ids,
                                      std::string* buf);

  // Encodes the specified columns of a row into lexicographic sort-order preserving format.
  static CHECKED_STATUS EncodeColumns(const ConstContiguousRow& row,
                                      const std::vector<ColumnId>& column_ids,
                                      std::string* buf);

  // Hashes a compound string of all columns into a 16-bit integer.
  static uint16_t HashColumnCompoundValue(const string& compound);

  // Encodes the specified columns of a row into 2-byte partition key using the multi column
  // hashing scheme.
  static CHECKED_STATUS EncodeColumns(const YBPartialRow& row, string* buf);

  // Encodes the specified columns of a row into 2-byte partition key using the multi column
  // hashing scheme.
  static CHECKED_STATUS EncodeColumns(const ConstContiguousRow& row, string* buf);

  // Assigns the row to a hash bucket according to the hash schema.
  template<typename Row>
  static CHECKED_STATUS BucketForRow(const Row& row,
                                     const HashBucketSchema& hash_bucket_schema,
                                     int32_t* bucket);

  // Private templated helper for PartitionContainsRow.
  template<typename Row>
  CHECKED_STATUS PartitionContainsRowImpl(const Partition& partition,
                                          const Row& row,
                                          bool* contains) const;

  // Appends the stringified range partition components of a partial row to a
  // vector.
  //
  // If any columns of the range partition do not exist in the partial row,
  // processing stops and the provided default string piece is appended to the vector.
  void AppendRangeDebugStringComponentsOrString(const YBPartialRow& row,
                                                GStringPiece default_string,
                                                std::vector<std::string>* components) const;

  // Appends the stringified range partition components of a partial row to a
  // vector.
  //
  // If any columns of the range partition do not exist in the partial row, the
  // logical minimum value for that column will be used instead.
  void AppendRangeDebugStringComponentsOrMin(const YBPartialRow& row,
                                             std::vector<std::string>* components) const;

  // Decodes a range partition key into a partial row, with variable-length
  // fields stored in the arena.
  CHECKED_STATUS DecodeRangeKey(Slice* encode_key,
                                YBPartialRow* partial_row,
                                Arena* arena) const;

  // Decodes the hash bucket component of a partition key into its buckets.
  //
  // This should only be called with partition keys created from a row, not with
  // partition keys from a partition.
  CHECKED_STATUS DecodeHashBuckets(Slice* partition_key, std::vector<int32_t>* buckets) const;

  // Clears the state of this partition schema.
  void Clear();

  // Validates that this partition schema is valid. Returns OK, or an
  // appropriate error code for an invalid partition schema.
  CHECKED_STATUS Validate(const Schema& schema) const;

  std::vector<HashBucketSchema> hash_bucket_schemas_;
  RangeSchema range_schema_;
  boost::optional<YBHashSchema> hash_schema_; // Defined only for table that is hash-partitioned.
};

} // namespace yb

#endif // YB_COMMON_PARTITION_H
