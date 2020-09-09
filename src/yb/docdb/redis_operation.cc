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

#include "yb/docdb/redis_operation.h"

#include "yb/docdb/doc_reader.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/doc_write_batch.h"
#include "yb/docdb/doc_write_batch_cache.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/subdocument.h"

#include "yb/util/stol_utils.h"
#include "yb/util/redis_util.h"

DEFINE_bool(emulate_redis_responses,
    true,
    "If emulate_redis_responses is false, we hope to get slightly better performance by just "
    "returning OK for commands that might require us to read additional records viz. SADD, HSET, "
    "and HDEL. If emulate_redis_responses is true, we read the required records to compute the "
    "response as specified by the official Redis API documentation. https://redis.io/commands");

namespace yb {
namespace docdb {

// A simple conversion from RedisDataTypes to ValueTypes
// Note: May run into issues if we want to support ttl on individual set elements,
// as they are represented by ValueType::kNullLow.
ValueType ValueTypeFromRedisType(RedisDataType dt) {
  switch(dt) {
  case RedisDataType::REDIS_TYPE_STRING:
    return ValueType::kString;
  case RedisDataType::REDIS_TYPE_SET:
    return ValueType::kRedisSet;
  case RedisDataType::REDIS_TYPE_HASH:
    return ValueType::kObject;
  case RedisDataType::REDIS_TYPE_SORTEDSET:
    return ValueType::kRedisSortedSet;
  case RedisDataType::REDIS_TYPE_TIMESERIES:
    return ValueType::kRedisTS;
  case RedisDataType::REDIS_TYPE_LIST:
    return ValueType::kRedisList;
  default:
    return ValueType::kInvalid;
  }
}

Status RedisWriteOperation::GetDocPaths(
    GetDocPathsMode mode, DocPathsToLock* paths, IsolationLevel *level) const {
  paths->push_back(DocKey::FromRedisKey(
      request_.key_value().hash_code(), request_.key_value().key()).EncodeAsRefCntPrefix());
  *level = IsolationLevel::SNAPSHOT_ISOLATION;

  return Status::OK();
}

namespace {

bool EmulateRedisResponse(const RedisDataType& data_type) {
  return FLAGS_emulate_redis_responses && data_type != REDIS_TYPE_TIMESERIES;
}

static const string wrong_type_message =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

CHECKED_STATUS PrimitiveValueFromSubKey(const RedisKeyValueSubKeyPB &subkey_pb,
                                        PrimitiveValue *primitive_value) {
  switch (subkey_pb.subkey_case()) {
    case RedisKeyValueSubKeyPB::kStringSubkey:
      *primitive_value = PrimitiveValue(subkey_pb.string_subkey());
      break;
    case RedisKeyValueSubKeyPB::kTimestampSubkey:
      // We use descending order for the timestamp in the timeseries type so that the latest
      // value sorts on top.
      *primitive_value = PrimitiveValue(subkey_pb.timestamp_subkey(), SortOrder::kDescending);
      break;
    case RedisKeyValueSubKeyPB::kDoubleSubkey: {
      *primitive_value = PrimitiveValue::Double(subkey_pb.double_subkey());
      break;
    }
    default:
      return STATUS_SUBSTITUTE(IllegalState, "Invalid enum value $0", subkey_pb.subkey_case());
  }
  return Status::OK();
}

// Stricter version of the above when we know the exact datatype to expect.
CHECKED_STATUS PrimitiveValueFromSubKeyStrict(const RedisKeyValueSubKeyPB &subkey_pb,
                                              const RedisDataType &data_type,
                                              PrimitiveValue *primitive_value) {
  switch (data_type) {
    case REDIS_TYPE_LIST: FALLTHROUGH_INTENDED;
    case REDIS_TYPE_SET: FALLTHROUGH_INTENDED;
    case REDIS_TYPE_HASH:
      if (!subkey_pb.has_string_subkey()) {
        return STATUS_SUBSTITUTE(InvalidArgument, "subkey: $0 should be of string type",
                                 subkey_pb.ShortDebugString());
      }
      break;
    case REDIS_TYPE_TIMESERIES:
      if (!subkey_pb.has_timestamp_subkey()) {
        return STATUS_SUBSTITUTE(InvalidArgument, "subkey: $0 should be of int64 type",
                                 subkey_pb.ShortDebugString());
      }
      break;
    case REDIS_TYPE_SORTEDSET:
      if (!subkey_pb.has_double_subkey()) {
        return STATUS_SUBSTITUTE(InvalidArgument, "subkey: $0 should be of double type",
                             subkey_pb.ShortDebugString());
      }
      break;
    default:
      return STATUS_SUBSTITUTE(IllegalState, "Invalid enum value $0", data_type);
  }
  return PrimitiveValueFromSubKey(subkey_pb, primitive_value);
}

Result<RedisDataType> GetRedisValueType(
    IntentAwareIterator* iterator,
    const RedisKeyValuePB &key_value_pb,
    DocWriteBatch* doc_write_batch = nullptr,
    int subkey_index = kNilSubkeyIndex,
    bool always_override = false) {
  if (!key_value_pb.has_key()) {
    return STATUS(Corruption, "Expected KeyValuePB");
  }
  KeyBytes encoded_subdoc_key;
  if (subkey_index == kNilSubkeyIndex) {
    encoded_subdoc_key = DocKey::EncodedFromRedisKey(key_value_pb.hash_code(), key_value_pb.key());
  } else {
    if (subkey_index >= key_value_pb.subkey_size()) {
      return STATUS_SUBSTITUTE(InvalidArgument,
                               "Size of subkeys ($0) must be larger than subkey_index ($1)",
                               key_value_pb.subkey_size(), subkey_index);
    }

    PrimitiveValue subkey_primitive;
    RETURN_NOT_OK(PrimitiveValueFromSubKey(key_value_pb.subkey(subkey_index), &subkey_primitive));
    encoded_subdoc_key = DocKey::EncodedFromRedisKey(key_value_pb.hash_code(), key_value_pb.key());
    subkey_primitive.AppendToKey(&encoded_subdoc_key);
  }
  SubDocument doc;
  bool doc_found = false;
  // Use the cached entry if possible to determine the value type.
  boost::optional<DocWriteBatchCache::Entry> cached_entry;
  if (doc_write_batch) {
    cached_entry = doc_write_batch->LookupCache(encoded_subdoc_key);
  }

  if (cached_entry) {
    doc_found = true;
    doc = SubDocument(cached_entry->value_type);
  } else {
    // TODO(dtxn) - pass correct transaction context when we implement cross-shard transactions
    // support for Redis.
    GetSubDocumentData data = { encoded_subdoc_key, &doc, &doc_found };
    data.return_type_only = true;
    data.exp.always_override = always_override;
    RETURN_NOT_OK(GetSubDocument(iterator, data, /* projection */ nullptr,
                                 SeekFwdSuffices::kFalse));
  }

  if (!doc_found) {
    return REDIS_TYPE_NONE;
  }

  switch (doc.value_type()) {
    case ValueType::kInvalid: FALLTHROUGH_INTENDED;
    case ValueType::kTombstone:
      return REDIS_TYPE_NONE;
    case ValueType::kObject:
      return REDIS_TYPE_HASH;
    case ValueType::kRedisSet:
      return REDIS_TYPE_SET;
    case ValueType::kRedisTS:
      return REDIS_TYPE_TIMESERIES;
    case ValueType::kRedisSortedSet:
      return REDIS_TYPE_SORTEDSET;
    case ValueType::kRedisList:
      return REDIS_TYPE_LIST;
    case ValueType::kNullLow: FALLTHROUGH_INTENDED; // This value is a set member.
    case ValueType::kString:
      return REDIS_TYPE_STRING;
    default:
      return STATUS_FORMAT(Corruption,
                           "Unknown value type for redis record: $0",
                           static_cast<char>(doc.value_type()));
  }
}

Result<RedisValue> GetRedisValue(
    IntentAwareIterator* iterator,
    const RedisKeyValuePB &key_value_pb,
    int subkey_index = kNilSubkeyIndex,
    bool always_override = false,
    Expiration* exp = nullptr) {
  if (!key_value_pb.has_key()) {
    return STATUS(Corruption, "Expected KeyValuePB");
  }
  auto encoded_doc_key = DocKey::EncodedFromRedisKey(key_value_pb.hash_code(), key_value_pb.key());

  if (!key_value_pb.subkey().empty()) {
    if (key_value_pb.subkey().size() != 1 && subkey_index == kNilSubkeyIndex) {
      return STATUS_SUBSTITUTE(Corruption,
                               "Expected at most one subkey, got $0", key_value_pb.subkey().size());
    }
    PrimitiveValue subkey_primitive;
    RETURN_NOT_OK(PrimitiveValueFromSubKey(
        key_value_pb.subkey(subkey_index == kNilSubkeyIndex ? 0 : subkey_index),
        &subkey_primitive));
    subkey_primitive.AppendToKey(&encoded_doc_key);
  }

  SubDocument doc;
  bool doc_found = false;

  // TODO(dtxn) - pass correct transaction context when we implement cross-shard transactions
  // support for Redis.
  GetSubDocumentData data = { encoded_doc_key, &doc, &doc_found };
  data.exp.always_override = always_override;
  RETURN_NOT_OK(GetSubDocument(iterator, data, /* projection */ nullptr, SeekFwdSuffices::kFalse));
  if (!doc_found) {
    return RedisValue{REDIS_TYPE_NONE};
  }

  bool has_expired = false;
  CHECK_OK(HasExpiredTTL(data.exp.write_ht, data.exp.ttl,
                         iterator->read_time().read, &has_expired));
  if (has_expired)
    return RedisValue{REDIS_TYPE_NONE};

  if (exp)
    *exp = data.exp;

  if (!doc.IsPrimitive()) {
    switch (doc.value_type()) {
      case ValueType::kObject:
        return RedisValue{REDIS_TYPE_HASH};
      case ValueType::kRedisTS:
        return RedisValue{REDIS_TYPE_TIMESERIES};
      case ValueType::kRedisSortedSet:
        return RedisValue{REDIS_TYPE_SORTEDSET};
      case ValueType::kRedisSet:
        return RedisValue{REDIS_TYPE_SET};
      case ValueType::kRedisList:
        return RedisValue{REDIS_TYPE_LIST};
      default:
        return STATUS_SUBSTITUTE(IllegalState, "Invalid value type: $0",
                                 static_cast<int>(doc.value_type()));
    }
  }

  auto val = RedisValue{REDIS_TYPE_STRING, doc.GetString(), data.exp};
  return val;
}

YB_STRONGLY_TYPED_BOOL(VerifySuccessIfMissing);

// Set response based on the type match. Return whether the type matches what's expected.
bool VerifyTypeAndSetCode(
    const RedisDataType expected_type,
    const RedisDataType actual_type,
    RedisResponsePB *response,
    VerifySuccessIfMissing verify_success_if_missing = VerifySuccessIfMissing::kFalse) {
  if (actual_type == RedisDataType::REDIS_TYPE_NONE) {
    if (verify_success_if_missing) {
      response->set_code(RedisResponsePB::NIL);
    } else {
      response->set_code(RedisResponsePB::NOT_FOUND);
    }
    return verify_success_if_missing;
  }
  if (actual_type != expected_type) {
    response->set_code(RedisResponsePB::WRONG_TYPE);
    response->set_error_message(wrong_type_message);
    return false;
  }
  response->set_code(RedisResponsePB::OK);
  return true;
}

bool VerifyTypeAndSetCode(
    const docdb::ValueType expected_type,
    const docdb::ValueType actual_type,
    RedisResponsePB *response) {
  if (actual_type != expected_type) {
    response->set_code(RedisResponsePB::WRONG_TYPE);
    response->set_error_message(wrong_type_message);
    return false;
  }
  response->set_code(RedisResponsePB::OK);
  return true;
}

CHECKED_STATUS AddPrimitiveValueToResponseArray(const PrimitiveValue& value,
                                                RedisArrayPB* redis_array) {
  switch (value.value_type()) {
    case ValueType::kString: FALLTHROUGH_INTENDED;
    case ValueType::kStringDescending: {
      redis_array->add_elements(value.GetString());
      return Status::OK();
    }
    case ValueType::kInt64: FALLTHROUGH_INTENDED;
    case ValueType::kInt64Descending: {
      redis_array->add_elements(std::to_string(value.GetInt64()));
      return Status::OK();
    }
    case ValueType::kDouble: FALLTHROUGH_INTENDED;
    case ValueType::kDoubleDescending: {
      redis_array->add_elements(std::to_string(value.GetDouble()));
      return Status::OK();
    }
    default:
      return STATUS_SUBSTITUTE(InvalidArgument, "Invalid value type: $0",
                             static_cast<int>(value.value_type()));
  }
}

CHECKED_STATUS AddResponseValuesGeneric(const PrimitiveValue& first,
                                        const PrimitiveValue& second,
                                        RedisResponsePB* response,
                                        bool add_keys,
                                        bool add_values,
                                        bool reverse = false) {
  if (add_keys) {
    RETURN_NOT_OK(AddPrimitiveValueToResponseArray(first, response->mutable_array_response()));
  }
  if (add_values) {
    RETURN_NOT_OK(AddPrimitiveValueToResponseArray(second, response->mutable_array_response()));
  }
  return Status::OK();
}

// Populate the response array for sorted sets range queries.
// first refers to the score for the given values.
// second refers to a subdocument where each key is a value with the given score.
CHECKED_STATUS AddResponseValuesSortedSets(const PrimitiveValue& first,
                                           const SubDocument& second,
                                           RedisResponsePB* response,
                                           bool add_keys,
                                           bool add_values,
                                           bool reverse = false) {
  if (reverse) {
    for (auto it = second.object_container().rbegin();
         it != second.object_container().rend();
         it++) {
      const PrimitiveValue& value = it->first;
      RETURN_NOT_OK(AddResponseValuesGeneric(value, first, response, add_values, add_keys));
    }
  } else {
    for (const auto& kv : second.object_container()) {
      const PrimitiveValue& value = kv.first;
      RETURN_NOT_OK(AddResponseValuesGeneric(value, first, response, add_values, add_keys));
    }
  }
  return Status::OK();
}

template <typename T, typename AddResponseRow>
CHECKED_STATUS PopulateRedisResponseFromInternal(T iter,
                                                 AddResponseRow add_response_row,
                                                 const T& iter_end,
                                                 RedisResponsePB *response,
                                                 bool add_keys,
                                                 bool add_values,
                                                 bool reverse = false) {
  response->set_allocated_array_response(new RedisArrayPB());
  for (; iter != iter_end; iter++) {
    RETURN_NOT_OK(add_response_row(
        iter->first, iter->second, response, add_keys, add_values, reverse));
  }
  return Status::OK();
}

template <typename AddResponseRow>
CHECKED_STATUS PopulateResponseFrom(const SubDocument::ObjectContainer &key_values,
                                    AddResponseRow add_response_row,
                                    RedisResponsePB *response,
                                    bool add_keys,
                                    bool add_values,
                                    bool reverse = false) {
  if (reverse) {
    return PopulateRedisResponseFromInternal(key_values.rbegin(), add_response_row,
                                             key_values.rend(), response, add_keys,
                                             add_values, reverse);
  } else {
    return PopulateRedisResponseFromInternal(key_values.begin(),  add_response_row,
                                             key_values.end(), response, add_keys,
                                             add_values, reverse);
  }
}

void SetOptionalInt(RedisDataType type, int64_t value, int64_t none_value,
                    RedisResponsePB* response) {
  if (type == RedisDataType::REDIS_TYPE_NONE) {
    response->set_code(RedisResponsePB::NIL);
    response->set_int_response(none_value);
  } else {
    response->set_code(RedisResponsePB::OK);
    response->set_int_response(value);
  }
}

void SetOptionalInt(RedisDataType type, int64_t value, RedisResponsePB* response) {
  SetOptionalInt(type, value, 0, response);
}

Result<int64_t> GetCardinality(IntentAwareIterator* iterator, const RedisKeyValuePB& kv) {
  auto encoded_key_card = DocKey::EncodedFromRedisKey(kv.hash_code(), kv.key());
  PrimitiveValue(ValueType::kCounter).AppendToKey(&encoded_key_card);
  SubDocument subdoc_card;

  bool subdoc_card_found = false;
  GetSubDocumentData data = { encoded_key_card, &subdoc_card, &subdoc_card_found };

  RETURN_NOT_OK(GetSubDocument(iterator, data, /* projection */ nullptr, SeekFwdSuffices::kFalse));

  return subdoc_card_found ? subdoc_card.GetInt64() : 0;
}

template <typename AddResponseValues>
CHECKED_STATUS GetAndPopulateResponseValues(
    IntentAwareIterator* iterator,
    AddResponseValues add_response_values,
    const GetSubDocumentData& data,
    ValueType expected_type,
    const RedisReadRequestPB& request,
    RedisResponsePB* response,
    bool add_keys, bool add_values, bool reverse) {

  RETURN_NOT_OK(GetSubDocument(iterator, data, /* projection */ nullptr, SeekFwdSuffices::kFalse));

  // Validate and populate response.
  response->set_allocated_array_response(new RedisArrayPB());
  if (!(*data.doc_found)) {
    response->set_code(RedisResponsePB::NIL);
    return Status::OK();
  }

  if (VerifyTypeAndSetCode(expected_type, data.result->value_type(), response)) {
      RETURN_NOT_OK(PopulateResponseFrom(data.result->object_container(),
                                         add_response_values,
                                         response, add_keys, add_values, reverse));
  }
  return Status::OK();
}

// Get normalized (with respect to card) upper and lower index bounds for reverse range scans.
void GetNormalizedBounds(int64 low_idx, int64 high_idx, int64 card, bool reverse,
                         int64* low_idx_normalized, int64* high_idx_normalized) {
  // Turn negative bounds positive.
  if (low_idx < 0) {
    low_idx = card + low_idx;
  }
  if (high_idx < 0) {
    high_idx = card + high_idx;
  }

  // Index from lower to upper instead of upper to lower.
  if (reverse) {
    *low_idx_normalized = card - high_idx - 1;
    *high_idx_normalized = card - low_idx - 1;
  } else {
    *low_idx_normalized = low_idx;
    *high_idx_normalized = high_idx;
  }

  // Fit bounds to range [0, card).
  if (*low_idx_normalized < 0) {
    *low_idx_normalized = 0;
  }
  if (*high_idx_normalized >= card) {
    *high_idx_normalized = card - 1;
  }
}

} // anonymous namespace

void RedisWriteOperation::InitializeIterator(const DocOperationApplyData& data) {
  auto subdoc_key = SubDocKey(DocKey::FromRedisKey(
      request_.key_value().hash_code(), request_.key_value().key()));

  auto iter = CreateIntentAwareIterator(
      data.doc_write_batch->doc_db(),
      BloomFilterMode::USE_BLOOM_FILTER, subdoc_key.Encode().AsSlice(),
      redis_query_id(), /* txn_op_context */ boost::none, data.deadline, data.read_time);

  iterator_ = std::move(iter);
}

Status RedisWriteOperation::Apply(const DocOperationApplyData& data) {
  switch (request_.request_case()) {
    case RedisWriteRequestPB::kSetRequest:
      return ApplySet(data);
    case RedisWriteRequestPB::kSetTtlRequest:
      return ApplySetTtl(data);
    case RedisWriteRequestPB::kGetsetRequest:
      return ApplyGetSet(data);
    case RedisWriteRequestPB::kAppendRequest:
      return ApplyAppend(data);
    case RedisWriteRequestPB::kDelRequest:
      return ApplyDel(data);
    case RedisWriteRequestPB::kSetRangeRequest:
      return ApplySetRange(data);
    case RedisWriteRequestPB::kIncrRequest:
      return ApplyIncr(data);
    case RedisWriteRequestPB::kPushRequest:
      return ApplyPush(data);
    case RedisWriteRequestPB::kInsertRequest:
      return ApplyInsert(data);
    case RedisWriteRequestPB::kPopRequest:
      return ApplyPop(data);
    case RedisWriteRequestPB::kAddRequest:
      return ApplyAdd(data);
    // TODO: Cut this short in doc_operation.
    case RedisWriteRequestPB::kNoOpRequest:
      return Status::OK();
    case RedisWriteRequestPB::REQUEST_NOT_SET: break;
  }
  return STATUS_FORMAT(Corruption, "Unsupported redis read operation: $0", request_.request_case());
}

Result<RedisDataType> RedisWriteOperation::GetValueType(
    const DocOperationApplyData& data, int subkey_index) {
  if (!iterator_) {
    InitializeIterator(data);
  }
  return GetRedisValueType(
      iterator_.get(), request_.key_value(), data.doc_write_batch, subkey_index);
}

Result<RedisValue> RedisWriteOperation::GetValue(
    const DocOperationApplyData& data, int subkey_index, Expiration* ttl) {
  if (!iterator_) {
    InitializeIterator(data);
  }
  return GetRedisValue(iterator_.get(), request_.key_value(),
                       subkey_index, /* always_override */ false, ttl);
}

Status RedisWriteOperation::ApplySet(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  const MonoDelta ttl = request_.set_request().has_ttl() ?
      MonoDelta::FromMilliseconds(request_.set_request().ttl()) : Value::kMaxTtl;
  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  if (kv.subkey_size() > 0) {
    RedisDataType data_type = VERIFY_RESULT(GetValueType(data));
    switch (kv.type()) {
      case REDIS_TYPE_TIMESERIES: FALLTHROUGH_INTENDED;
      case REDIS_TYPE_HASH: {
        if (data_type != kv.type() && data_type != REDIS_TYPE_NONE) {
          response_.set_code(RedisResponsePB::WRONG_TYPE);
          response_.set_error_message(wrong_type_message);
          return Status::OK();
        }
        SubDocument kv_entries = SubDocument();
        for (int i = 0; i < kv.subkey_size(); i++) {
          PrimitiveValue subkey_value;
          RETURN_NOT_OK(PrimitiveValueFromSubKeyStrict(kv.subkey(i), kv.type(), &subkey_value));
          kv_entries.SetChild(subkey_value,
                              SubDocument(PrimitiveValue(kv.value(i))));
        }

        if (kv.type() == REDIS_TYPE_TIMESERIES) {
          RETURN_NOT_OK(kv_entries.ConvertToRedisTS());
        }

        // For an HSET command (which has only one subkey), we need to read the subkey to find out
        // if the key already existed, and return 0 or 1 accordingly. This read is unnecessary for
        // HMSET and TSADD.
        if (kv.subkey_size() == 1 && EmulateRedisResponse(kv.type()) &&
            !request_.set_request().expect_ok_response()) {
          RedisDataType type = VERIFY_RESULT(GetValueType(data, 0));
          // For HSET/TSADD, we return 0 or 1 depending on if the key already existed.
          // If flag is false, no int response is returned.
          SetOptionalInt(type, 0, 1, &response_);
        }
        if (data_type == REDIS_TYPE_NONE && kv.type() == REDIS_TYPE_TIMESERIES) {
          // Need to insert the document instead of extending it.
          RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
              doc_path, kv_entries, data.read_time, data.deadline, redis_query_id(), ttl,
              Value::kInvalidUserTimestamp, false /* init_marker_ttl */));
        } else {
          RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
              doc_path, kv_entries, data.read_time, data.deadline, redis_query_id(), ttl));
        }
        break;
      }
      case REDIS_TYPE_SORTEDSET: {
        if (data_type != kv.type() && data_type != REDIS_TYPE_NONE) {
          response_.set_code(RedisResponsePB::WRONG_TYPE);
          response_.set_error_message(wrong_type_message);
          return Status::OK();
        }

        // The SubDocuments to be inserted for card, the forward mapping, and reverse mapping.
        SubDocument kv_entries_card;
        SubDocument kv_entries_forward;
        SubDocument kv_entries_reverse;

        // The top level mapping.
        SubDocument kv_entries;

        int new_elements_added = 0;
        int return_value = 0;
        for (int i = 0; i < kv.subkey_size(); i++) {
          // Check whether the value is already in the document, if so delete it.
          SubDocKey key_reverse = SubDocKey(DocKey::FromRedisKey(kv.hash_code(), kv.key()),
                                            PrimitiveValue(ValueType::kSSReverse),
                                            PrimitiveValue(kv.value(i)));
          SubDocument subdoc_reverse;
          bool subdoc_reverse_found = false;
          auto encoded_key_reverse = key_reverse.EncodeWithoutHt();
          GetSubDocumentData get_data = { encoded_key_reverse, &subdoc_reverse,
                                          &subdoc_reverse_found };
          RETURN_NOT_OK(GetSubDocument(
              data.doc_write_batch->doc_db(),
              get_data, redis_query_id(), boost::none /* txn_op_context */, data.deadline,
              data.read_time));

          // Flag indicating whether we should add the given entry to the sorted set.
          bool should_add_entry = true;
          // Flag indicating whether we shoould remove an entry from the sorted set.
          bool should_remove_existing_entry = false;

          if (!subdoc_reverse_found) {
            // The value is not already in the document.
            switch (request_.set_request().sorted_set_options().update_options()) {
              case SortedSetOptionsPB::NX: FALLTHROUGH_INTENDED;
              case SortedSetOptionsPB::NONE: {
                // Both these options call for inserting new elements, increment return_value and
                // keep should_add_entry as true.
                return_value++;
                new_elements_added++;
                break;
              }
              default: {
                // XX option calls for no new elements, don't increment return_value and set
                // should_add_entry to false.
                should_add_entry = false;
                break;
              }
            }
          } else {
            // The value is already in the document.
            switch (request_.set_request().sorted_set_options().update_options()) {
              case SortedSetOptionsPB::XX:
              case SortedSetOptionsPB::NONE: {
                // First make sure that the new score is different from the old score.
                // Both these options call for updating existing elements, set
                // should_remove_existing_entry to true, and if the CH flag is on (return both
                // elements changed and elements added), increment return_value.
                double score_to_remove = subdoc_reverse.GetDouble();
                // If incr option is set, we add the increment to the existing score.
                double score_to_add = request_.set_request().sorted_set_options().incr() ?
                    score_to_remove + kv.subkey(i).double_subkey() : kv.subkey(i).double_subkey();
                if (score_to_remove != score_to_add) {
                  should_remove_existing_entry = true;
                  if (request_.set_request().sorted_set_options().ch()) {
                    return_value++;
                  }
                }
                break;
              }
              default: {
                // NX option calls for only new elements, set should_add_entry to false.
                should_add_entry = false;
                break;
              }
            }
          }

          if (should_remove_existing_entry) {
            double score_to_remove = subdoc_reverse.GetDouble();
            SubDocument subdoc_forward_tombstone;
            subdoc_forward_tombstone.SetChild(PrimitiveValue(kv.value(i)),
                                              SubDocument(ValueType::kTombstone));
            kv_entries_forward.SetChild(PrimitiveValue::Double(score_to_remove),
                                        SubDocument(subdoc_forward_tombstone));
          }

          if (should_add_entry) {
            // If the incr option is specified, we need insert the existing score + new score
            // instead of just the new score.
            double score_to_add = request_.set_request().sorted_set_options().incr() ?
                kv.subkey(i).double_subkey() + subdoc_reverse.GetDouble() :
                kv.subkey(i).double_subkey();

            // Add the forward mapping to the entries.
            SubDocument *forward_entry =
                kv_entries_forward.GetOrAddChild(PrimitiveValue::Double(score_to_add)).first;
            forward_entry->SetChild(PrimitiveValue(kv.value(i)),
                                    SubDocument(PrimitiveValue()));

            // Add the reverse mapping to the entries.
            kv_entries_reverse.SetChild(PrimitiveValue(kv.value(i)),
                                        SubDocument(PrimitiveValue::Double(score_to_add)));
          }
        }

        if (new_elements_added > 0) {
          int64_t card = VERIFY_RESULT(GetCardinality(iterator_.get(), kv));
          // Insert card + new_elements_added back into the document for the updated card.
          kv_entries_card = SubDocument(PrimitiveValue(card + new_elements_added));
          kv_entries.SetChild(PrimitiveValue(ValueType::kCounter), SubDocument(kv_entries_card));
        }

        if (kv_entries_forward.object_num_keys() > 0) {
          kv_entries.SetChild(PrimitiveValue(ValueType::kSSForward),
                              SubDocument(kv_entries_forward));
        }

        if (kv_entries_reverse.object_num_keys() > 0) {
          kv_entries.SetChild(PrimitiveValue(ValueType::kSSReverse),
                              SubDocument(kv_entries_reverse));
        }

        if (kv_entries.object_num_keys() > 0) {
          RETURN_NOT_OK(kv_entries.ConvertToRedisSortedSet());
          if (data_type == REDIS_TYPE_NONE) {
                RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
                    doc_path, kv_entries, data.read_time, data.deadline, redis_query_id(), ttl));
          } else {
                RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
                    doc_path, kv_entries, data.read_time, data.deadline, redis_query_id(), ttl));
          }
        }
        response_.set_code(RedisResponsePB::OK);
        response_.set_int_response(return_value);
        break;
    }
    case REDIS_TYPE_STRING: {
        return STATUS_SUBSTITUTE(InvalidCommand,
            "Redis data type $0 in SET command should not have subkeys", kv.type());
      }
      default:
        return STATUS_SUBSTITUTE(InvalidCommand,
            "Redis data type $0 not supported in SET command", kv.type());
    }
  } else {
    if (kv.type() != REDIS_TYPE_STRING) {
      return STATUS_SUBSTITUTE(InvalidCommand,
          "Redis data type for SET must be string if subkey not present, found $0", kv.type());
    }
    if (kv.value_size() != 1) {
      return STATUS_SUBSTITUTE(InvalidCommand,
          "There must be only one value in SET if there is only one key, found $0",
          kv.value_size());
    }
    const RedisWriteMode mode = request_.set_request().mode();
    if (mode != RedisWriteMode::REDIS_WRITEMODE_UPSERT) {
      RedisDataType data_type = VERIFY_RESULT(GetValueType(data));
      if ((mode == RedisWriteMode::REDIS_WRITEMODE_INSERT && data_type != REDIS_TYPE_NONE)
          || (mode == RedisWriteMode::REDIS_WRITEMODE_UPDATE && data_type == REDIS_TYPE_NONE)) {

        if (request_.set_request().has_expect_ok_response() &&
            !request_.set_request().expect_ok_response()) {
          // For SETNX we return 0 or 1 depending on if the key already existed.
          response_.set_int_response(0);
          response_.set_code(RedisResponsePB::OK);
        } else {
          response_.set_code(RedisResponsePB::NIL);
        }
        return Status::OK();
      }
    }
    RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
        doc_path, Value(PrimitiveValue(kv.value(0)), ttl),
        data.read_time, data.deadline, redis_query_id()));
  }

  if (request_.set_request().has_expect_ok_response() &&
      !request_.set_request().expect_ok_response()) {
    // For SETNX we return 0 or 1 depending on if the key already existed.
    response_.set_int_response(1);
  }

  response_.set_code(RedisResponsePB::OK);
  return Status::OK();
}

