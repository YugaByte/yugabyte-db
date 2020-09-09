// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
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

#ifndef YB_ROCKSDB_TABLE_ADAPTIVE_TABLE_FACTORY_H
#define YB_ROCKSDB_TABLE_ADAPTIVE_TABLE_FACTORY_H

#ifndef ROCKSDB_LITE

#include <string>
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/status.h"
#include "yb/rocksdb/table.h"

namespace rocksdb {

struct EnvOptions;

using std::unique_ptr;
class WritableFile;
class Table;
class TableBuilder;

class AdaptiveTableFactory : public TableFactory {
 public:
  ~AdaptiveTableFactory() {}

  explicit AdaptiveTableFactory(
      std::shared_ptr<TableFactory> table_factory_to_write,
      std::shared_ptr<TableFactory> block_based_table_factory,
      std::shared_ptr<TableFactory> plain_table_factory);

  const char* Name() const override { return "AdaptiveTableFactory"; }

  Status NewTableReader(const TableReaderOptions& table_reader_options,
                        unique_ptr<RandomAccessFileReader>&& file,
                        uint64_t file_size,
                        unique_ptr<TableReader>* table) const override;

  TableBuilder *NewTableBuilder(const TableBuilderOptions &table_builder_options,
      uint32_t column_family_id,
      WritableFileWriter *base_file,
      WritableFileWriter *data_file = nullptr) const override;

  bool IsSplitSstForWriteSupported() const override;

  // Sanitizes the specified DB Options.
  Status SanitizeOptions(const DBOptions& db_opts,
                         const ColumnFamilyOptions& cf_opts) const override {
    return Status::OK();
  }

  std::string GetPrintableTableOptions() const override;

 private:
  std::shared_ptr<TableFactory> table_factory_to_write_;
  std::shared_ptr<TableFactory> block_based_table_factory_;
  std::shared_ptr<TableFactory> plain_table_factory_;
};

}  // namespace rocksdb
#endif  // ROCKSDB_LITE

#endif  // YB_ROCKSDB_TABLE_ADAPTIVE_TABLE_FACTORY_H
