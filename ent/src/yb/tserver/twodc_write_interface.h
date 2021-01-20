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

#ifndef ENT_SRC_YB_TSERVER_TWODC_WRITE_INTERFACE_H
#define ENT_SRC_YB_TSERVER_TWODC_WRITE_INTERFACE_H

#include <memory>
#include <string>

namespace yb {
namespace cdc {

class CDCRecordPB;

}
namespace tserver {

class WriteRequestPB;

namespace enterprise {

class TwoDCWriteInterface {
 public:
  virtual ~TwoDCWriteInterface() {}
  virtual std::unique_ptr<WriteRequestPB> GetNextWriteRequest() = 0;
  virtual CHECKED_STATUS ProcessRecord(
      const std::string& tablet_id, const cdc::CDCRecordPB& record) = 0;
};

void ResetWriteInterface(std::unique_ptr<TwoDCWriteInterface>* write_strategy);

} // namespace enterprise
} // namespace tserver
} // namespace yb


#endif // ENT_SRC_YB_TSERVER_TWODC_WRITE_INTERFACE_H
