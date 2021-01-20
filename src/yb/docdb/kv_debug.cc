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

#include <string>

#include "yb/docdb/kv_debug.h"

#include "yb/util/result.h"
#include "yb/util/format.h"

#include "yb/docdb/docdb_types.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/doc_kv_util.h"

namespace yb {
namespace docdb {

Result<std::string> DocDBKeyToDebugStr(Slice key_slice, StorageDbType db_type) {
  auto key_type = GetKeyType(key_slice, db_type);
  SubDocKey subdoc_key;
  switch (key_type) {
    case KeyType::kIntentKey:
    {
      auto decoded_intent_key = VERIFY_RESULT(DecodeIntentKey(key_slice));
      RETURN_NOT_OK(subdoc_key.FullyDecodeFromKeyWithOptionalHybridTime(
          decoded_intent_key.intent_prefix));
      return subdoc_key.ToString() + " " + ToString(decoded_intent_key.intent_types) + " " +
             decoded_intent_key.doc_ht.ToString();
    }
    case KeyType::kReverseTxnKey:
    {
      RETURN_NOT_OK(key_slice.consume_byte(ValueTypeAsChar::kTransactionId));
      auto transaction_id = VERIFY_RESULT(DecodeTransactionId(&key_slice));
      auto doc_ht = VERIFY_RESULT_PREPEND(
          DecodeInvertedDocHt(key_slice), Format("Reverse txn record for: $0", transaction_id));
      return Format("TXN REV $0 $1", transaction_id, doc_ht);
    }
    case KeyType::kTransactionMetadata:
    {
      RETURN_NOT_OK(key_slice.consume_byte(ValueTypeAsChar::kTransactionId));
      auto transaction_id = DecodeTransactionId(&key_slice);
      RETURN_NOT_OK(transaction_id);
      return Format("TXN META $0", *transaction_id);
    }
    case KeyType::kEmpty: FALLTHROUGH_INTENDED;
    case KeyType::kValueKey:
      RETURN_NOT_OK_PREPEND(
          subdoc_key.FullyDecodeFrom(key_slice),
          "Error: failed decoding RocksDB intent key " +
          FormatSliceAsStr(key_slice));
      return subdoc_key.ToString();
    case KeyType::kExternalIntents:
    {
      RETURN_NOT_OK(key_slice.consume_byte(ValueTypeAsChar::kExternalTransactionId));
      auto transaction_id = VERIFY_RESULT(DecodeTransactionId(&key_slice));
      auto doc_hybrid_time = VERIFY_RESULT_PREPEND(
          DecodeInvertedDocHt(key_slice), Format("External txn record for: $0", transaction_id));
      return Format("TXN EXT $0 $1", transaction_id, doc_hybrid_time);
    }
  }
  return STATUS_FORMAT(Corruption, "Corrupted KeyType: $0", yb::ToString(key_type));
}

Result<std::string> DocDBValueToDebugStr(Slice value_slice, const KeyType& key_type) {
  std::string prefix;
  if (key_type == KeyType::kIntentKey) {
    auto txn_id_res = VERIFY_RESULT(DecodeTransactionIdFromIntentValue(&value_slice));
    prefix = Format("TransactionId($0) ", txn_id_res);
    if (!value_slice.empty()) {
      RETURN_NOT_OK(value_slice.consume_byte(ValueTypeAsChar::kWriteId));
      if (value_slice.size() < sizeof(IntraTxnWriteId)) {
        return STATUS_FORMAT(Corruption, "Not enought bytes for write id: $0", value_slice.size());
      }
      auto write_id = BigEndian::Load32(value_slice.data());
      value_slice.remove_prefix(sizeof(write_id));
      prefix += Format("WriteId($0) ", write_id);
    }
  }

  // Empty values are allowed for weak intents.
  if (!value_slice.empty() || key_type != KeyType::kIntentKey) {
    Value v;
    RETURN_NOT_OK_PREPEND(
        v.Decode(value_slice),
        Format("Error: failed to decode value $0", prefix));
    return prefix + v.ToString();
  } else {
    return prefix + "none";
  }
}

Result<std::string> DocDBValueToDebugStr(
    KeyType key_type, const std::string& key_str, Slice value) {
  switch (key_type) {
    case KeyType::kTransactionMetadata: {
      TransactionMetadataPB metadata_pb;
      if (!metadata_pb.ParseFromArray(value.cdata(), value.size())) {
        return STATUS_FORMAT(Corruption, "Bad metadata: $0", value.ToDebugHexString());
      }
      return ToString(VERIFY_RESULT(TransactionMetadata::FromPB(metadata_pb)));
    }
    case KeyType::kReverseTxnKey:
      return DocDBKeyToDebugStr(value, StorageDbType::kIntents);
    case KeyType::kEmpty: FALLTHROUGH_INTENDED;
    case KeyType::kIntentKey: FALLTHROUGH_INTENDED;
    case KeyType::kValueKey:
      return DocDBValueToDebugStr(value, key_type);
    case KeyType::kExternalIntents: {
      std::vector<std::string> intents;
      SubDocKey sub_doc_key;
      RETURN_NOT_OK(value.consume_byte(ValueTypeAsChar::kUuid));
      Uuid involved_tablet;
      RETURN_NOT_OK(involved_tablet.FromSlice(value.Prefix(kUuidSize)));
      value.remove_prefix(kUuidSize);
      RETURN_NOT_OK(value.consume_byte(ValueTypeAsChar::kExternalIntents));
      for (;;) {
        auto len = VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&value));
        if (len == 0) {
          break;
        }
        RETURN_NOT_OK(sub_doc_key.FullyDecodeFrom(value.Prefix(len), HybridTimeRequired::kFalse));
        value.remove_prefix(len);
        len = VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&value));
        intents.push_back(Format(
            "$0 -> $1",
            sub_doc_key,
            VERIFY_RESULT(DocDBValueToDebugStr(value.Prefix(len), KeyType::kValueKey))));
        value.remove_prefix(len);
      }
      DCHECK(value.empty());
      return Format("IT $0 $1", involved_tablet.ToHexString(), intents);
    }
  }
  FATAL_INVALID_ENUM_VALUE(KeyType, key_type);
}

}  // namespace docdb
}  // namespace yb
