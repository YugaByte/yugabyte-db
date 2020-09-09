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

#ifndef ROCKSDB_LITE
#include "yb/rocksdb/table/adaptive_table_factory.h"

#include "yb/rocksdb/table/table_builder.h"
#include "yb/rocksdb/table/format.h"
#include "yb/rocksdb/port/port.h"

namespace rocksdb {

AdaptiveTableFactory::AdaptiveTableFactory(
    std::shared_ptr<TableFactory> table_factory_to_write,
    std::shared_ptr<TableFactory> block_based_table_factory,
    std::shared_ptr<TableFactory> plain_table_factory)
    : table_factory_to_write_(table_factory_to_write),
      block_based_table_factory_(block_based_table_factory),
      plain_table_factory_(plain_table_factory) {
  if (!table_factory_to_write_) {
    table_factory_to_write_ = block_based_table_factory_;
  }
  if (!plain_table_factory_) {
    plain_table_factory_.reset(NewPlainTableFactory());
  }
  if (!block_based_table_factory_) {
    block_based_table_factory_.reset(NewBlockBasedTableFactory());
  }
}

extern const uint64_t kPlainTableMagicNumber;
extern const uint64_t kLegacyPlainTableMagicNumber;
extern const uint64_t kBlockBasedTableMagicNumber;
extern const uint64_t kLegacyBlockBasedTableMagicNumber;

Status AdaptiveTableFactory::NewTableReader(
    const TableReaderOptions& table_reader_options,
    unique_ptr<RandomAccessFileReader>&& file, uint64_t file_size,
    unique_ptr<TableReader>* table) const {
  Footer footer;
  auto s = ReadFooterFromFile(file.get(), file_size, &footer);
  if (!s.ok()) {
    return s;
  }
  if (footer.table_magic_number() == kPlainTableMagicNumber ||
      footer.table_magic_number() == kLegacyPlainTableMagicNumber) {
    return plain_table_factory_->NewTableReader(
        table_reader_options, std::move(file), file_size, table);
  } else if (footer.table_magic_number() == kBlockBasedTableMagicNumber ||
      footer.table_magic_number() == kLegacyBlockBasedTableMagicNumber) {
    return block_based_table_factory_->NewTableReader(
        table_reader_options, std::move(file), file_size, table);
  } else {
    return STATUS(NotSupported, "Unidentified table format");
  }
}

TableBuilder* AdaptiveTableFactory::NewTableBuilder(
    const TableBuilderOptions &table_builder_options, uint32_t column_family_id,
    WritableFileWriter* base_file, WritableFileWriter* data_file) const {
  return table_factory_to_write_->NewTableBuilder(table_builder_options,
      column_family_id, base_file, data_file);
}

bool AdaptiveTableFactory::IsSplitSstForWriteSupported() const {
  return table_factory_to_write_->IsSplitSstForWriteSupported();
}

std::string AdaptiveTableFactory::GetPrintableTableOptions() const {
  std::string ret;
  ret.reserve(20000);
  const int kBufferSize = 200;
  char buffer[kBufferSize];

  if (!table_factory_to_write_) {
    snprintf(buffer, kBufferSize, "  write factory (%s) options:\n%s\n",
             table_factory_to_write_->Name(),
             table_factory_to_write_->GetPrintableTableOptions().c_str());
    ret.append(buffer);
  }
  if (!plain_table_factory_) {
    snprintf(buffer, kBufferSize, "  %s options:\n%s\n",
             plain_table_factory_->Name(),
             plain_table_factory_->GetPrintableTableOptions().c_str());
    ret.append(buffer);
  }
  if (!block_based_table_factory_) {
    snprintf(buffer, kBufferSize, "  %s options:\n%s\n",
             block_based_table_factory_->Name(),
             block_based_table_factory_->GetPrintableTableOptions().c_str());
    ret.append(buffer);
  }
  return ret;
}

extern TableFactory* NewAdaptiveTableFactory(
    std::shared_ptr<TableFactory> table_factory_to_write,
    std::shared_ptr<TableFactory> block_based_table_factory,
    std::shared_ptr<TableFactory> plain_table_factory) {
  return new AdaptiveTableFactory(table_factory_to_write,
      block_based_table_factory, plain_table_factory);
}

}  // namespace rocksdb
#endif  // ROCKSDB_LITE