Status RedisWriteOperation::ApplySetTtl(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();

  // We only support setting TTLs on top-level keys.
  if (!kv.subkey().empty()) {
    return STATUS_SUBSTITUTE(Corruption,
                             "Expected no subkeys, got $0", kv.subkey().size());
  }

  MonoDelta ttl;
  bool absolute_expiration = request_.set_ttl_request().has_absolute_time();

  // Handle ExpireAt
  if (absolute_expiration) {
    int64_t calc_ttl = request_.set_ttl_request().absolute_time() -
      server::HybridClock::GetPhysicalValueNanos(data.read_time.read) /
      MonoTime::kNanosecondsPerMillisecond;
    if (calc_ttl <= 0) {
      return ApplyDel(data);
    }
    ttl = MonoDelta::FromMilliseconds(calc_ttl);
  }

  Expiration exp;
  auto value = VERIFY_RESULT(GetValue(data, kNilSubkeyIndex, &exp));

  if (value.type == REDIS_TYPE_TIMESERIES) { // This command is not supported.
    return STATUS_SUBSTITUTE(InvalidCommand,
        "Redis data type $0 not supported in EXPIRE and PERSIST commands", value.type);
  }

  if (value.type == REDIS_TYPE_NONE) { // Key does not exist.
    VLOG(1) << "TTL cannot be set because the key does not exist";
    response_.set_int_response(0);
    return Status::OK();
  }

  if (!absolute_expiration && request_.set_ttl_request().ttl() == -1) { // Handle PERSIST.
    MonoDelta new_ttl = VERIFY_RESULT(exp.ComputeRelativeTtl(iterator_->read_time().read));
    if (new_ttl.IsNegative() || new_ttl == Value::kMaxTtl) {
      response_.set_int_response(0);
      return Status::OK();
    }
  }

  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  if (!absolute_expiration) {
    ttl = request_.set_ttl_request().ttl() == -1 ? Value::kMaxTtl :
      MonoDelta::FromMilliseconds(request_.set_ttl_request().ttl());
  }

  ValueType v_type = ValueTypeFromRedisType(value.type);
  if (v_type == ValueType::kInvalid)
    return STATUS(Corruption, "Invalid value type.");

  RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
      doc_path, Value(PrimitiveValue(v_type), ttl, Value::kInvalidUserTimestamp, Value::kTtlFlag),
      data.read_time, data.deadline, redis_query_id()));
  VLOG(2) << "Set TTL successfully to " << ttl << " for key " << kv.key();
  response_.set_int_response(1);
  return Status::OK();
}

