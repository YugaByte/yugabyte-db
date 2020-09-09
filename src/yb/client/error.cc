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

#include "yb/client/error.h"

namespace yb {
namespace client {

////////////////////////////////////////////////////////////
// Error
////////////////////////////////////////////////////////////

YBError::YBError(YBOperationPtr failed_op, const Status& status)
    : failed_op_(std::move(failed_op)), status_(status) {}

YBError::~YBError() {}

const Status& YBError::status() const {
  return status_;
}

const YBOperation& YBError::failed_op() const {
  return *failed_op_;
}

YBOperation& YBError::failed_op() {
  return *failed_op_;
}

bool YBError::was_possibly_successful() const {
  // TODO: implement me - right now be conservative.
  return true;
}

} // namespace client
} // namespace yb
