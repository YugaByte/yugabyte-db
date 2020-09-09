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

#ifndef YB_DOCDB_DOCDB_DEBUG_H_
#define YB_DOCDB_DOCDB_DEBUG_H_

#include <ostream>

#include <string>

#include <boost/function.hpp>

#include "yb/docdb/docdb_fwd.h"
#include "yb/docdb/docdb_types.h"
#include "yb/rocksdb/db.h"

namespace yb {
namespace docdb {

std::string EntryToString(const rocksdb::Iterator& iterator, StorageDbType db_type);

// Create a debug dump of the document database. Tries to decode all keys/values despite failures.
// Reports all errors to the output stream and returns the status of the first failed operation,
// if any.
void DocDBDebugDump(
    rocksdb::DB* rocksdb,
    std::ostream& out,
    StorageDbType db_type,
    IncludeBinary include_binary = IncludeBinary::kFalse);

std::string DocDBDebugDumpToStr(
    rocksdb::DB* rocksdb, StorageDbType db_type = StorageDbType::kRegular,
    IncludeBinary include_binary = IncludeBinary::kFalse);

std::string DocDBDebugDumpToStr(DocDB docdb, IncludeBinary include_binary = IncludeBinary::kFalse);

void DocDBDebugDumpToContainer(DocDB docdb, std::unordered_set<std::string>* out);

void DumpRocksDBToLog(rocksdb::DB* rocksdb, StorageDbType db_type = StorageDbType::kRegular);

}  // namespace docdb
}  // namespace yb

#endif  // YB_DOCDB_DOCDB_DEBUG_H_