Status RedisWriteOperation::ApplyGetSet(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();

  if (kv.value_size() != 1) {
    return STATUS_SUBSTITUTE(Corruption,
        "Getset kv should have 1 value, found $0", kv.value_size());
  }

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
      VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }
  response_.set_string_response(value->value);

  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()),
      Value(PrimitiveValue(kv.value(0))), data.read_time, data.deadline, redis_query_id());
}

Status RedisWriteOperation::ApplyAppend(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();

  if (kv.value_size() != 1) {
    return STATUS_SUBSTITUTE(Corruption,
        "Append kv should have 1 value, found $0", kv.value_size());
  }

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
                            VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  value->value += kv.value(0);

  response_.set_code(RedisResponsePB::OK);
  response_.set_int_response(value->value.length());

  // TODO: update the TTL with the write time rather than read time,
  // or store the expiration.
  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()),
      Value(PrimitiveValue(value->value),
            VERIFY_RESULT(value->exp.ComputeRelativeTtl(iterator_->read_time().read))),
      data.read_time,
      data.deadline,
      redis_query_id());
}

// TODO (akashnil): Actually check if the value existed, return 0 if not. handle multidel in future.
//                  See ENG-807
Status RedisWriteOperation::ApplyDel(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  RedisDataType data_type = VERIFY_RESULT(GetValueType(data));
  if (data_type != REDIS_TYPE_NONE && data_type != kv.type() && kv.type() != REDIS_TYPE_NONE) {
    response_.set_code(RedisResponsePB::WRONG_TYPE);
    response_.set_error_message(wrong_type_message);
    return Status::OK();
  }

  SubDocument values;
  // Number of distinct keys being removed.
  int num_keys = 0;
  switch (kv.type()) {
    case REDIS_TYPE_NONE: {
      values = SubDocument(ValueType::kTombstone);
      num_keys = data_type == REDIS_TYPE_NONE ? 0 : 1;
      break;
    }
    case REDIS_TYPE_TIMESERIES: {
      if (data_type == REDIS_TYPE_NONE) {
        return Status::OK();
      }
      for (int i = 0; i < kv.subkey_size(); i++) {
        PrimitiveValue primitive_value;
        RETURN_NOT_OK(PrimitiveValueFromSubKeyStrict(kv.subkey(i), data_type, &primitive_value));
        values.SetChild(primitive_value, SubDocument(ValueType::kTombstone));
      }
      num_keys = kv.subkey_size();
      break;
    }
    case REDIS_TYPE_SORTEDSET: {
      SubDocument values_card;
      SubDocument values_forward;
      SubDocument values_reverse;
      num_keys = kv.subkey_size();
      for (int i = 0; i < kv.subkey_size(); i++) {
        // Check whether the value is already in the document.
        SubDocument doc_reverse;
        bool doc_reverse_found = false;
        SubDocKey subdoc_key_reverse = SubDocKey(DocKey::FromRedisKey(kv.hash_code(), kv.key()),
                                                 PrimitiveValue(ValueType::kSSReverse),
                                                 PrimitiveValue(kv.subkey(i).string_subkey()));
        // Todo(Rahul): Add values to the write batch cache and then do an additional check.
        // As of now, we only check to see if a value is in rocksdb, and we should also check
        // the write batch.
        auto encoded_subdoc_key_reverse = subdoc_key_reverse.EncodeWithoutHt();
        GetSubDocumentData get_data = { encoded_subdoc_key_reverse, &doc_reverse,
                                        &doc_reverse_found };
        RETURN_NOT_OK(GetSubDocument(
            data.doc_write_batch->doc_db(),
            get_data, redis_query_id(), boost::none /* txn_op_context */, data.deadline,
            data.read_time));
        if (doc_reverse_found && doc_reverse.value_type() != ValueType::kTombstone) {
          // The value is already in the doc, needs to be removed.
          values_reverse.SetChild(PrimitiveValue(kv.subkey(i).string_subkey()),
                          SubDocument(ValueType::kTombstone));
          // For sorted sets, the forward mapping also needs to be deleted.
          SubDocument doc_forward;
          doc_forward.SetChild(PrimitiveValue(kv.subkey(i).string_subkey()),
                               SubDocument(ValueType::kTombstone));
          values_forward.SetChild(PrimitiveValue::Double(doc_reverse.GetDouble()),
                          SubDocument(doc_forward));
        } else {
          // If the key is absent, it doesn't contribute to the count of keys being deleted.
          num_keys--;
        }
      }
      int64_t card = VERIFY_RESULT(GetCardinality(iterator_.get(), kv));
      // The new cardinality is card - num_keys.
      values_card = SubDocument(PrimitiveValue(card - num_keys));

      values.SetChild(PrimitiveValue(ValueType::kCounter), SubDocument(values_card));
      values.SetChild(PrimitiveValue(ValueType::kSSForward), SubDocument(values_forward));
      values.SetChild(PrimitiveValue(ValueType::kSSReverse), SubDocument(values_reverse));

      break;
    }
    default: {
      num_keys = kv.subkey_size(); // We know the subkeys are distinct.
      // Avoid reads for redis timeseries type.
      if (EmulateRedisResponse(kv.type())) {
        for (int i = 0; i < kv.subkey_size(); i++) {
          RedisDataType type = VERIFY_RESULT(GetValueType(data, i));
          if (type == REDIS_TYPE_STRING) {
            values.SetChild(PrimitiveValue(kv.subkey(i).string_subkey()),
                            SubDocument(ValueType::kTombstone));
          } else {
            // If the key is absent, it doesn't contribute to the count of keys being deleted.
            num_keys--;
          }
        }
      }
      break;
    }
  }

  if (num_keys != 0) {
    DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
    RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(doc_path, values,
        data.read_time, data.deadline, redis_query_id()));
  }

  response_.set_code(RedisResponsePB::OK);
  if (EmulateRedisResponse(kv.type())) {
    // If the flag is true, we respond with the number of keys actually being deleted. We don't
    // report this number for the redis timeseries type to avoid reads.
    response_.set_int_response(num_keys);
  }
  return Status::OK();
}

