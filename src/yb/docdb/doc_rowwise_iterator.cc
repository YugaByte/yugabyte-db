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

#include "yb/docdb/doc_rowwise_iterator.h"

#include "yb/common/partition.h"
#include "yb/common/transaction.h"
#include "yb/common/ql_expr.h"
#include "yb/common/ql_scanspec.h"
#include "yb/common/ql_value.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_ql_scanspec.h"
#include "yb/docdb/doc_reader.h"
#include "yb/docdb/doc_scanspec_util.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/docdb/subdocument.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksdb/db/compaction.h"
#include "yb/rocksutil/yb_rocksdb.h"

#include "yb/yql/pggate/util/pg_doc_data.h"

using std::string;

namespace yb {
namespace docdb {

class ScanChoices {
 public:
  explicit ScanChoices(bool is_forward_scan) : is_forward_scan_(is_forward_scan) {}
  virtual ~ScanChoices() {}

  bool CurrentIteratorPositionMatchesCurrentTarget(const Slice& curr) {
    VLOG(3) << __PRETTY_FUNCTION__ << " checking if acceptable ? "
            << (curr == current_scan_target_ ? "YEP" : "NOPE")
            << ": " << DocKey::DebugSliceToString(curr)
            << " vs " << DocKey::DebugSliceToString(current_scan_target_.AsSlice());
    return curr == current_scan_target_;
  }

  // Returns false if there are still target keys we need to scan, and true if we are done.
  virtual bool FinishedWithScanChoices() const { return finished_; }

  // Go to the next scan target if any.
  virtual CHECKED_STATUS DoneWithCurrentTarget() = 0;

  // Go (directly) to the new target (or the one after if new_target does not
  // exist in the desired list/range). If the new_target is larger than all scan target options it
  // means we are done.
  virtual CHECKED_STATUS SkipTargetsUpTo(const Slice& new_target) = 0;

  // If the given doc_key isn't already at the desired target, seek appropriately to go to the
  // current target.
  virtual CHECKED_STATUS SeekToCurrentTarget(IntentAwareIterator* db_iter) = 0;

 protected:
  const bool is_forward_scan_;
  KeyBytes current_scan_target_;
  bool finished_ = false;
};

class DiscreteScanChoices : public ScanChoices {
 public:
  DiscreteScanChoices(const DocQLScanSpec& doc_spec, const KeyBytes& lower_doc_key,
                      const KeyBytes& upper_doc_key)
      : ScanChoices(doc_spec.is_forward_scan()) {
    range_cols_scan_options_ = doc_spec.range_options();
    current_scan_target_idxs_.resize(range_cols_scan_options_->size());
    for (int i = 0; i < range_cols_scan_options_->size(); i++) {
      current_scan_target_idxs_[i] = range_cols_scan_options_->at(i).begin();
    }

    // Initialize target doc key.
    if (is_forward_scan_) {
      current_scan_target_ = lower_doc_key;
      if (CHECK_RESULT(ClearRangeComponents(&current_scan_target_))) {
        CHECK_OK(SkipTargetsUpTo(lower_doc_key));
      }
    } else {
      current_scan_target_ = upper_doc_key;
      if (CHECK_RESULT(ClearRangeComponents(&current_scan_target_))) {
        CHECK_OK(SkipTargetsUpTo(upper_doc_key));
      }
    }
  }

  DiscreteScanChoices(const DocPgsqlScanSpec& doc_spec, const KeyBytes& lower_doc_key,
                      const KeyBytes& upper_doc_key)
      : ScanChoices(doc_spec.is_forward_scan()) {
    range_cols_scan_options_ = doc_spec.range_options();
    current_scan_target_idxs_.resize(range_cols_scan_options_->size());
    for (int i = 0; i < range_cols_scan_options_->size(); i++) {
      current_scan_target_idxs_[i] = range_cols_scan_options_->at(i).begin();
    }

    // Initialize target doc key.
    if (is_forward_scan_) {
      current_scan_target_ = lower_doc_key;
      if (CHECK_RESULT(ClearRangeComponents(&current_scan_target_))) {
        CHECK_OK(SkipTargetsUpTo(lower_doc_key));
      }
    } else {
      current_scan_target_ = upper_doc_key;
      if (CHECK_RESULT(ClearRangeComponents(&current_scan_target_))) {
        CHECK_OK(SkipTargetsUpTo(upper_doc_key));
      }
    }
  }

  CHECKED_STATUS DoneWithCurrentTarget() override;
  CHECKED_STATUS SkipTargetsUpTo(const Slice& new_target) override;
  CHECKED_STATUS SeekToCurrentTarget(IntentAwareIterator* db_iter) override;

