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

#include "yb/docdb/doc_reader.h"

#include <string>
#include <vector>

#include "yb/common/hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/docdb/subdocument.h"
#include "yb/docdb/value.h"
#include "yb/docdb/value_type.h"
#include "yb/docdb/deadline_info.h"
#include "yb/docdb/docdb_types.h"

#include "yb/server/hybrid_clock.h"

#include "yb/util/status.h"

using std::vector;

using yb::HybridTime;

namespace yb {
namespace docdb {

// ------------------------------------------------------------------------------------------------
// Standalone functions
// ------------------------------------------------------------------------------------------------

namespace {

void SeekToLowerBound(const SliceKeyBound& lower_bound, IntentAwareIterator* iter) {
  if (lower_bound.is_exclusive()) {
    iter->SeekPastSubKey(lower_bound.key());
  } else {
    iter->SeekForward(lower_bound.key());
  }
}

// This function does not assume that object init_markers are present. If no init marker is present,
// or if a tombstone is found at some level, it still looks for subkeys inside it if they have
// larger timestamps.
//
// TODO(akashnil): ENG-1152: If object init markers were required, this read path may be optimized.
// We look at all rocksdb keys with prefix = subdocument_key, and construct a subdocument out of
// them, between the timestamp range high_ts and low_ts.
//
// The iterator is expected to be placed at the smallest key that is subdocument_key or later, and
// after the function returns, the iterator should be placed just completely outside the
// subdocument_key prefix. Although if high_subkey is specified, the iterator is only guaranteed
// to be positioned after the high_subkey and not necessarily outside the subdocument_key prefix.
// num_values_observed is used for queries on indices, and keeps track of the number of primitive
// values observed thus far. In a query with lower index bound k, ignore the first k primitive
// values before building the subdocument.
CHECKED_STATUS BuildSubDocument(
    IntentAwareIterator* iter,
    const GetSubDocumentData& data,
    DocHybridTime low_ts,
    int64* num_values_observed) {
  VLOG(3) << "BuildSubDocument data: " << data << " read_time: " << iter->read_time()
          << " low_ts: " << low_ts;
  while (iter->valid()) {
    if (data.deadline_info && data.deadline_info->CheckAndSetDeadlinePassed()) {
      return STATUS(Expired, "Deadline for query passed.");
    }
    // Since we modify num_values_observed on recursive calls, we keep a local copy of the value.
    int64 current_values_observed = *num_values_observed;
    auto key_data = VERIFY_RESULT(iter->FetchKey());
    auto key = key_data.key;
    const auto write_time = key_data.write_time;
    VLOG(4) << "iter: " << SubDocKey::DebugSliceToString(key)
            << ", key: " << SubDocKey::DebugSliceToString(data.subdocument_key);
    DCHECK(key.starts_with(data.subdocument_key))
        << "iter: " << SubDocKey::DebugSliceToString(key)
        << ", key: " << SubDocKey::DebugSliceToString(data.subdocument_key);

    // Key could be invalidated because we could move iterator, so back it up.
    KeyBytes key_copy(key);
    key = key_copy.AsSlice();
    rocksdb::Slice value = iter->value();
    // Checking that IntentAwareIterator returns an entry with correct time.
    DCHECK(key_data.same_transaction ||
           iter->read_time().global_limit >= write_time.hybrid_time())
        << "Bad key: " << SubDocKey::DebugSliceToString(key)
        << ", global limit: " << iter->read_time().global_limit
        << ", write time: " << write_time.hybrid_time();

    if (low_ts > write_time) {
      VLOG(3) << "SeekPastSubKey: " << SubDocKey::DebugSliceToString(key);
      iter->SeekPastSubKey(key);
      continue;
    }
    Value doc_value;
    RETURN_NOT_OK(doc_value.Decode(value));
    ValueType value_type = doc_value.value_type();
    if (key == data.subdocument_key) {
      if (write_time == DocHybridTime::kMin)
        return STATUS(Corruption, "No hybrid timestamp found on entry");

      // We may need to update the TTL in individual columns.
      if (write_time.hybrid_time() >= data.exp.write_ht) {
        // We want to keep the default TTL otherwise.
        if (doc_value.ttl() != Value::kMaxTtl) {
          data.exp.write_ht = write_time.hybrid_time();
          data.exp.ttl = doc_value.ttl();
        } else if (data.exp.ttl.IsNegative()) {
          data.exp.ttl = -data.exp.ttl;
        }
      }

      // If the hybrid time is kMin, then we must be using default TTL.
      if (data.exp.write_ht == HybridTime::kMin) {
        data.exp.write_ht = write_time.hybrid_time();
      }

      bool has_expired;
      CHECK_OK(HasExpiredTTL(data.exp.write_ht, data.exp.ttl,
                             iter->read_time().read, &has_expired));

      // Treat an expired value as a tombstone written at the same time as the original value.
      if (has_expired) {
        doc_value = Value::Tombstone();
        value_type = ValueType::kTombstone;
      }

      const bool is_collection = IsCollectionType(value_type);
      // We have found some key that matches our entire subdocument_key, i.e. we didn't skip ahead
      // to a lower level key (with optional object init markers).
      if (is_collection || value_type == ValueType::kTombstone) {
        if (low_ts < write_time) {
          low_ts = write_time;
        }
        if (is_collection) {
          *data.result = SubDocument(value_type);
        }

        // If the subkey lower bound filters out the key we found, we want to skip to the lower
        // bound. If it does not, we want to seek to the next key. This prevents an infinite loop
        // where the iterator keeps seeking to itself if the key we found matches the low subkey.
        // TODO: why are not we doing this for arrays?
        if (IsObjectType(value_type) && !data.low_subkey->CanInclude(key)) {
          // Try to seek to the low_subkey for efficiency.
          SeekToLowerBound(*data.low_subkey, iter);
        } else {
          VLOG(3) << "SeekPastSubKey: " << SubDocKey::DebugSliceToString(key);
          iter->SeekPastSubKey(key);
        }
        continue;
      } else if (IsPrimitiveValueType(value_type)) {
        // TODO: the ttl_seconds in primitive value is currently only in use for CQL. At some
        // point streamline by refactoring CQL to use the mutable Expiration in GetSubDocumentData.
        if (data.exp.ttl == Value::kMaxTtl) {
          doc_value.mutable_primitive_value()->SetTtl(-1);
        } else {
          int64_t time_since_write_seconds = (
              server::HybridClock::GetPhysicalValueMicros(iter->read_time().read) -
              server::HybridClock::GetPhysicalValueMicros(write_time.hybrid_time())) /
              MonoTime::kMicrosecondsPerSecond;
          int64_t ttl_seconds = std::max(static_cast<int64_t>(0),
              data.exp.ttl.ToMilliseconds() /
              MonoTime::kMillisecondsPerSecond - time_since_write_seconds);
          doc_value.mutable_primitive_value()->SetTtl(ttl_seconds);
        }
        // Choose the user supplied timestamp if present.
        const UserTimeMicros user_timestamp = doc_value.user_timestamp();
        doc_value.mutable_primitive_value()->SetWriteTime(
            user_timestamp == Value::kInvalidUserTimestamp
            ? write_time.hybrid_time().GetPhysicalValueMicros()
            : doc_value.user_timestamp());
        if (!data.high_index->CanInclude(current_values_observed)) {
          iter->SeekOutOfSubDoc(&key_copy);
          return Status::OK();
        }
        if (data.low_index->CanInclude(*num_values_observed)) {
          *data.result = SubDocument(doc_value.primitive_value());
        }
        (*num_values_observed)++;
        VLOG(3) << "SeekOutOfSubDoc: " << SubDocKey::DebugSliceToString(key);
        iter->SeekOutOfSubDoc(&key_copy);
        return Status::OK();
      } else {
        return STATUS_FORMAT(Corruption, "Expected primitive value type, got $0", value_type);
      }
    }
    SubDocument descendant{PrimitiveValue(ValueType::kInvalid)};
    // TODO: what if the key we found is the same as before?
    //       We'll get into an infinite recursion then.
    {
      IntentAwareIteratorPrefixScope prefix_scope(key, iter);
      RETURN_NOT_OK(BuildSubDocument(
          iter, data.Adjusted(key, &descendant), low_ts,
          num_values_observed));

    }
    if (descendant.value_type() == ValueType::kInvalid) {
      // The document was not found in this level (maybe a tombstone was encountered).
      continue;
    }

    if (!data.low_subkey->CanInclude(key)) {
      VLOG(3) << "Filtered by low_subkey: " << data.low_subkey->ToString()
              << ", key: " << SubDocKey::DebugSliceToString(key);
      // The value provided is lower than what we are looking for, seek to the lower bound.
      SeekToLowerBound(*data.low_subkey, iter);
      continue;
    }

    // We use num_values_observed as a conservative figure for lower bound and
    // current_values_observed for upper bound so we don't lose any data we should be including.
    if (!data.low_index->CanInclude(*num_values_observed)) {
      continue;
    }

    if (!data.high_subkey->CanInclude(key)) {
      VLOG(3) << "Filtered by high_subkey: " << data.high_subkey->ToString()
              << ", key: " << SubDocKey::DebugSliceToString(key);
      // We have encountered a subkey higher than our constraints, we should stop here.
      return Status::OK();
    }

    if (!data.high_index->CanInclude(current_values_observed)) {
      return Status::OK();
    }

    if (!IsObjectType(data.result->value_type())) {
      *data.result = SubDocument();
    }

    SubDocument* current = data.result;
    size_t num_children;
    RETURN_NOT_OK(current->NumChildren(&num_children));
    if (data.limit != 0 && num_children >= data.limit) {
      // We have processed enough records.
      return Status::OK();
    }

    if (data.count_only) {
      // We need to only count the records that we found.
      data.record_count++;
    } else {
      Slice temp = key;
      temp.remove_prefix(data.subdocument_key.size());
      for (;;) {
        PrimitiveValue child;
        RETURN_NOT_OK(child.DecodeFromKey(&temp));
        if (temp.empty()) {
          current->SetChild(child, std::move(descendant));
          break;
        }
        current = current->GetOrAddChild(child).first;
      }
    }
  }

  return Status::OK();
}

// If there is a key equal to key_bytes_without_ht + some timestamp, which is later than
// max_overwrite_time, we update max_overwrite_time, and result_value (unless it is nullptr).
// If there is a TTL with write time later than the write time in expiration, it is updated with
// the new write time and TTL, unless its value is kMaxTTL.
// When the TTL found is kMaxTTL and it is not a merge record, then it is assumed not to be
// explicitly set. Because it does not override the default table ttl, exp, which was initialized
// to the table ttl, is not updated.
// Observe that exp updates based on the first record found, while max_overwrite_time updates
// based on the first non-merge record found.
// This should not be used for leaf nodes. - Why? Looks like it is already used for leaf nodes
// also.
// Note: it is responsibility of caller to make sure key_bytes_without_ht doesn't have hybrid
// time.
// TODO: We could also check that the value is kTombStone or kObject type for sanity checking - ?
// It could be a simple value as well, not necessarily kTombstone or kObject.
Status FindLastWriteTime(
    IntentAwareIterator* iter,
    const Slice& key_without_ht,
    DocHybridTime* max_overwrite_time,
    Expiration* exp,
    Value* result_value = nullptr) {
  Slice value;
  DocHybridTime doc_ht = *max_overwrite_time;
  RETURN_NOT_OK(iter->FindLatestRecord(key_without_ht, &doc_ht, &value));
  if (!iter->valid()) {
    return Status::OK();
  }

  uint64_t merge_flags = 0;
  MonoDelta ttl;
  ValueType value_type;
  RETURN_NOT_OK(Value::DecodePrimitiveValueType(value, &value_type, &merge_flags, &ttl));
  if (value_type == ValueType::kInvalid) {
    return Status::OK();
  }

  // We update the expiration if and only if the write time is later than the write time
  // currently stored in expiration, and the record is not a regular record with default TTL.
  // This is done independently of whether the row is a TTL row.
  // In the case that the always_override flag is true, default TTL will not be preserved.
  Expiration new_exp = *exp;
  if (doc_ht.hybrid_time() >= exp->write_ht) {
    // We want to keep the default TTL otherwise.
    if (ttl != Value::kMaxTtl || merge_flags == Value::kTtlFlag || exp->always_override) {
      new_exp.write_ht = doc_ht.hybrid_time();
      new_exp.ttl = ttl;
    } else if (exp->ttl.IsNegative()) {
      new_exp.ttl = -new_exp.ttl;
    }
  }

  // If we encounter a TTL row, we assign max_overwrite_time to be the write time of the
  // original value/init marker.
  if (merge_flags == Value::kTtlFlag) {
    DocHybridTime new_ht;
    RETURN_NOT_OK(iter->NextFullValue(&new_ht, &value));

    // There could be a case where the TTL row exists, but the value has been
    // compacted away. Then, it is treated as a Tombstone written at the time
    // of the TTL row.
    if (!iter->valid() && !new_exp.ttl.IsNegative()) {
      new_exp.ttl = -new_exp.ttl;
    } else {
      ValueType value_type;
      RETURN_NOT_OK(Value::DecodePrimitiveValueType(value, &value_type));
      // Because we still do not know whether we are seeking something expired,
      // we must take the max_overwrite_time as if the value were not expired.
      doc_ht = new_ht;
    }
  }

  if ((value_type == ValueType::kTombstone || value_type == ValueType::kInvalid) &&
      !new_exp.ttl.IsNegative()) {
    new_exp.ttl = -new_exp.ttl;
  }
  *exp = new_exp;

  if (doc_ht > *max_overwrite_time) {
    *max_overwrite_time = doc_ht;
    VLOG(4) << "Max overwritten time for " << key_without_ht.ToDebugHexString() << ": "
            << *max_overwrite_time;
  }

  if (result_value)
    RETURN_NOT_OK(result_value->Decode(value));

  return Status::OK();
}

}  // namespace

yb::Status GetSubDocument(
    const DocDB& doc_db,
    const GetSubDocumentData& data,
    const rocksdb::QueryId query_id,
    const TransactionOperationContextOpt& txn_op_context,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time) {
  auto iter = CreateIntentAwareIterator(
      doc_db, BloomFilterMode::USE_BLOOM_FILTER, data.subdocument_key, query_id,
      txn_op_context, deadline, read_time);
  return GetSubDocument(iter.get(), data, nullptr /* projection */, SeekFwdSuffices::kFalse);
}

yb::Status GetSubDocument(
    IntentAwareIterator *db_iter,
    const GetSubDocumentData& data,
    const vector<PrimitiveValue>* projection,
    const SeekFwdSuffices seek_fwd_suffices) {
  // TODO(dtxn) scan through all involved transactions first to cache statuses in a batch,
  // so during building subdocument we don't need to request them one by one.
  // TODO(dtxn) we need to restart read with scan_ht = commit_ht if some transaction was committed
  // at time commit_ht within [scan_ht; read_request_time + max_clock_skew). Also we need
  // to wait until time scan_ht = commit_ht passed.
  // TODO(dtxn) for each scanned key (and its subkeys) we need to avoid *new* values committed at
  // ht <= scan_ht (or just ht < scan_ht?)
  // Question: what will break if we allow later commit at ht <= scan_ht ? Need to write down
  // detailed example.
  *data.doc_found = false;
  DOCDB_DEBUG_LOG("GetSubDocument for key $0 @ $1", data.subdocument_key.ToDebugHexString(),
                  db_iter->read_time().ToString());

  // The latest time at which any prefix of the given key was overwritten.
  DocHybridTime max_overwrite_ht(DocHybridTime::kMin);
  VLOG(4) << "GetSubDocument(" << data << ")";

  SubDocKey found_subdoc_key;
  auto dockey_size =
      VERIFY_RESULT(DocKey::EncodedSize(data.subdocument_key, DocKeyPart::kWholeDocKey));

  Slice key_slice(data.subdocument_key.data(), dockey_size);

  // Check ancestors for init markers, tombstones, and expiration, tracking the expiration and
  // corresponding most recent write time in exp, and the general most recent overwrite time in
  // max_overwrite_ht.
  //
  // First, check for an ancestor at the ID level: a table tombstone.  Currently, this is only
  // supported for YSQL colocated tables.  Since iterators only ever pertain to one table, there is
  // no need to create a prefix scope here.
  if (data.table_tombstone_time && *data.table_tombstone_time == DocHybridTime::kInvalid) {
    // Only check for table tombstones if the table is colocated, as signified by the prefix of
    // kPgTableOid.
    // TODO: adjust when fixing issue #3551
    if (key_slice[0] == ValueTypeAsChar::kPgTableOid) {
      // Seek to the ID level to look for a table tombstone.  Since this seek is expensive, cache
      // the result in data.table_tombstone_time to avoid double seeking for the lifetime of the
      // DocRowwiseIterator.
      DocKey empty_key;
      RETURN_NOT_OK(empty_key.DecodeFrom(key_slice, DocKeyPart::kUpToId));
      db_iter->Seek(empty_key);
      Value doc_value = Value(PrimitiveValue(ValueType::kInvalid));
      RETURN_NOT_OK(FindLastWriteTime(
          db_iter,
          empty_key.Encode(),
          &max_overwrite_ht,
          &data.exp,
          &doc_value));
      if (doc_value.value_type() == ValueType::kTombstone) {
        SCHECK_NE(max_overwrite_ht, DocHybridTime::kInvalid, Corruption,
                  "Invalid hybrid time for table tombstone");
        *data.table_tombstone_time = max_overwrite_ht;
      } else {
        *data.table_tombstone_time = DocHybridTime::kMin;
      }
    } else {
      *data.table_tombstone_time = DocHybridTime::kMin;
    }
  } else if (data.table_tombstone_time) {
    // Use the cached result.  Don't worry about exp as YSQL does not support TTL, yet.
    max_overwrite_ht = *data.table_tombstone_time;
  }
  // Second, check the descendants of the ID level.
  IntentAwareIteratorPrefixScope prefix_scope(key_slice, db_iter);
  if (seek_fwd_suffices) {
    db_iter->SeekForward(key_slice);
  } else {
    db_iter->Seek(key_slice);
  }
  {
    auto temp_key = data.subdocument_key;
    temp_key.remove_prefix(dockey_size);
    for (;;) {
      auto decode_result = VERIFY_RESULT(SubDocKey::DecodeSubkey(&temp_key));
      if (!decode_result) {
        break;
      }
      RETURN_NOT_OK(FindLastWriteTime(db_iter, key_slice, &max_overwrite_ht, &data.exp));
      key_slice = Slice(key_slice.data(), temp_key.data() - key_slice.data());
    }
  }

  // By this point, key_slice is the DocKey and all the subkeys of subdocument_key. Check for
  // init-marker / tombstones at the top level; update max_overwrite_ht.
  Value doc_value = Value(PrimitiveValue(ValueType::kInvalid));
  RETURN_NOT_OK(FindLastWriteTime(db_iter, key_slice, &max_overwrite_ht, &data.exp, &doc_value));

  const ValueType value_type = doc_value.value_type();

  if (data.return_type_only) {
    *data.doc_found = value_type != ValueType::kInvalid &&
      !data.exp.ttl.IsNegative();
    // Check for expiration.
    if (*data.doc_found && max_overwrite_ht != DocHybridTime::kMin) {
      bool has_expired;
      CHECK_OK(HasExpiredTTL(data.exp.write_ht, data.exp.ttl,
                             db_iter->read_time().read, &has_expired));
      *data.doc_found = !has_expired;
    }
    if (*data.doc_found) {
      // Observe that this will have the right type but not necessarily the right value.
      *data.result = SubDocument(doc_value.primitive_value());
    }
    return Status::OK();
  }

  if (projection == nullptr) {
    *data.result = SubDocument(ValueType::kInvalid);
    int64 num_values_observed = 0;
    IntentAwareIteratorPrefixScope prefix_scope(key_slice, db_iter);
    RETURN_NOT_OK(BuildSubDocument(db_iter, data, max_overwrite_ht,
                                   &num_values_observed));
    *data.doc_found = data.result->value_type() != ValueType::kInvalid;
    if (*data.doc_found) {
      if (value_type == ValueType::kRedisSet) {
        RETURN_NOT_OK(data.result->ConvertToRedisSet());
      } else if (value_type == ValueType::kRedisTS) {
        RETURN_NOT_OK(data.result->ConvertToRedisTS());
      } else if (value_type == ValueType::kRedisSortedSet) {
        RETURN_NOT_OK(data.result->ConvertToRedisSortedSet());
      } else if (value_type == ValueType::kRedisList) {
        RETURN_NOT_OK(data.result->ConvertToRedisList());
      }
    }
    return Status::OK();
  }
  // Seed key_bytes with the subdocument key. For each subkey in the projection, build subdocument
  // and reuse key_bytes while appending the subkey.
  *data.result = SubDocument();
  KeyBytes key_bytes;
  // Preallocate some extra space to avoid allocation for small subkeys.
  key_bytes.Reserve(data.subdocument_key.size() + kMaxBytesPerEncodedHybridTime + 32);
  key_bytes.AppendRawBytes(data.subdocument_key);
  const size_t subdocument_key_size = key_bytes.size();
  for (const PrimitiveValue& subkey : *projection) {
    // Append subkey to subdocument key. Reserve extra kMaxBytesPerEncodedHybridTime + 1 bytes in
    // key_bytes to avoid the internal buffer from getting reallocated and moved by SeekForward()
    // appending the hybrid time, thereby invalidating the buffer pointer saved by prefix_scope.
    subkey.AppendToKey(&key_bytes);
    key_bytes.Reserve(key_bytes.size() + kMaxBytesPerEncodedHybridTime + 1);
    // This seek is to initialize the iterator for BuildSubDocument call.
    IntentAwareIteratorPrefixScope prefix_scope(key_bytes, db_iter);
    db_iter->SeekForward(&key_bytes);
    SubDocument descendant(ValueType::kInvalid);
    int64 num_values_observed = 0;
    RETURN_NOT_OK(BuildSubDocument(
        db_iter, data.Adjusted(key_bytes, &descendant), max_overwrite_ht,
        &num_values_observed));
    *data.doc_found = descendant.value_type() != ValueType::kInvalid;
    data.result->SetChild(subkey, std::move(descendant));

    // Restore subdocument key by truncating the appended subkey.
    key_bytes.Truncate(subdocument_key_size);
  }
  // Make sure the iterator is placed outside the whole document in the end.
  key_bytes.Truncate(dockey_size);
  db_iter->SeekOutOfSubDoc(&key_bytes);
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