Status RedisWriteOperation::ApplySetRange(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  if (kv.value_size() != 1) {
    return STATUS_SUBSTITUTE(Corruption,
        "SetRange kv should have 1 value, found $0", kv.value_size());
  }

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
        VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  if (request_.set_range_request().offset() > value->value.length()) {
    value->value.resize(request_.set_range_request().offset(), 0);
  }
  value->value.replace(request_.set_range_request().offset(), kv.value(0).length(), kv.value(0));
  response_.set_code(RedisResponsePB::OK);
  response_.set_int_response(value->value.length());

  // TODO: update the TTL with the write time rather than read time,
  // or store the expiration.
  Value new_val = Value(PrimitiveValue(value->value),
        VERIFY_RESULT(value->exp.ComputeRelativeTtl(iterator_->read_time().read)));
  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()), new_val, std::move(iterator_));
}

Status RedisWriteOperation::ApplyIncr(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  const int64_t incr = request_.incr_request().increment_int();

  if (kv.type() != REDIS_TYPE_HASH && kv.type() != REDIS_TYPE_STRING) {
    return STATUS_SUBSTITUTE(InvalidCommand,
                             "Redis data type $0 not supported in Incr command", kv.type());
  }

  RedisDataType container_type = VERIFY_RESULT(GetValueType(data));
  if (!VerifyTypeAndSetCode(kv.type(), container_type, &response_,
                            VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  int subkey = (kv.type() == REDIS_TYPE_HASH ? 0 : -1);
  auto value = GetValue(data, subkey);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
      VerifySuccessIfMissing::kTrue)) {
    response_.set_code(RedisResponsePB::WRONG_TYPE);
    response_.set_error_message(wrong_type_message);
    return Status::OK();
  }

  // If no value is present, 0 is the default.
  int64_t old_value = 0, new_value;
  if (value->type != REDIS_TYPE_NONE) {
    auto old = CheckedStoll(value->value);
    if (!old.ok()) {
      // This can happen if there are leading or trailing spaces, or the value
      // is out of range.
      response_.set_code(RedisResponsePB::WRONG_TYPE);
      response_.set_error_message("ERR value is not an integer or out of range");
      return Status::OK();
    }
    old_value = *old;
  }

  if ((incr < 0 && old_value < 0 && incr < numeric_limits<int64_t>::min() - old_value) ||
      (incr > 0 && old_value > 0 && incr > numeric_limits<int64_t>::max() - old_value)) {
    response_.set_code(RedisResponsePB::WRONG_TYPE);
    response_.set_error_message("Increment would overflow");
    return Status::OK();
  }
  new_value = old_value + incr;
  response_.set_code(RedisResponsePB::OK);
  response_.set_int_response(new_value);

  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  PrimitiveValue new_pvalue = PrimitiveValue(std::to_string(new_value));
  if (kv.type() == REDIS_TYPE_HASH) {
    SubDocument kv_entries = SubDocument();
    PrimitiveValue subkey_value;
    RETURN_NOT_OK(PrimitiveValueFromSubKeyStrict(kv.subkey(0), kv.type(), &subkey_value));
    kv_entries.SetChild(subkey_value, SubDocument(new_pvalue));
    return data.doc_write_batch->ExtendSubDocument(
        doc_path, kv_entries, data.read_time, data.deadline, redis_query_id());
  } else {  // kv.type() == REDIS_TYPE_STRING
    // TODO: update the TTL with the write time rather than read time,
    // or store the expiration.
    Value new_val = Value(new_pvalue,
        VERIFY_RESULT(value->exp.ComputeRelativeTtl(iterator_->read_time().read)));
    return data.doc_write_batch->SetPrimitive(doc_path, new_val, std::move(iterator_));
  }
}