 protected:
  // Utility function for (multi)key scans. Updates the target scan key by incrementing the option
  // index for one column. Will handle overflow by setting current column index to 0 and
  // incrementing the previous column instead. If it overflows at first column it means we are done,
  // so it clears the scan target idxs array.
  CHECKED_STATUS IncrementScanTargetAtColumn(size_t start_col);

  // Utility function for (multi)key scans to initialize the range portion of the current scan
  // target, scan target with the first option.
  // Only needed for scans that include the static row, otherwise Init will take care of this.
  Result<bool> InitScanTargetRangeGroupIfNeeded();

 private:
  // For (multi)key scans (e.g. selects with 'IN' condition on the range columns) we hold the
  // options for each range column as we iteratively seek to each target key.
  // e.g. for a query "h = 1 and r1 in (2,3) and r2 in (4,5) and r3 = 6":
  //  range_cols_scan_options_   [[2, 3], [4, 5], [6]] -- value options for each column.
  //  current_scan_target_idxs_  goes from [0, 0, 0] up to [1, 1, 0] -- except when including the
  //                             static row when it starts from [0, 0, -1] instead.
  //  current_scan_target_       goes from [1][2,4,6] up to [1][3,5,6] -- is the doc key containing,
  //                             for each range column, the value (option) referenced by the
  //                             corresponding index (updated along with current_scan_target_idxs_).
  std::shared_ptr<std::vector<std::vector<PrimitiveValue>>> range_cols_scan_options_;
  mutable std::vector<std::vector<PrimitiveValue>::const_iterator> current_scan_target_idxs_;
};

Status DiscreteScanChoices::IncrementScanTargetAtColumn(size_t start_col) {
  DCHECK_LE(start_col, current_scan_target_idxs_.size());

  // Increment start col, move backwards in case of overflow.
  int col_idx = start_col;
  for (; col_idx >= 0; col_idx--) {
    const auto& choices = range_cols_scan_options_->at(col_idx);
    auto& it = current_scan_target_idxs_[col_idx];

    if (++it != choices.end()) {
      break;
    }
    it = choices.begin();
  }

  if (col_idx < 0) {
    // If we got here we finished all the options and are done.
    finished_ = true;
    return Status::OK();
  }

  DocKeyDecoder decoder(current_scan_target_);
  RETURN_NOT_OK(decoder.DecodeToRangeGroup());
  for (int i = 0; i != col_idx; ++i) {
    RETURN_NOT_OK(decoder.DecodePrimitiveValue());
  }

  current_scan_target_.Truncate(
      decoder.left_input().cdata() - current_scan_target_.AsSlice().cdata());

  for (size_t i = col_idx; i <= start_col; ++i) {
    current_scan_target_idxs_[i]->AppendToKey(&current_scan_target_);
  }

  return Status::OK();
}

Result<bool> DiscreteScanChoices::InitScanTargetRangeGroupIfNeeded() {
  DocKeyDecoder decoder(current_scan_target_.AsSlice());
  RETURN_NOT_OK(decoder.DecodeToRangeGroup());

  // Initialize the range key values if needed (i.e. we scanned the static row until now).
  if (!VERIFY_RESULT(decoder.HasPrimitiveValue())) {
    current_scan_target_.mutable_data()->pop_back();
    for (size_t col_idx = 0; col_idx < range_cols_scan_options_->size(); col_idx++) {
      current_scan_target_idxs_[col_idx]->AppendToKey(&current_scan_target_);
    }
    current_scan_target_.AppendValueType(ValueType::kGroupEnd);
    return true;
  }
  return false;
}

Status DiscreteScanChoices::DoneWithCurrentTarget() {
  VLOG(2) << __PRETTY_FUNCTION__ << " moving on to next target";
  DCHECK(!FinishedWithScanChoices());

  // Initialize the first target/option if not done already, otherwise go to the next one.
  if (!VERIFY_RESULT(InitScanTargetRangeGroupIfNeeded())) {
    RETURN_NOT_OK(IncrementScanTargetAtColumn(range_cols_scan_options_->size() - 1));
    current_scan_target_.AppendValueType(ValueType::kGroupEnd);
  }
  return Status::OK();
}

Status DiscreteScanChoices::SkipTargetsUpTo(const Slice& new_target) {
  VLOG(2) << __PRETTY_FUNCTION__ << " Updating current target to be >= " << new_target;
  DCHECK(!FinishedWithScanChoices());
  RETURN_NOT_OK(InitScanTargetRangeGroupIfNeeded());
  DocKeyDecoder decoder(new_target);
  RETURN_NOT_OK(decoder.DecodeToRangeGroup());
  current_scan_target_.Reset(Slice(new_target.data(), decoder.left_input().data()));

  size_t col_idx = 0;
  PrimitiveValue target_value;
  while (col_idx < range_cols_scan_options_->size()) {
    RETURN_NOT_OK(decoder.DecodePrimitiveValue(&target_value));
    const auto& choices = (*range_cols_scan_options_)[col_idx];
    auto& it = current_scan_target_idxs_[col_idx];

    // Fast-path in case the existing value for this column already matches the new target.
    if (target_value == *it) {
      col_idx++;
      target_value.AppendToKey(&current_scan_target_);
      continue;
    }

    // Search for the option that matches new target value (for the current column).
    if (is_forward_scan_) {
      it = std::lower_bound(choices.begin(), choices.end(), target_value);
    } else {
      it = std::lower_bound(choices.begin(), choices.end(), target_value, std::greater<>());
    }

    // If we overflowed, the new target value for this column is larger than all our options, so
    // we go back and increment the previous column instead.
    if (it == choices.end()) {
      RETURN_NOT_OK(IncrementScanTargetAtColumn(col_idx - 1));
      break;
    }

    // Else, update the current target value for this column.
    it->AppendToKey(&current_scan_target_);

    // If we did not find an exact match we are already beyond the new target so we can stop.
    if (target_value != *it) {
      col_idx++;
      break;
    }

    col_idx++;
  }

  // If there are any columns left (i.e. we stopped early), it means we did not find an exact
  // match and we reached beyond the new target key. So we need to include all options for the
  // leftover columns (i.e. set all following indexes to 0).
  for (size_t i = col_idx; i < current_scan_target_idxs_.size(); i++) {
    current_scan_target_idxs_[i] = (*range_cols_scan_options_)[i].begin();
    current_scan_target_idxs_[i]->AppendToKey(&current_scan_target_);
  }

  current_scan_target_.AppendValueType(ValueType::kGroupEnd);

  return Status::OK();
}

Status DiscreteScanChoices::SeekToCurrentTarget(IntentAwareIterator* db_iter) {
  VLOG(2) << __PRETTY_FUNCTION__ << " Advancing iterator towards target";
  // Seek to the current target doc key if needed.
  if (!FinishedWithScanChoices()) {
    if (is_forward_scan_) {
      VLOG(2) << __PRETTY_FUNCTION__ << " Seeking to " << current_scan_target_;
      db_iter->Seek(current_scan_target_);
    } else {
      auto tmp = current_scan_target_;
      tmp.AppendValueType(ValueType::kHighest);
      VLOG(2) << __PRETTY_FUNCTION__ << " Going to PrevDocKey " << tmp;
      db_iter->PrevDocKey(tmp);
    }
  }
  return Status::OK();
}

class RangeBasedScanChoices : public ScanChoices {
 public:
  RangeBasedScanChoices(const Schema& schema, const DocQLScanSpec& doc_spec)
      : ScanChoices(doc_spec.is_forward_scan()) {
    DCHECK(doc_spec.range_bounds());
    lower_.reserve(schema.num_range_key_columns());
    upper_.reserve(schema.num_range_key_columns());
    int idx = 0;
    for (idx = schema.num_hash_key_columns(); idx < schema.num_key_columns(); idx++) {
      const ColumnId col_idx = schema.column_id(idx);
      const auto col_sort_type = schema.column(idx).sorting_type();
      const common::QLScanRange::QLRange range = doc_spec.range_bounds()->RangeFor(col_idx);
      const auto lower = GetQLRangeBoundAsPVal(range, col_sort_type, true /* lower_bound */);
      const auto upper = GetQLRangeBoundAsPVal(range, col_sort_type, false /* upper_bound */);
      lower_.emplace_back(lower);
      upper_.emplace_back(upper);
    }
  }

