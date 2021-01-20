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

#ifndef YB_DOCDB_DOCDB_TYPES_H_
#define YB_DOCDB_DOCDB_TYPES_H_

#include "yb/util/enums.h"
#include "yb/util/strongly_typed_bool.h"

namespace yb {
namespace docdb {

// Used in various debug dump functions to specify whether to include binary representation in the
// output.
YB_STRONGLY_TYPED_BOOL(IncludeBinary);

// To decide between regular (permanent) records RocksDB and provisional records (intents) RocksDB.
YB_DEFINE_ENUM(StorageDbType, (kRegular)(kIntents));

// Type of keys written by DocDB into RocksDB.
YB_DEFINE_ENUM(
    KeyType,
    (kEmpty)
    (kIntentKey)
    (kReverseTxnKey)
    (kValueKey)
    (kTransactionMetadata)
    (kExternalIntents));

// ------------------------------------------------------------------------------------------------
// Bounds
// ------------------------------------------------------------------------------------------------

YB_DEFINE_ENUM(BoundType,
    (kInvalid)
    (kExclusiveLower)
    (kInclusiveLower)
    (kExclusiveUpper)
    (kInclusiveUpper));

inline BoundType LowerBound(bool exclusive) {
  return exclusive ? BoundType::kExclusiveLower : BoundType::kInclusiveLower;
}

inline BoundType UpperBound(bool exclusive) {
  return exclusive ? BoundType::kExclusiveUpper : BoundType::kInclusiveUpper;
}

} // namespace docdb
} // namespace yb

#endif  // YB_DOCDB_DOCDB_TYPES_H_
