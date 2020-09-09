//
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
//

#ifndef YB_CONSENSUS_CONSENSUS_ERROR_H
#define YB_CONSENSUS_CONSENSUS_ERROR_H

#include "yb/consensus/consensus.pb.h"
#include "yb/util/status.h"

namespace yb {
namespace consensus {

struct ConsensusErrorTag : IntegralErrorTag<ConsensusErrorPB::Code> {
  // This category id is part of the wire protocol and should not be changed once released.
  static constexpr uint8_t kCategory = 11;

  static const std::string& ToMessage(Value code) {
    return ConsensusErrorPB::Code_Name(code);
  }
};

typedef StatusErrorCodeImpl<ConsensusErrorTag> ConsensusError;

} // namespace consensus
} // namespace yb

#endif // YB_CONSENSUS_CONSENSUS_ERROR_H