Status RedisWriteOperation::ApplyPush(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  RedisDataType data_type = VERIFY_RESULT(GetValueType(data));
  if (data_type != REDIS_TYPE_LIST && data_type != REDIS_TYPE_NONE) {
    response_.set_code(RedisResponsePB::WRONG_TYPE);
    response_.set_error_message(wrong_type_message);
    return Status::OK();
  }

  SubDocument list;
  int64_t card = VERIFY_RESULT(GetCardinality(iterator_.get(), kv)) + kv.value_size();
  list.SetChild(PrimitiveValue(ValueType::kCounter), SubDocument(PrimitiveValue(card)));

  SubDocument elements(request_.push_request().side() == REDIS_SIDE_LEFT ?
                   ListExtendOrder::PREPEND : ListExtendOrder::APPEND);
  for (auto val : kv.value()) {
    elements.AddListElement(SubDocument(PrimitiveValue(val)));
  }
  list.SetChild(PrimitiveValue(ValueType::kArray), std::move(elements));
  RETURN_NOT_OK(list.ConvertToRedisList());

  if (data_type == REDIS_TYPE_NONE) {
    RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
        DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()), list,
        data.read_time, data.deadline, redis_query_id()));
  } else {
    RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
        DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()), list,
        data.read_time, data.deadline, redis_query_id()));
  }

  response_.set_int_response(card);
  response_.set_code(RedisResponsePB::OK);
  return Status::OK();
}

Status RedisWriteOperation::ApplyInsert(const DocOperationApplyData& data) {
  return STATUS(NotSupported, "Redis operation has not been implemented");
}

Status RedisWriteOperation::ApplyPop(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  RedisDataType data_type = VERIFY_RESULT(GetValueType(data));

  if (!VerifyTypeAndSetCode(kv.type(), data_type, &response_, VerifySuccessIfMissing::kTrue)) {
    // We already set the error code in the function.
    return Status::OK();
  }

  SubDocument list;
  int64_t card = VERIFY_RESULT(GetCardinality(iterator_.get(), kv));

  if (!card) {
    response_.set_code(RedisResponsePB::NIL);
    return Status::OK();
  }

  std::vector<int> indices;
  std::vector<SubDocument> new_value = {SubDocument(PrimitiveValue(ValueType::kTombstone))};
  std::vector<std::string> value;

  if (request_.pop_request().side() == REDIS_SIDE_LEFT) {
    indices.push_back(1);
    RETURN_NOT_OK(data.doc_write_batch->ReplaceInList(doc_path, indices, new_value,
        data.read_time, data.deadline, redis_query_id(), Direction::kForward, 0, &value));
  } else {
    indices.push_back(card);
    RETURN_NOT_OK(data.doc_write_batch->ReplaceInList(doc_path, indices, new_value,
        data.read_time, data.deadline, redis_query_id(), Direction::kBackward, card + 1, &value));
  }

  list.SetChild(PrimitiveValue(ValueType::kCounter), SubDocument(PrimitiveValue(--card)));
  RETURN_NOT_OK(list.ConvertToRedisList());
  RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
        doc_path, list, data.read_time, data.deadline, redis_query_id()));

  if (value.size() != 1)
    return STATUS_SUBSTITUTE(Corruption,
                             "Expected one popped value, got $0", value.size());

  response_.set_string_response(value[0]);
  response_.set_code(RedisResponsePB::OK);
  return Status::OK();
}

Status RedisWriteOperation::ApplyAdd(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  RedisDataType data_type = VERIFY_RESULT(GetValueType(data));

  if (data_type != REDIS_TYPE_SET && data_type != REDIS_TYPE_NONE) {
    response_.set_code(RedisResponsePB::WRONG_TYPE);
    response_.set_error_message(wrong_type_message);
    return Status::OK();
  }

  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());

  if (kv.subkey_size() == 0) {
    return STATUS(InvalidCommand, "SADD request has no subkeys set");
  }

  int num_keys_found = 0;

  SubDocument set_entries = SubDocument();

  for (int i = 0 ; i < kv.subkey_size(); i++) { // We know that each subkey is distinct.
    if (FLAGS_emulate_redis_responses) {
      RedisDataType type = VERIFY_RESULT(GetValueType(data, i));
      if (type != REDIS_TYPE_NONE) {
        num_keys_found++;
      }
    }

    set_entries.SetChild(
        PrimitiveValue(kv.subkey(i).string_subkey()),
        SubDocument(PrimitiveValue(ValueType::kNullLow)));
  }

  RETURN_NOT_OK(set_entries.ConvertToRedisSet());

  Status s;

  if (data_type == REDIS_TYPE_NONE) {
    RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
        doc_path, set_entries, data.read_time, data.deadline, redis_query_id()));
  } else {
    RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
        doc_path, set_entries, data.read_time, data.deadline, redis_query_id()));
  }

  response_.set_code(RedisResponsePB::OK);
  if (FLAGS_emulate_redis_responses) {
    // If flag is set, the actual number of new keys added is sent as response.
    response_.set_int_response(kv.subkey_size() - num_keys_found);
  }
  return Status::OK();
}

Status RedisWriteOperation::ApplyRemove(const DocOperationApplyData& data) {
  return STATUS(NotSupported, "Redis operation has not been implemented");
}

