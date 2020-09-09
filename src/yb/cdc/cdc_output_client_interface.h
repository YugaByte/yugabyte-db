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

#ifndef YB_CDC_CDC_OUTPUT_CLIENT_INTERFACE_H
#define YB_CDC_CDC_OUTPUT_CLIENT_INTERFACE_H

#include "yb/cdc/cdc_service.pb.h"

#include "yb/util/status.h"
#include "yb/util/status_callback.h"
#include "yb/consensus/opid_util.h"


namespace yb {

namespace client {

class YBTableName;

} // namespace client

namespace cdc {

struct OutputClientResponse {
  Status status;
  OpIdPB last_applied_op_id;
  uint32_t processed_record_count;
};

class CDCOutputClient {
 public:
  virtual ~CDCOutputClient() {}
  virtual void Shutdown() {}
  // Async call for applying changes.
  virtual CHECKED_STATUS ApplyChanges(const cdc::GetChangesResponsePB* resp) = 0;
};

} // namespace cdc
} // namespace yb

#endif // YB_CDC_CDC_OUTPUT_CLIENT_INTERFACE_H
