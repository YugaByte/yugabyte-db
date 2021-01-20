//--------------------------------------------------------------------------------------------------
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
// Structure definitions for a Postgres table descriptor.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_PGGATE_PG_TABLEDESC_H_
#define YB_YQL_PGGATE_PG_TABLEDESC_H_

#include "yb/common/pgsql_protocol.pb.h"
#include "yb/client/client.h"
#include "yb/client/yb_op.h"
#include "yb/yql/pggate/pg_column.h"
#include "yb/docdb/doc_key.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------

// This class can be used to describe any reference of a column.
class PgTableDesc : public RefCountedThreadSafe<PgTableDesc> {
 public:

  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef scoped_refptr<PgTableDesc> ScopedRefPtr;

  explicit PgTableDesc(std::shared_ptr<client::YBTable> pg_table);

  const client::YBTableName& table_name() const;

  const std::shared_ptr<client::YBTable> table() const {
    return table_;
  }

  static int ToPgAttrNum(const string &attr_name, int attr_num);

  std::vector<PgColumn>& columns() {
    return columns_;
  }

  const size_t num_hash_key_columns() const;
  const size_t num_key_columns() const;
  const size_t num_columns() const;

  std::unique_ptr<client::YBPgsqlReadOp> NewPgsqlSelect();
  std::unique_ptr<client::YBPgsqlWriteOp> NewPgsqlInsert();
  std::unique_ptr<client::YBPgsqlWriteOp> NewPgsqlUpdate();
  std::unique_ptr<client::YBPgsqlWriteOp> NewPgsqlDelete();
  std::unique_ptr<client::YBPgsqlWriteOp> NewPgsqlTruncateColocated();

  // Find the column given the postgres attr number.
  Result<PgColumn *> FindColumn(int attr_num);

  CHECKED_STATUS GetColumnInfo(int16_t attr_number, bool *is_primary, bool *is_hash) const;

  bool IsHashPartitioned() const;

  bool IsRangePartitioned() const;

  const std::vector<std::string>& GetPartitions() const;

  int GetPartitionCount() const;

  // When reading a row given its associated ybctid, the ybctid value is decoded to the row.
  Result<string> DecodeYbctid(const Slice& ybctid) const;

  // Seek the tablet partition where the row whose "ybctid" value was given can be found.
  Result<int> FindPartitionIndex(const Slice& ybctid) const;

  // These values are set by  PgGate to optimize query to narrow the scanning range of a query.
  CHECKED_STATUS SetScanBoundary(PgsqlReadRequestPB *req,
                                 const string& partition_lower_bound,
                                 bool lower_bound_is_inclusive,
                                 const string& partition_upper_bound,
                                 bool upper_bound_is_inclusive);

  bool IsTransactional() const;
  bool IsColocated() const;

 private:
  std::shared_ptr<client::YBTable> table_;
  const std::shared_ptr<const client::VersionedTablePartitions> table_partitions_;

  std::vector<PgColumn> columns_;
  std::unordered_map<int, size_t> attr_num_map_; // Attr number to column index map.

  // Hidden columns.
  PgColumn column_ybctid_;
};

}  // namespace pggate
}  // namespace yb

#endif  // YB_YQL_PGGATE_PG_TABLEDESC_H_