Status RedisReadOperation::Execute() {
  SimulateTimeoutIfTesting(&deadline_);
  // If we have a KEYS command, we don't specify any key for the iterator. Therefore, don't use
  // bloom filters for this command.
  SubDocKey doc_key(
      DocKey::FromRedisKey(request_.key_value().hash_code(), request_.key_value().key()));
  auto bloom_filter_mode = request_.has_keys_request() ?
      BloomFilterMode::DONT_USE_BLOOM_FILTER : BloomFilterMode::USE_BLOOM_FILTER;
  auto iter = yb::docdb::CreateIntentAwareIterator(
      doc_db_, bloom_filter_mode,
      doc_key.Encode().AsSlice(),
      redis_query_id(), /* txn_op_context */ boost::none, deadline_, read_time_);
  iterator_ = std::move(iter);
  deadline_info_.emplace(deadline_);

  switch (request_.request_case()) {
    case RedisReadRequestPB::kGetForRenameRequest:
      return ExecuteGetForRename();
    case RedisReadRequestPB::kGetRequest:
      return ExecuteGet();
    case RedisReadRequestPB::kGetTtlRequest:
      return ExecuteGetTtl();
    case RedisReadRequestPB::kStrlenRequest:
      return ExecuteStrLen();
    case RedisReadRequestPB::kExistsRequest:
      return ExecuteExists();
    case RedisReadRequestPB::kGetRangeRequest:
      return ExecuteGetRange();
    case RedisReadRequestPB::kGetCollectionRangeRequest:
      return ExecuteCollectionGetRange();
    case RedisReadRequestPB::kKeysRequest:
      return ExecuteKeys();
    default:
      return STATUS_FORMAT(
          Corruption, "Unsupported redis read operation: $0", request_.request_case());
  }
}

int RedisReadOperation::ApplyIndex(int32_t index, const int32_t len) {
  if (index < 0) index += len;
  if (index < 0) index = 0;
  if (index > len) index = len;
  return index;
}

Status RedisReadOperation::ExecuteHGetAllLikeCommands(ValueType value_type,
                                                      bool add_keys,
                                                      bool add_values) {
  SubDocKey doc_key(
      DocKey::FromRedisKey(request_.key_value().hash_code(), request_.key_value().key()));
  SubDocument doc;
  bool doc_found = false;
  auto encoded_doc_key = doc_key.EncodeWithoutHt();

  // TODO(dtxn) - pass correct transaction context when we implement cross-shard transactions
  // support for Redis.
  GetSubDocumentData data = { encoded_doc_key, &doc, &doc_found };
  data.deadline_info = deadline_info_.get_ptr();

  bool has_cardinality_subkey = value_type == ValueType::kRedisSortedSet ||
                                value_type == ValueType::kRedisList;
  bool return_array_response = add_keys || add_values;

  if (has_cardinality_subkey) {
    data.return_type_only = !return_array_response;
  } else {
    data.count_only = !return_array_response;
  }

  RETURN_NOT_OK(GetSubDocument(iterator_.get(), data, /* projection */ nullptr,
                               SeekFwdSuffices::kFalse));
  if (return_array_response)
    response_.set_allocated_array_response(new RedisArrayPB());

  if (!doc_found) {
    response_.set_code(RedisResponsePB::OK);
    if (!return_array_response)
      response_.set_int_response(0);
    return Status::OK();
  }

  if (VerifyTypeAndSetCode(value_type, doc.value_type(), &response_)) {
    if (return_array_response) {
      RETURN_NOT_OK(PopulateResponseFrom(doc.object_container(), AddResponseValuesGeneric,
                                         &response_, add_keys, add_values));
    } else {
      int64_t card = has_cardinality_subkey ?
        VERIFY_RESULT(GetCardinality(iterator_.get(), request_.key_value())) :
        data.record_count;
      response_.set_int_response(card);
      response_.set_code(RedisResponsePB::OK);
    }
  }
  return Status::OK();
}

Status RedisReadOperation::ExecuteCollectionGetRangeByBounds(
    RedisCollectionGetRangeRequestPB::GetRangeRequestType request_type, bool add_keys) {
  RedisSubKeyBoundPB lower_bound;
  lower_bound.set_infinity_type(RedisSubKeyBoundPB::NEGATIVE);
  RedisSubKeyBoundPB upper_bound;
  upper_bound.set_infinity_type(RedisSubKeyBoundPB::POSITIVE);
  return ExecuteCollectionGetRangeByBounds(request_type, lower_bound, upper_bound, add_keys);
}

Status RedisReadOperation::ExecuteCollectionGetRangeByBounds(
    RedisCollectionGetRangeRequestPB::GetRangeRequestType request_type,
    const RedisSubKeyBoundPB& lower_bound, const RedisSubKeyBoundPB& upper_bound, bool add_keys) {
  if ((lower_bound.has_infinity_type() &&
       lower_bound.infinity_type() == RedisSubKeyBoundPB::POSITIVE) ||
      (upper_bound.has_infinity_type() &&
       upper_bound.infinity_type() == RedisSubKeyBoundPB::NEGATIVE)) {
    // Return empty response.
    response_.set_code(RedisResponsePB::OK);
    RETURN_NOT_OK(PopulateResponseFrom(
        SubDocument::ObjectContainer(), AddResponseValuesGeneric, &response_, /* add_keys */ true,
        /* add_values */ true));
    return Status::OK();
  }

  if (request_type == RedisCollectionGetRangeRequestPB::ZRANGEBYSCORE) {
    auto type = VERIFY_RESULT(GetValueType());
    auto expected_type = REDIS_TYPE_SORTEDSET;
    if (!VerifyTypeAndSetCode(expected_type, type, &response_, VerifySuccessIfMissing::kTrue)) {
      return Status::OK();
    }
    auto encoded_doc_key =
        DocKey::EncodedFromRedisKey(request_.key_value().hash_code(), request_.key_value().key());
    PrimitiveValue(ValueType::kSSForward).AppendToKey(&encoded_doc_key);
    double low_double = lower_bound.subkey_bound().double_subkey();
    double high_double = upper_bound.subkey_bound().double_subkey();

    KeyBytes low_sub_key_bound;
    KeyBytes high_sub_key_bound;

    SliceKeyBound low_subkey;
    if (!lower_bound.has_infinity_type()) {
      low_sub_key_bound = encoded_doc_key;
      PrimitiveValue::Double(low_double).AppendToKey(&low_sub_key_bound);
      low_subkey = SliceKeyBound(low_sub_key_bound, LowerBound(lower_bound.is_exclusive()));
    }
    SliceKeyBound high_subkey;
    if (!upper_bound.has_infinity_type()) {
      high_sub_key_bound = encoded_doc_key;
      PrimitiveValue::Double(high_double).AppendToKey(&high_sub_key_bound);
      high_subkey = SliceKeyBound(high_sub_key_bound, UpperBound(upper_bound.is_exclusive()));
    }

    SubDocument doc;
    bool doc_found = false;
    GetSubDocumentData data = {encoded_doc_key, &doc, &doc_found};
    data.deadline_info = deadline_info_.get_ptr();
    data.low_subkey = &low_subkey;
    data.high_subkey = &high_subkey;

    IndexBound low_index;
    IndexBound high_index;
    if (request_.has_range_request_limit()) {
      int32_t offset = request_.index_range().lower_bound().index();
      int32_t limit = request_.range_request_limit();

      if (offset < 0 || limit == 0) {
        // Return an empty response.
        response_.set_code(RedisResponsePB::OK);
        RETURN_NOT_OK(PopulateResponseFrom(
            SubDocument::ObjectContainer(), AddResponseValuesGeneric, &response_,
            /* add_keys */ true, /* add_values */ true));
        return Status::OK();
      }

      low_index = IndexBound(offset, true /* is_lower */);
      data.low_index = &low_index;
      if (limit > 0) {
        // Only define upper bound if limit is positive.
        high_index = IndexBound(offset + limit - 1, false /* is_lower */);
        data.high_index = &high_index;
      }
    }
    RETURN_NOT_OK(GetAndPopulateResponseValues(
        iterator_.get(), AddResponseValuesSortedSets, data, ValueType::kObject, request_,
        &response_,
        /* add_keys */ add_keys, /* add_values */ true, /* reverse */ false));
  } else {
    auto encoded_doc_key =
        DocKey::EncodedFromRedisKey(request_.key_value().hash_code(), request_.key_value().key());
    int64_t low_timestamp = lower_bound.subkey_bound().timestamp_subkey();
    int64_t high_timestamp = upper_bound.subkey_bound().timestamp_subkey();

    KeyBytes low_sub_key_bound;
    KeyBytes high_sub_key_bound;

    SliceKeyBound low_subkey;
    // Need to switch the order since we store the timestamps in descending order.
    if (!upper_bound.has_infinity_type()) {
      low_sub_key_bound = encoded_doc_key;
      PrimitiveValue(high_timestamp, SortOrder::kDescending).AppendToKey(&low_sub_key_bound);
      low_subkey = SliceKeyBound(low_sub_key_bound, LowerBound(upper_bound.is_exclusive()));
    }
    SliceKeyBound high_subkey;
    if (!lower_bound.has_infinity_type()) {
      high_sub_key_bound = encoded_doc_key;
      PrimitiveValue(low_timestamp, SortOrder::kDescending).AppendToKey(&high_sub_key_bound);
      high_subkey = SliceKeyBound(high_sub_key_bound, UpperBound(lower_bound.is_exclusive()));
    }

    SubDocument doc;
    bool doc_found = false;
    GetSubDocumentData data = {encoded_doc_key, &doc, &doc_found};
    data.deadline_info = deadline_info_.get_ptr();
    data.low_subkey = &low_subkey;
    data.high_subkey = &high_subkey;
    data.limit = request_.range_request_limit();
    bool is_reverse = true;
    if (request_type == RedisCollectionGetRangeRequestPB::TSREVRANGEBYTIME) {
      // If reverse is false, newest element is the first element returned.
      is_reverse = false;
    }
    RETURN_NOT_OK(GetAndPopulateResponseValues(
        iterator_.get(), AddResponseValuesGeneric, data, ValueType::kRedisTS, request_, &response_,
        /* add_keys */ true, /* add_values */ true, is_reverse));
  }
  return Status::OK();
}

