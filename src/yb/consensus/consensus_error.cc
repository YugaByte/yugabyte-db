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

#include "yb/consensus/consensus_error.h"

namespace yb {
namespace consensus {

static const std::string kConsensusErrorCategoryName = "consensus error";

static yb::StatusCategoryRegisterer consensus_error_category_registerer(
    yb::StatusCategoryDescription::Make<ConsensusErrorTag>(&kConsensusErrorCategoryName));

} // namespace consensus
} // namespace yb
