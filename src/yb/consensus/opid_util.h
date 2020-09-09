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

#ifndef YB_CONSENSUS_OPID_UTIL_H_
#define YB_CONSENSUS_OPID_UTIL_H_

#include <stdint.h>

#include <iosfwd>
#include <string>
#include <utility>

#include "yb/util/opid.h"

namespace yb {

class OpIdPB;

namespace consensus {

class ConsensusRequestPB;

// Minimum possible term.
extern const int64_t kMinimumTerm;

// Minimum possible log index.
extern const int64_t kMinimumOpIdIndex;

// Log index that is lower than the minimum index (and so will never occur).
extern const int64_t kInvalidOpIdIndex;

// Returns true iff left == right.
bool OpIdEquals(const OpIdPB& left, const OpIdPB& right);

// Returns true iff left < right.
bool OpIdLessThan(const OpIdPB& left, const OpIdPB& right);

// Returns true iff left > right.
bool OpIdBiggerThan(const OpIdPB& left, const OpIdPB& right);

// Copies to_compare into target under the following conditions:
// - If to_compare is initialized and target is not.
// - If they are both initialized and to_compare is less than target.
// Otherwise, does nothing.
// If to_compare is copied into target, returns true, else false.
bool CopyIfOpIdLessThan(const OpIdPB& to_compare, OpIdPB* target);

// Return -1 if left < right,
//         0 if equal,
//         1 if left > right.
int OpIdCompare(const OpIdPB& left, const OpIdPB& right);

// OpId hash functor. Suitable for use with std::unordered_map.
struct OpIdHashFunctor {
  size_t operator() (const OpIdPB& id) const;
};

// OpId equals functor. Suitable for use with std::unordered_map.
struct OpIdEqualsFunctor {
  bool operator() (const OpIdPB& left, const OpIdPB& right) const;
};

// OpId less than functor for pointers.. Suitable for use with std::sort and std::map.
struct OpIdLessThanPtrFunctor {
  // Returns true iff left < right.
  bool operator() (const OpIdPB* left, const OpIdPB* right) const;
};

// Sorts op id's by index only, disregarding the term.
struct OpIdIndexLessThanPtrFunctor {
  // Returns true iff left.index() < right.index().
  bool operator() (const OpIdPB* left, const OpIdPB* right) const;
};

// OpId compare() functor. Suitable for use with std::sort and std::map.
struct OpIdCompareFunctor {
  // Returns true iff left < right.
  bool operator() (const OpIdPB& left, const OpIdPB& right) const;
};

// OpId comparison functor that returns true iff left > right. Suitable for use
// with std::sort and std::map to sort keys in increasing order.]
struct OpIdBiggerThanFunctor {
  bool operator() (const OpIdPB& left, const OpIdPB& right) const;
};

std::ostream& operator<<(std::ostream& os, const OpIdPB& op_id);

// Return the minimum possible OpId.
OpIdPB MinimumOpId();

// Return the maximum possible OpId.
OpIdPB MaximumOpId();

std::string OpIdToString(const OpIdPB& id);

std::string OpsRangeString(const ConsensusRequestPB& req);

OpIdPB MakeOpId(int64_t term, int64_t index);
OpIdPB MakeOpIdPB(const yb::OpId& op_id);

}  // namespace consensus

using consensus::operator<<;

}  // namespace yb

#endif /* YB_CONSENSUS_OPID_UTIL_H_ */