  RangeBasedScanChoices(const Schema& schema, const DocPgsqlScanSpec& doc_spec)
      : ScanChoices(doc_spec.is_forward_scan()) {
    DCHECK(doc_spec.range_bounds());
    lower_.reserve(schema.num_range_key_columns());
    upper_.reserve(schema.num_range_key_columns());
    int idx = 0;
    for (idx = schema.num_hash_key_columns(); idx < schema.num_key_columns(); idx++) {
      const ColumnId col_idx = schema.column_id(idx);
      const auto col_sort_type = schema.column(idx).sorting_type();
      const common::QLScanRange::QLRange range = doc_spec.range_bounds()->RangeFor(col_idx);
      const auto lower = GetQLRangeBoundAsPVal(range, col_sort_type, true /* lower_bound */);
      const auto upper = GetQLRangeBoundAsPVal(range, col_sort_type, false /* upper_bound */);
      lower_.emplace_back(lower);
      upper_.emplace_back(upper);
    }
  }

  CHECKED_STATUS SkipTargetsUpTo(const Slice& new_target) override;
  CHECKED_STATUS DoneWithCurrentTarget() override;
  CHECKED_STATUS SeekToCurrentTarget(IntentAwareIterator* db_iter) override;

 private:
  std::vector<PrimitiveValue> lower_, upper_;
  KeyBytes prev_scan_target_;
};

Status RangeBasedScanChoices::SkipTargetsUpTo(const Slice& new_target) {
  VLOG(2) << __PRETTY_FUNCTION__ << " Updating current target to be >= "
          << DocKey::DebugSliceToString(new_target);
  DCHECK(!FinishedWithScanChoices());

  /*
   Let's say we have a row key with (A B) as the hash part and C, D as the range part:
   ((A B) C D) E F

   Let's say we have a range constraint :
    l_c < C < u_c
     4        6

    a b  0 d  -> a  b l_c  l_d

    a b  5 d  -> a  b  5    d
                  [ Will subsequently week out of document on reading the subdoc]

    a b  7 d  -> a <b> MAX
                [ This will seek to <b_next> and on the next invocation update:
                   a <b_next> ? ? -> a <b_next> l_c l_d ]
  */
  DocKeyDecoder decoder(new_target);
  RETURN_NOT_OK(decoder.DecodeToRangeGroup());
  current_scan_target_.Reset(Slice(new_target.data(), decoder.left_input().data()));

  int col_idx = 0;
  PrimitiveValue target_value;
  bool last_was_infinity = false;
  for (col_idx = 0; VERIFY_RESULT(decoder.HasPrimitiveValue()); col_idx++) {
    RETURN_NOT_OK(decoder.DecodePrimitiveValue(&target_value));
    VLOG(3) << "col_idx " << col_idx << " is " << target_value << " in ["
            << yb::ToString(lower_[col_idx]) << " , " << yb::ToString(upper_[col_idx]) << " ] ?";

    const auto& lower = lower_[col_idx];
    if (target_value < lower) {
      const auto tgt = (is_forward_scan_ ? lower : PrimitiveValue(ValueType::kLowest));
      tgt.AppendToKey(&current_scan_target_);
      last_was_infinity = tgt.IsInfinity();
      VLOG(3) << " Updating idx " << col_idx << " from " << target_value << " to " << tgt;
      break;
    }
    const auto& upper = upper_[col_idx];
    if (target_value > upper) {
      const auto tgt = (!is_forward_scan_ ? upper : PrimitiveValue(ValueType::kHighest));
      VLOG(3) << " Updating idx " << col_idx << " from " << target_value << " to " << tgt;
      tgt.AppendToKey(&current_scan_target_);
      last_was_infinity = tgt.IsInfinity();
      break;
    }
    target_value.AppendToKey(&current_scan_target_);
    last_was_infinity = target_value.IsInfinity();
  }

  // Reset the remaining range columns to kHighest/lower for forward scans
  // or kLowest/upper for backward scans.
  while (++col_idx < lower_.size()) {
    if (last_was_infinity) {
      // No point having more components after +/- Inf.
      break;
    }
    if (is_forward_scan_) {
      VLOG(3) << " Updating col_idx " << col_idx << " to " << lower_[col_idx];
      lower_[col_idx].AppendToKey(&current_scan_target_);
      last_was_infinity = lower_[col_idx].IsInfinity();
    } else {
      VLOG(3) << " Updating col_idx " << col_idx << " to " << upper_[col_idx];
      upper_[col_idx].AppendToKey(&current_scan_target_);
      last_was_infinity = upper_[col_idx].IsInfinity();
    }
  }
  VLOG(2) << "After " << __PRETTY_FUNCTION__ << " current_scan_target_ is "
          << DocKey::DebugSliceToString(current_scan_target_);
  current_scan_target_.AppendValueType(ValueType::kGroupEnd);

  return Status::OK();
}

Status RangeBasedScanChoices::DoneWithCurrentTarget() {
  prev_scan_target_ = current_scan_target_;
  current_scan_target_.Clear();
  return Status::OK();
}

Status RangeBasedScanChoices::SeekToCurrentTarget(IntentAwareIterator* db_iter) {
  VLOG(2) << __PRETTY_FUNCTION__ << " Advancing iterator towards target";

  if (!FinishedWithScanChoices()) {
    if (!current_scan_target_.empty()) {
      VLOG(3) << __PRETTY_FUNCTION__ << " current_scan_target_ is non-empty. "
              << current_scan_target_;
      if (is_forward_scan_) {
        VLOG(3) << __PRETTY_FUNCTION__ << " Seeking to " << current_scan_target_;
        db_iter->Seek(current_scan_target_);
      } else {
        auto tmp = current_scan_target_;
        PrimitiveValue(ValueType::kHighest).AppendToKey(&tmp);
        VLOG(3) << __PRETTY_FUNCTION__ << " Going to PrevDocKey " << tmp;  // Never seen.
        db_iter->PrevDocKey(tmp);
      }
    } else {
      if (!is_forward_scan_ && !prev_scan_target_.empty()) {
        db_iter->PrevDocKey(prev_scan_target_);
      }
    }
  }

  return Status::OK();
}

DocRowwiseIterator::DocRowwiseIterator(
    const Schema &projection,
    const Schema &schema,
    const TransactionOperationContextOpt& txn_op_context,
    const DocDB& doc_db,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    RWOperationCounter* pending_op_counter)
    : projection_(projection),
      schema_(schema),
      txn_op_context_(txn_op_context),
      deadline_(deadline),
      read_time_(read_time),
      doc_db_(doc_db),
      has_bound_key_(false),
      pending_op_(pending_op_counter),
      done_(false) {
  projection_subkeys_.reserve(projection.num_columns() + 1);
  projection_subkeys_.push_back(PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
  for (size_t i = projection_.num_key_columns(); i < projection.num_columns(); i++) {
    projection_subkeys_.emplace_back(projection.column_id(i));
  }
  std::sort(projection_subkeys_.begin(), projection_subkeys_.end());
  deadline_info_.emplace(deadline);
}

DocRowwiseIterator::~DocRowwiseIterator() {
}

Status DocRowwiseIterator::Init() {
  db_iter_ = CreateIntentAwareIterator(
      doc_db_,
      BloomFilterMode::DONT_USE_BLOOM_FILTER,
      boost::none /* user_key_for_filter */,
      rocksdb::kDefaultQueryId,
      txn_op_context_,
      deadline_,
      read_time_);

  DocKeyEncoder(&iter_key_).Schema(schema_);
  row_key_ = iter_key_;
  row_hash_key_ = row_key_;
  VLOG(3) << __PRETTY_FUNCTION__ << " Seeking to " << row_key_;
  db_iter_->Seek(row_key_);
  row_ready_ = false;
  has_bound_key_ = false;

  return Status::OK();
}

Result<bool> DocRowwiseIterator::InitScanChoices(
    const DocQLScanSpec& doc_spec, const KeyBytes& lower_doc_key, const KeyBytes& upper_doc_key) {
  if (doc_spec.range_options()) {
    scan_choices_.reset(new DiscreteScanChoices(doc_spec, lower_doc_key, upper_doc_key));
    // Let's not seek to the lower doc key or upper doc key. We know exactly what we want.
    RETURN_NOT_OK(AdvanceIteratorToNextDesiredRow());
    return true;
  }

  if (doc_spec.range_bounds()) {
    scan_choices_.reset(new RangeBasedScanChoices(schema_, doc_spec));
  }

  return false;
}

Result<bool> DocRowwiseIterator::InitScanChoices(
    const DocPgsqlScanSpec& doc_spec, const KeyBytes& lower_doc_key,
    const KeyBytes& upper_doc_key) {
  if (doc_spec.range_options()) {
    scan_choices_.reset(new DiscreteScanChoices(doc_spec, lower_doc_key, upper_doc_key));
    // Let's not seek to the lower doc key or upper doc key. We know exactly what we want.
    RETURN_NOT_OK(AdvanceIteratorToNextDesiredRow());
    return true;
  }

  if (doc_spec.range_bounds()) {
    scan_choices_.reset(new RangeBasedScanChoices(schema_, doc_spec));
  }

  return false;
}

template <class T>
Status DocRowwiseIterator::DoInit(const T& doc_spec) {
  is_forward_scan_ = doc_spec.is_forward_scan();

  VLOG(4) << "Initializing iterator direction: " << (is_forward_scan_ ? "FORWARD" : "BACKWARD");

  auto lower_doc_key = VERIFY_RESULT(doc_spec.LowerBound());
  auto upper_doc_key = VERIFY_RESULT(doc_spec.UpperBound());
  VLOG(4) << "DocKey Bounds " << DocKey::DebugSliceToString(lower_doc_key.AsSlice())
          << ", " << DocKey::DebugSliceToString(upper_doc_key.AsSlice());

  // TODO(bogdan): decide if this is a good enough heuristic for using blooms for scans.
  const bool is_fixed_point_get =
      !lower_doc_key.empty() &&
      VERIFY_RESULT(HashedOrFirstRangeComponentsEqual(lower_doc_key, upper_doc_key));
  const auto mode = is_fixed_point_get ? BloomFilterMode::USE_BLOOM_FILTER
                                       : BloomFilterMode::DONT_USE_BLOOM_FILTER;

  db_iter_ = CreateIntentAwareIterator(
      doc_db_, mode, lower_doc_key.AsSlice(), doc_spec.QueryId(), txn_op_context_,
      deadline_, read_time_, doc_spec.CreateFileFilter());

  row_ready_ = false;

  if (is_forward_scan_) {
    has_bound_key_ = !upper_doc_key.empty();
    if (has_bound_key_) {
      bound_key_ = std::move(upper_doc_key);
      db_iter_->SetUpperbound(bound_key_);
    }
  } else {
    has_bound_key_ = !lower_doc_key.empty();
    if (has_bound_key_) {
      bound_key_ = std::move(lower_doc_key);
    }
  }

  if (!VERIFY_RESULT(InitScanChoices(doc_spec, lower_doc_key, upper_doc_key))) {
    if (is_forward_scan_) {
      VLOG(3) << __PRETTY_FUNCTION__ << " Seeking to " << DocKey::DebugSliceToString(lower_doc_key);
      db_iter_->Seek(lower_doc_key);
    } else {
      // TODO consider adding an operator bool to DocKey to use instead of empty() here.
      if (!upper_doc_key.empty()) {
        db_iter_->PrevDocKey(upper_doc_key);
      } else {
        db_iter_->SeekToLastDocKey();
      }
    }
  }

  return Status::OK();
}

Status DocRowwiseIterator::Init(const common::QLScanSpec& spec) {
  return DoInit(dynamic_cast<const DocQLScanSpec&>(spec));
}

Status DocRowwiseIterator::Init(const common::PgsqlScanSpec& spec) {
  return DoInit(dynamic_cast<const DocPgsqlScanSpec&>(spec));
}

Status DocRowwiseIterator::AdvanceIteratorToNextDesiredRow() const {
  if (scan_choices_) {
    if (!IsNextStaticColumn()
        && !scan_choices_->CurrentIteratorPositionMatchesCurrentTarget(row_key_)) {
      return scan_choices_->SeekToCurrentTarget(db_iter_.get());
    }
  } else {
    if (!is_forward_scan_) {
      VLOG(4) << __PRETTY_FUNCTION__ << " setting as PrevDocKey";
      db_iter_->PrevDocKey(row_key_);
    }
  }

  return Status::OK();
}

Result<bool> DocRowwiseIterator::HasNext() const {
  VLOG(4) << __PRETTY_FUNCTION__;

  // Repeated HasNext calls (without Skip/NextRow in between) should be idempotent:
  // 1. If a previous call failed we returned the same status.
  // 2. If a row is already available (row_ready_), return true directly.
  // 3. If we finished all target rows for the scan (done_), return false directly.
  RETURN_NOT_OK(has_next_status_);
  if (row_ready_) {
    // If row is ready, then HasNext returns true.
    return true;
  }
  if (done_) {
    return false;
  }

  bool doc_found = false;
  while (!doc_found) {
    if (!db_iter_->valid() || (scan_choices_ && scan_choices_->FinishedWithScanChoices())) {
      done_ = true;
      return false;
    }

    const auto key_data = db_iter_->FetchKey();
    if (!key_data.ok()) {
      has_next_status_ = key_data.status();
      return has_next_status_;
    }

    VLOG(4) << "*fetched_key is " << SubDocKey::DebugSliceToString(key_data->key);

    // The iterator is positioned by the previous GetSubDocument call (which places the iterator
    // outside the previous doc_key). Ensure the iterator is pushed forward/backward indeed. We
    // check it here instead of after GetSubDocument() below because we want to avoid the extra
    // expensive FetchKey() call just to fetch and validate the key.
    if (!iter_key_.data().empty() &&
        (is_forward_scan_ ? iter_key_.CompareTo(key_data->key) >= 0
                          : iter_key_.CompareTo(key_data->key) <= 0)) {
      has_next_status_ = STATUS_SUBSTITUTE(Corruption, "Infinite loop detected at $0",
                                           FormatSliceAsStr(key_data->key));
      return has_next_status_;
    }
    iter_key_.Reset(key_data->key);
    VLOG(4) << " Current iter_key_ is " << iter_key_;

    const auto dockey_sizes = DocKey::EncodedHashPartAndDocKeySizes(iter_key_);
    if (!dockey_sizes.ok()) {
      has_next_status_ = dockey_sizes.status();
      return has_next_status_;
    }
    row_hash_key_ = iter_key_.AsSlice().Prefix(dockey_sizes->first);
    row_key_ = iter_key_.AsSlice().Prefix(dockey_sizes->second);

    if (!DocKeyBelongsTo(row_key_, schema_) ||
        (has_bound_key_ && is_forward_scan_ == (row_key_.compare(bound_key_) >= 0))) {
      done_ = true;
      return false;
    }

    // Prepare the DocKey to get the SubDocument. Trim the DocKey to contain just the primary key.
    Slice sub_doc_key = row_key_;
    VLOG(4) << " sub_doc_key part of iter_key_ is " << DocKey::DebugSliceToString(sub_doc_key);

    bool is_static_column = IsNextStaticColumn();
    if (scan_choices_ && !is_static_column) {
      if (!scan_choices_->CurrentIteratorPositionMatchesCurrentTarget(row_key_)) {
        // We must have seeked past the target key we are looking for (no result) so we can safely
        // skip all scan targets between the current target and row key (excluding row_key_ itself).
        // Update the target key and iterator and call HasNext again to try the next target.
        RETURN_NOT_OK(scan_choices_->SkipTargetsUpTo(row_key_));

        // We updated scan target above, if it goes past the row_key_ we will seek again, and
        // process the found key in the next loop.
        if (!scan_choices_->CurrentIteratorPositionMatchesCurrentTarget(row_key_)) {
          RETURN_NOT_OK(scan_choices_->SeekToCurrentTarget(db_iter_.get()));
          continue;
        }
      }
      // We found a match for the target key or a static column, so we move on to getting the
      // SubDocument.
    }

    GetSubDocumentData data = {
      sub_doc_key,
      &row_,
      &doc_found,
      TableTTL(schema_),
      &table_tombstone_time_,
    };
    data.deadline_info = deadline_info_.get_ptr();
    has_next_status_ = GetSubDocument(db_iter_.get(), data, &projection_subkeys_);
    RETURN_NOT_OK(has_next_status_);
    // After this, the iter should be positioned right after the subdocument.

    if (!doc_found) {
      SubDocument full_row;
      // If doc is not found, decide if some non-projection column exists.
      // Currently we read the whole doc here,
      // may be optimized by exiting on the first column in future.
      db_iter_->Seek(row_key_);  // Position it for GetSubDocument.
      data.result = &full_row;
      has_next_status_ = GetSubDocument(db_iter_.get(), data);
      RETURN_NOT_OK(has_next_status_);
    }
    if (scan_choices_ && !is_static_column) {
      has_next_status_ = scan_choices_->DoneWithCurrentTarget();
      RETURN_NOT_OK(has_next_status_);
    }
    has_next_status_ = AdvanceIteratorToNextDesiredRow();
    RETURN_NOT_OK(has_next_status_);
  }
  row_ready_ = true;
  return true;
}

string DocRowwiseIterator::ToString() const {
  return "DocRowwiseIterator";
}

namespace {

// Set primary key column values (hashed or range columns) in a QL row value map.
CHECKED_STATUS SetQLPrimaryKeyColumnValues(const Schema& schema,
                                           const size_t begin_index,
                                           const size_t column_count,
                                           const char* column_type,
                                           DocKeyDecoder* decoder,
                                           QLTableRow* table_row) {
  if (begin_index + column_count > schema.num_columns()) {
    return STATUS_SUBSTITUTE(
        Corruption,
        "$0 primary key columns between positions $1 and $2 go beyond table columns $3",
        column_type, begin_index, begin_index + column_count - 1, schema.num_columns());
  }
  PrimitiveValue primitive_value;
  for (size_t i = 0, j = begin_index; i < column_count; i++, j++) {
    const auto ql_type = schema.column(j).type();
    QLTableColumn& column = table_row->AllocColumn(schema.column_id(j));
    RETURN_NOT_OK(decoder->DecodePrimitiveValue(&primitive_value));
    PrimitiveValue::ToQLValuePB(primitive_value, ql_type, &column.value);
  }
  return decoder->ConsumeGroupEnd();
}

} // namespace

void DocRowwiseIterator::SkipRow() {
  row_ready_ = false;
}

HybridTime DocRowwiseIterator::RestartReadHt() {
  auto max_seen_ht = db_iter_->max_seen_ht();
  if (max_seen_ht.is_valid() && max_seen_ht > db_iter_->read_time().read) {
    VLOG(4) << "Restart read: " << max_seen_ht << ", original: " << db_iter_->read_time();
    return max_seen_ht;
  }
  return HybridTime::kInvalid;
}

bool DocRowwiseIterator::IsNextStaticColumn() const {
  return schema_.has_statics() && row_hash_key_.end() + 1 == row_key_.end();
}

Status DocRowwiseIterator::DoNextRow(const Schema& projection, QLTableRow* table_row) {
  VLOG(4) << __PRETTY_FUNCTION__;

  if (PREDICT_FALSE(done_)) {
    return STATUS(NotFound, "end of iter");
  }

  // Ensure row is ready to be read. HasNext() must be called before reading the first row, or
  // again after the previous row has been read or skipped.
  if (!row_ready_) {
    return STATUS(InternalError, "next row has not be prepared for reading");
  }

  DocKeyDecoder decoder(row_key_);
  RETURN_NOT_OK(decoder.DecodeCotableId());
  RETURN_NOT_OK(decoder.DecodePgtableId());
  bool has_hash_components = VERIFY_RESULT(decoder.DecodeHashCode());

  // Populate the key column values from the doc key. The key column values in doc key were
  // written in the same order as in the table schema (see DocKeyFromQLKey). If the range columns
  // are present, read them also.
  if (has_hash_components) {
    RETURN_NOT_OK(SetQLPrimaryKeyColumnValues(
        schema_, 0, schema_.num_hash_key_columns(),
        "hash", &decoder, table_row));
  }
  if (!decoder.GroupEnded()) {
    RETURN_NOT_OK(SetQLPrimaryKeyColumnValues(
        schema_, schema_.num_hash_key_columns(), schema_.num_range_key_columns(),
        "range", &decoder, table_row));
  }

  for (size_t i = projection.num_key_columns(); i < projection.num_columns(); i++) {
    const auto& column_id = projection.column_id(i);
    const auto ql_type = projection.column(i).type();
    const SubDocument* column_value = row_.GetChild(PrimitiveValue(column_id));
    if (column_value != nullptr) {
      QLTableColumn& column = table_row->AllocColumn(column_id);
      SubDocument::ToQLValuePB(*column_value, ql_type, &column.value);
      column.ttl_seconds = column_value->GetTtl();
      if (column_value->IsWriteTimeSet()) {
        column.write_time = column_value->GetWriteTime();
      }
    }
  }

  row_ready_ = false;
  return Status::OK();
}

bool DocRowwiseIterator::LivenessColumnExists() const {
  const SubDocument* subdoc = row_.GetChild(
      PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
  return subdoc != nullptr && subdoc->value_type() != ValueType::kInvalid;
}

CHECKED_STATUS DocRowwiseIterator::GetNextReadSubDocKey(SubDocKey* sub_doc_key) const {
  if (db_iter_ == nullptr) {
    return STATUS(Corruption, "Iterator not initialized.");
  }

  // There are no more rows to fetch, so no next SubDocKey to read.
  if (!VERIFY_RESULT(HasNext())) {
    DVLOG(3) << "No Next SubDocKey";
    return Status::OK();
  }

  DocKey doc_key;
  RETURN_NOT_OK(doc_key.FullyDecodeFrom(row_key_));
  *sub_doc_key = SubDocKey(doc_key, read_time_.read);
  DVLOG(3) << "Next SubDocKey: " << sub_doc_key->ToString();
  return Status::OK();
}

Result<Slice> DocRowwiseIterator::GetTupleId() const {
  // Return tuple id without cotable id / pgtable id if any.
  Slice tuple_id = row_key_;
  if (tuple_id.starts_with(ValueTypeAsChar::kTableId)) {
    tuple_id.remove_prefix(1 + kUuidSize);
  } else if (tuple_id.starts_with(ValueTypeAsChar::kPgTableOid)) {
    tuple_id.remove_prefix(1 + sizeof(PgTableOid));
  }
  return tuple_id;
}

Result<bool> DocRowwiseIterator::SeekTuple(const Slice& tuple_id) {
  // If cotable id / pgtable id is present in the table schema, then
  // we need to prepend it in the tuple key to seek.
  if (schema_.has_cotable_id() || schema_.has_pgtable_id()) {
    uint32_t size = schema_.has_pgtable_id() ? sizeof(PgTableOid) : kUuidSize;
    if (!tuple_key_) {
      tuple_key_.emplace();
      tuple_key_->Reserve(1 + size + tuple_id.size());

      if (schema_.has_cotable_id()) {
        std::string bytes;
        schema_.cotable_id().EncodeToComparable(&bytes);
        tuple_key_->AppendValueType(ValueType::kTableId);
        tuple_key_->AppendRawBytes(bytes);
      } else {
        tuple_key_->AppendValueType(ValueType::kPgTableOid);
        tuple_key_->AppendUInt32(schema_.pgtable_id());
      }
    } else {
      tuple_key_->Truncate(1 + size);
    }
    tuple_key_->AppendRawBytes(tuple_id);
    db_iter_->Seek(*tuple_key_);
  } else {
    db_iter_->Seek(tuple_id);
  }

  iter_key_.Clear();
  row_ready_ = false;

  return VERIFY_RESULT(HasNext()) && VERIFY_RESULT(GetTupleId()) == tuple_id;
}

}  // namespace docdb
}  // namespace yb