Status RedisReadOperation::ExecuteCollectionGetRange() {
  const RedisKeyValuePB& key_value = request_.key_value();
  if (!request_.has_key_value() || !key_value.has_key()) {
    return STATUS(InvalidArgument, "Need to specify the key");
  }

  const auto request_type = request_.get_collection_range_request().request_type();
  switch (request_type) {
    case RedisCollectionGetRangeRequestPB::TSREVRANGEBYTIME:
      FALLTHROUGH_INTENDED;
    case RedisCollectionGetRangeRequestPB::ZRANGEBYSCORE: FALLTHROUGH_INTENDED;
    case RedisCollectionGetRangeRequestPB::TSRANGEBYTIME: {
      if(!request_.has_subkey_range() || !request_.subkey_range().has_lower_bound() ||
          !request_.subkey_range().has_upper_bound()) {
        return STATUS(InvalidArgument, "Need to specify the subkey range");
      }
      const RedisSubKeyBoundPB& lower_bound = request_.subkey_range().lower_bound();
      const RedisSubKeyBoundPB& upper_bound = request_.subkey_range().upper_bound();
      const bool add_keys = request_.get_collection_range_request().with_scores();
      return ExecuteCollectionGetRangeByBounds(request_type, lower_bound, upper_bound, add_keys);
    }
    case RedisCollectionGetRangeRequestPB::ZRANGE: FALLTHROUGH_INTENDED;
    case RedisCollectionGetRangeRequestPB::ZREVRANGE: {
      if(!request_.has_index_range() || !request_.index_range().has_lower_bound() ||
          !request_.index_range().has_upper_bound()) {
        return STATUS(InvalidArgument, "Need to specify the index range");
      }

      // First make sure is of type sorted set or none.
      RedisDataType type = VERIFY_RESULT(GetValueType());
      auto expected_type = RedisDataType::REDIS_TYPE_SORTEDSET;
      if (!VerifyTypeAndSetCode(expected_type, type, &response_, VerifySuccessIfMissing::kTrue)) {
        return Status::OK();
      }

      int64_t card = VERIFY_RESULT(GetCardinality(iterator_.get(), request_.key_value()));

      const RedisIndexBoundPB& low_index_bound = request_.index_range().lower_bound();
      const RedisIndexBoundPB& high_index_bound = request_.index_range().upper_bound();

      int64 low_idx_normalized, high_idx_normalized;

      int64 low_idx = low_index_bound.index();
      int64 high_idx = high_index_bound.index();
      // Normalize the bounds to be positive and go from low to high index.
      bool reverse = false;
      if (request_type == RedisCollectionGetRangeRequestPB::ZREVRANGE) {
        reverse = true;
      }
      GetNormalizedBounds(
          low_idx, high_idx, card, reverse, &low_idx_normalized, &high_idx_normalized);

      if (high_idx_normalized < low_idx_normalized) {
        // Return empty response.
        response_.set_code(RedisResponsePB::OK);
        RETURN_NOT_OK(PopulateResponseFrom(SubDocument::ObjectContainer(),
                                           AddResponseValuesGeneric,
                                           &response_, /* add_keys */
                                           true, /* add_values */
                                           true));
        return Status::OK();
      }
      auto encoded_doc_key = DocKey::EncodedFromRedisKey(
          request_.key_value().hash_code(), request_.key_value().key());
      PrimitiveValue(ValueType::kSSForward).AppendToKey(&encoded_doc_key);

      bool add_keys = request_.get_collection_range_request().with_scores();

      IndexBound low_bound = IndexBound(low_idx_normalized, true /* is_lower */);
      IndexBound high_bound = IndexBound(high_idx_normalized, false /* is_lower */);

      SubDocument doc;
      bool doc_found = false;
      GetSubDocumentData data = { encoded_doc_key, &doc, &doc_found};
      data.deadline_info = deadline_info_.get_ptr();
      data.low_index = &low_bound;
      data.high_index = &high_bound;

      RETURN_NOT_OK(GetAndPopulateResponseValues(
          iterator_.get(), AddResponseValuesSortedSets, data, ValueType::kObject, request_,
          &response_, add_keys, /* add_values */ true, reverse));
      break;
    }
    case RedisCollectionGetRangeRequestPB::UNKNOWN:
      return STATUS(InvalidCommand, "Unknown Collection Get Range Request not supported");
  }

  return Status::OK();
}

Result<RedisDataType> RedisReadOperation::GetValueType(int subkey_index) {
  return GetRedisValueType(iterator_.get(), request_.key_value(),
                           nullptr /* doc_write_batch */, subkey_index);
}

Result<RedisValue> RedisReadOperation::GetOverrideValue(int subkey_index) {
  return GetRedisValue(iterator_.get(), request_.key_value(),
                       subkey_index, /* always_override */ true);
}

Result<RedisValue> RedisReadOperation::GetValue(int subkey_index) {
    return GetRedisValue(iterator_.get(), request_.key_value(), subkey_index);
}

namespace {

// Note: Do not use if also retrieving other value, as some work will be repeated.
// Assumes every value has a TTL, and the TTL is stored in the row with this key.
// Also observe that tombstone checking only works because we assume the key has
// no ancestors.
Result<boost::optional<Expiration>> GetTtl(
    const Slice& encoded_subdoc_key, IntentAwareIterator* iter) {
  auto dockey_size =
    VERIFY_RESULT(DocKey::EncodedSize(encoded_subdoc_key, DocKeyPart::kWholeDocKey));
  Slice key_slice(encoded_subdoc_key.data(), dockey_size);
  iter->Seek(key_slice);
  if (!iter->valid())
    return boost::none;
  auto key_data = VERIFY_RESULT(iter->FetchKey());
  if (!key_data.key.compare(key_slice)) {
    Value doc_value = Value(PrimitiveValue(ValueType::kInvalid));
    RETURN_NOT_OK(doc_value.Decode(iter->value()));
    if (doc_value.value_type() != ValueType::kTombstone) {
      return Expiration(key_data.write_time.hybrid_time(), doc_value.ttl());
    }
  }
  return boost::none;
}

} // namespace

Status RedisReadOperation::ExecuteGetTtl() {
  const RedisKeyValuePB& kv = request_.key_value();
  if (!kv.has_key()) {
    return STATUS(Corruption, "Expected KeyValuePB");
  }
  // We currently only support getting and setting TTL on top level keys.
  if (!kv.subkey().empty()) {
    return STATUS_SUBSTITUTE(Corruption,
                             "Expected no subkeys, got $0", kv.subkey().size());
  }

  auto encoded_doc_key = DocKey::EncodedFromRedisKey(kv.hash_code(), kv.key());
  auto maybe_ttl_exp = VERIFY_RESULT(GetTtl(encoded_doc_key.AsSlice(), iterator_.get()));

  if (!maybe_ttl_exp.has_value()) {
    response_.set_int_response(-2);
    return Status::OK();
  }

  auto exp = maybe_ttl_exp.get();
  if (exp.ttl.Equals(Value::kMaxTtl)) {
    response_.set_int_response(-1);
    return Status::OK();
  }

  MonoDelta ttl = VERIFY_RESULT(exp.ComputeRelativeTtl(iterator_->read_time().read));
  if (ttl.IsNegative()) {
    // The value has expired.
    response_.set_int_response(-2);
    return Status::OK();
  }

  response_.set_int_response(request_.get_ttl_request().return_seconds() ?
                             (int64_t) std::round(ttl.ToSeconds()) :
                             ttl.ToMilliseconds());
  return Status::OK();
}

Status RedisReadOperation::ExecuteGetForRename() {
  RedisDataType type = VERIFY_RESULT(GetValueType());
  response_.set_type(type);
  switch (type) {
    case RedisDataType::REDIS_TYPE_STRING: {
      return ExecuteGet(RedisGetRequestPB::GET);
    }

    case RedisDataType::REDIS_TYPE_HASH: {
      return ExecuteGet(RedisGetRequestPB::HGETALL);
    }

    case RedisDataType::REDIS_TYPE_SET: {
      return ExecuteGet(RedisGetRequestPB::SMEMBERS);
    }

    case RedisDataType::REDIS_TYPE_SORTEDSET: {
      return ExecuteCollectionGetRangeByBounds(
          RedisCollectionGetRangeRequestPB::ZRANGEBYSCORE, true);
    }

    case RedisDataType::REDIS_TYPE_TIMESERIES: {
      return ExecuteCollectionGetRangeByBounds(
          RedisCollectionGetRangeRequestPB::TSRANGEBYTIME, true);
    }

    case RedisDataType::REDIS_TYPE_NONE: {
      response_.set_code(RedisResponsePB::NOT_FOUND);
      return Status::OK();
    }

    case RedisDataType::REDIS_TYPE_LIST:
    default: {
      LOG(DFATAL) << "Unhandled Redis Data Type " << type;
    }
  }
  return Status::OK();
}

Status RedisReadOperation::ExecuteGet(RedisGetRequestPB::GetRequestType type) {
  RedisGetRequestPB request;
  request.set_request_type(type);
  return ExecuteGet(request);
}

Status RedisReadOperation::ExecuteGet() { return ExecuteGet(request_.get_request()); }

