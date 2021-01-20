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

#include "yb/common/transaction.h"
#include "yb/docdb/docdb-internal.h"

namespace yb {
namespace docdb {

KeyType GetKeyType(const Slice& slice, StorageDbType db_type) {
  if (slice.empty()) {
    return KeyType::kEmpty;
  }

  if (db_type == StorageDbType::kRegular) {
    return KeyType::kValueKey;
  }

  if (slice[0] == ValueTypeAsChar::kTransactionId) {
    if (slice.size() == TransactionId::StaticSize() + 1) {
      return KeyType::kTransactionMetadata;
    } else {
      return KeyType::kReverseTxnKey;
    }
  }
  if (slice[0] == ValueTypeAsChar::kExternalTransactionId) {
    return KeyType::kExternalIntents;
  }

  return KeyType::kIntentKey;
}

} // namespace docdb
} // namespace yb
