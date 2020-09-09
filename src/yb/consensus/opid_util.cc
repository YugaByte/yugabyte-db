// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
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

#include "yb/consensus/opid_util.h"

#include <algorithm>
#include <limits>

#include <glog/logging.h>

#include "yb/consensus/consensus.pb.h"
#include "yb/gutil/port.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/opid.h"

namespace yb {
namespace consensus {

const int64_t kMinimumTerm = 0;
const int64_t kMinimumOpIdIndex = 0;
const int64_t kInvalidOpIdIndex = -1;

int OpIdCompare(const OpIdPB& first, const OpIdPB& second) {
  DCHECK(first.IsInitialized());
  DCHECK(second.IsInitialized());
  if (PREDICT_TRUE(first.term() == second.term())) {
    return first.index() < second.index() ? -1 : first.index() == second.index() ? 0 : 1;
  }
  return first.term() < second.term() ? -1 : 1;
}

bool OpIdEquals(const OpIdPB& left, const OpIdPB& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  return left.term() == right.term() && left.index() == right.index();
}

bool OpIdLessThan(const OpIdPB& left, const OpIdPB& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  if (left.term() < right.term()) return true;
  if (left.term() > right.term()) return false;
  return left.index() < right.index();
}

bool OpIdBiggerThan(const OpIdPB& left, const OpIdPB& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  if (left.term() > right.term()) return true;
  if (left.term() < right.term()) return false;
  return left.index() > right.index();
}

bool CopyIfOpIdLessThan(const OpIdPB& to_compare, OpIdPB* target) {
  if (to_compare.IsInitialized() &&
      (!target->IsInitialized() || OpIdLessThan(to_compare, *target))) {
    target->CopyFrom(to_compare);
    return true;
  }
  return false;
}

size_t OpIdHashFunctor::operator() (const OpIdPB& id) const {
  return (id.term() + 31) ^ id.index();
}

bool OpIdEqualsFunctor::operator() (const OpIdPB& left, const OpIdPB& right) const {
  return OpIdEquals(left, right);
}

bool OpIdLessThanPtrFunctor::operator() (const OpIdPB* left, const OpIdPB* right) const {
  return OpIdLessThan(*left, *right);
}

bool OpIdIndexLessThanPtrFunctor::operator() (const OpIdPB* left, const OpIdPB* right) const {
  return left->index() < right->index();
}

bool OpIdCompareFunctor::operator() (const OpIdPB& left, const OpIdPB& right) const {
  return OpIdLessThan(left, right);
}

bool OpIdBiggerThanFunctor::operator() (const OpIdPB& left, const OpIdPB& right) const {
  return OpIdBiggerThan(left, right);
}

OpIdPB MinimumOpId() {
  OpIdPB op_id;
  op_id.set_term(0);
  op_id.set_index(0);
  return op_id;
}

OpIdPB MaximumOpId() {
  OpIdPB op_id;
  op_id.set_term(std::numeric_limits<int64_t>::max());
  op_id.set_index(std::numeric_limits<int64_t>::max());
  return op_id;
}

// helper hash functor for delta store ids
struct DeltaIdHashFunction {
  size_t operator()(const std::pair<int64_t, int64_t >& id) const {
    return (id.first + 31) ^ id.second;
  }
};

// helper equals functor for delta store ids
struct DeltaIdEqualsTo {
  bool operator()(const std::pair<int64_t, int64_t >& left,
                  const std::pair<int64_t, int64_t >& right) const {
    return left.first == right.first && left.second == right.second;
  }
};

std::ostream& operator<<(std::ostream& os, const OpIdPB& op_id) {
  os << OpIdToString(op_id);
  return os;
}

std::string OpIdToString(const OpIdPB& op_id) {
  if (!op_id.IsInitialized()) {
    return "<uninitialized op>";
  }
  return strings::Substitute("$0.$1", op_id.term(), op_id.index());
}

std::string OpsRangeString(const ConsensusRequestPB& req) {
  std::string ret;
  ret.reserve(100);
  ret.push_back('[');
  if (req.ops_size() > 0) {
    const OpIdPB& first_op = req.ops(0).id();
    const OpIdPB& last_op = req.ops(req.ops_size() - 1).id();
    strings::SubstituteAndAppend(&ret, "$0.$1-$2.$3",
                                 first_op.term(), first_op.index(),
                                 last_op.term(), last_op.index());
  }
  ret.push_back(']');
  return ret;
}

OpIdPB MakeOpId(int64_t term, int64_t index) {
  OpIdPB ret;
  ret.set_index(index);
  ret.set_term(term);
  LOG_IF(DFATAL, term < 0 || index < 0) << "MakeOpId: negative term/index: " << OpIdToString(ret);
  return ret;
}

OpIdPB MakeOpIdPB(const yb::OpId& op_id) {
  return MakeOpId(op_id.term, op_id.index);
}

} // namespace consensus
}  // namespace yb