Status RedisReadOperation::ExecuteGet(const RedisGetRequestPB& get_request) {
  auto request_type = get_request.request_type();
  RedisDataType expected_type = REDIS_TYPE_NONE;
  switch (request_type) {
    case RedisGetRequestPB::GET:
      expected_type = REDIS_TYPE_STRING; break;
    case RedisGetRequestPB::TSGET:
      expected_type = REDIS_TYPE_TIMESERIES; break;
    case RedisGetRequestPB::HGET: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB::HEXISTS:
      expected_type = REDIS_TYPE_HASH; break;
    case RedisGetRequestPB::SISMEMBER:
      expected_type = REDIS_TYPE_SET; break;
    case RedisGetRequestPB::ZSCORE:
      expected_type = REDIS_TYPE_SORTEDSET; break;
    default:
      expected_type = REDIS_TYPE_NONE;
  }
  switch (request_type) {
    case RedisGetRequestPB::GET: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB::TSGET: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB::HGET: {
      RedisDataType type = VERIFY_RESULT(GetValueType());
      // TODO: this is primarily glue for the Timeseries bug where the parent
      // may get compacted due to an outdated TTL even though the children
      // have longer TTL's and thus still exist. When fixing, take note that
      // GetValueType finds the value type of the parent, so if the parent
      // does not have the maximum TTL, it will return REDIS_TYPE_NONE when it
      // should not.
      if (expected_type == REDIS_TYPE_TIMESERIES && type == REDIS_TYPE_NONE) {
        type = expected_type;
      }
      // If wrong type, we set the error code in the response.
      if (VerifyTypeAndSetCode(expected_type, type, &response_, VerifySuccessIfMissing::kTrue)) {
        auto value = request_type == RedisGetRequestPB::TSGET ? GetOverrideValue() : GetValue();
        RETURN_NOT_OK(value);
        if (VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
            VerifySuccessIfMissing::kTrue)) {
          response_.set_string_response(value->value);
        }
      }
      return Status::OK();
    }
    case RedisGetRequestPB::ZSCORE: {
      RedisDataType type = VERIFY_RESULT(GetValueType());
      // If wrong type, we set the error code in the response.
      if (!VerifyTypeAndSetCode(expected_type, type, &response_, VerifySuccessIfMissing::kTrue)) {
        return Status::OK();
      }
      SubDocKey key_reverse = SubDocKey(
          DocKey::FromRedisKey(request_.key_value().hash_code(), request_.key_value().key()),
          PrimitiveValue(ValueType::kSSReverse),
          PrimitiveValue(request_.key_value().subkey(0).string_subkey()));
      SubDocument subdoc_reverse;
      bool subdoc_reverse_found = false;
      auto encoded_key_reverse = key_reverse.EncodeWithoutHt();
      GetSubDocumentData get_data = { encoded_key_reverse, &subdoc_reverse, &subdoc_reverse_found };
      RETURN_NOT_OK(GetSubDocument(doc_db_, get_data, redis_query_id(),
                                   boost::none /* txn_op_context */, deadline_, read_time_));
      if (subdoc_reverse_found) {
        double score = subdoc_reverse.GetDouble();
        response_.set_string_response(std::to_string(score));
      } else {
        response_.set_code(RedisResponsePB::NIL);
      }
      return Status::OK();
    }
    case RedisGetRequestPB::HEXISTS: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB::SISMEMBER: {
      RedisDataType type = VERIFY_RESULT(GetValueType());
      if (VerifyTypeAndSetCode(expected_type, type, &response_, VerifySuccessIfMissing::kTrue)) {
        RedisDataType subtype = VERIFY_RESULT(GetValueType(0));
        SetOptionalInt(subtype, 1, &response_);
        response_.set_code(RedisResponsePB::OK);
      }
      return Status::OK();
    }
    case RedisGetRequestPB::HSTRLEN: {
      RedisDataType type = VERIFY_RESULT(GetValueType());
      if (VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_HASH, type, &response_,
                               VerifySuccessIfMissing::kTrue)) {
        auto value = GetValue();
        RETURN_NOT_OK(value);
        SetOptionalInt(value->type, value->value.length(), &response_);
        response_.set_code(RedisResponsePB::OK);
      }
      return Status::OK();
    }
    case RedisGetRequestPB::MGET: {
      return STATUS(NotSupported, "MGET not yet supported");
    }
    case RedisGetRequestPB::HMGET: {
      RedisDataType type = VERIFY_RESULT(GetValueType());
      if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_HASH, type, &response_,
                                VerifySuccessIfMissing::kTrue)) {
        return Status::OK();
      }

      response_.set_allocated_array_response(new RedisArrayPB());
      const auto& req_kv = request_.key_value();
      size_t num_subkeys = req_kv.subkey_size();
      vector<int> indices(num_subkeys);
      for (int i = 0; i < num_subkeys; ++i) {
        indices[i] = i;
      }
      std::sort(indices.begin(), indices.end(), [&req_kv](int i, int j) {
            return req_kv.subkey(i).string_subkey() < req_kv.subkey(j).string_subkey();
          });

      string current_value = "";
      response_.mutable_array_response()->mutable_elements()->Reserve(num_subkeys);
      for (int i = 0; i < num_subkeys; ++i) {
        response_.mutable_array_response()->add_elements();
      }
      for (int i = 0; i < num_subkeys; ++i) {
        if (i == 0 ||
            req_kv.subkey(indices[i]).string_subkey() !=
            req_kv.subkey(indices[i - 1]).string_subkey()) {
          // If the condition above is false, we encountered the same key again, no need to call
          // GetValue() once more, current_value is already correct.
          auto value = GetValue(indices[i]);
          RETURN_NOT_OK(value);
          if (value->type == REDIS_TYPE_STRING) {
            current_value = std::move(value->value);
          } else {
            current_value = ""; // Empty string is nil response.
          }
        }
        *response_.mutable_array_response()->mutable_elements(indices[i]) = current_value;
      }

      response_.set_code(RedisResponsePB::OK);
      return Status::OK();
    }
    case RedisGetRequestPB::HGETALL:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, true, true);
    case RedisGetRequestPB::HKEYS:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, true, false);
    case RedisGetRequestPB::HVALS:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, false, true);
    case RedisGetRequestPB::HLEN:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, false, false);
    case RedisGetRequestPB::SMEMBERS:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisSet, true, false);
    case RedisGetRequestPB::SCARD:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisSet, false, false);
    case RedisGetRequestPB::TSCARD:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisTS, false, false);
    case RedisGetRequestPB::ZCARD:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisSortedSet, false, false);
    case RedisGetRequestPB::LLEN:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisList, false, false);
    case RedisGetRequestPB::UNKNOWN: {
      return STATUS(InvalidCommand, "Unknown Get Request not supported");
    }
  }
  return Status::OK();
}

Status RedisReadOperation::ExecuteStrLen() {
  auto value = GetValue();
  response_.set_code(RedisResponsePB::OK);
  RETURN_NOT_OK(value);

  if (VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
        VerifySuccessIfMissing::kTrue)) {
    SetOptionalInt(value->type, value->value.length(), &response_);
  }
  response_.set_code(RedisResponsePB::OK);

  return Status::OK();
}

Status RedisReadOperation::ExecuteExists() {
  auto value = GetValue();
  response_.set_code(RedisResponsePB::OK);
  RETURN_NOT_OK(value);

  // We only support exist command with one argument currently.
  SetOptionalInt(value->type, 1, &response_);
  response_.set_code(RedisResponsePB::OK);

  return Status::OK();
}

Status RedisReadOperation::ExecuteGetRange() {
  auto value = GetValue();
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
      VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  const int32_t len = value->value.length();
  int32_t exclusive_end = request_.get_range_request().end() + 1;
  if (exclusive_end == 0) {
    exclusive_end = len;
  }

  // We treat negative indices to refer backwards from the end of the string.
  const int32_t start = ApplyIndex(request_.get_range_request().start(), len);
  int32_t end = ApplyIndex(exclusive_end, len);
  if (end < start) {
    end = start;
  }

  response_.set_code(RedisResponsePB::OK);
  response_.set_string_response(value->value.c_str() + start, end - start);
  return Status::OK();
}

Status RedisReadOperation::ExecuteKeys() {
  iterator_->Seek(DocKey());
  int threshold = request_.keys_request().threshold();

  bool doc_found;
  SubDocument result;

  while (iterator_->valid()) {
    if (deadline_info_.get_ptr() && deadline_info_->CheckAndSetDeadlinePassed()) {
      return STATUS(Expired, "Deadline for query passed.");
    }
    auto key = VERIFY_RESULT(iterator_->FetchKey()).key;

    // Key could be invalidated because we could move iterator, so back it up.
    KeyBytes key_copy(key);
    key = key_copy.AsSlice();

    DocKey doc_key;
    RETURN_NOT_OK(doc_key.FullyDecodeFrom(key));
    const PrimitiveValue& key_primitive = doc_key.hashed_group().front();
    if (!key_primitive.IsString() ||
        !RedisUtil::RedisPatternMatch(request_.keys_request().pattern(),
                                     key_primitive.GetString(),
                                     false /* ignore_case */)) {
      iterator_->SeekOutOfSubDoc(key);
      continue;
    }

    GetSubDocumentData data = {key, &result, &doc_found};
    data.deadline_info = deadline_info_.get_ptr();
    data.return_type_only = true;
    RETURN_NOT_OK(GetSubDocument(iterator_.get(), data, /* projection */ nullptr,
                                 SeekFwdSuffices::kFalse));

    if (doc_found) {
      if (--threshold < 0) {
        response_.clear_array_response();
        response_.set_code(RedisResponsePB::SERVER_ERROR);
        response_.set_error_message("Too many keys in the database.");
        return Status::OK();
      }
      RETURN_NOT_OK(AddPrimitiveValueToResponseArray(key_primitive,
                                                     response_.mutable_array_response()));
    }
    iterator_->SeekOutOfSubDoc(key);
  }

  response_.set_code(RedisResponsePB::OK);
  return Status::OK();
}

const RedisResponsePB& RedisReadOperation::response() {
  return response_;
}

}  // namespace docdb
}  // namespace yb
