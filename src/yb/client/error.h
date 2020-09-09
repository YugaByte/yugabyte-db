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

#ifndef YB_CLIENT_ERROR_H
#define YB_CLIENT_ERROR_H

#include "yb/client/client_fwd.h"

namespace yb {
namespace client {

// An error which occurred in a given operation. This tracks the operation
// which caused the error, along with whatever the actual error was.
class YBError {
 public:
  YBError(std::shared_ptr<YBOperation> failed_op, const Status& error);
  ~YBError();

  // Return the actual error which occurred.
  const Status& status() const;

  // Return the operation which failed.
  const YBOperation& failed_op() const;
  YBOperation& failed_op();

  // In some cases, it's possible that the server did receive and successfully
  // perform the requested operation, but the client can't tell whether or not
  // it was successful. For example, if the call times out, the server may still
  // succeed in processing at a later time.
  //
  // This function returns true if there is some chance that the server did
  // process the operation, and false if it can guarantee that the operation
  // did not succeed.
  bool was_possibly_successful() const;

 private:
  std::shared_ptr<YBOperation> failed_op_;
  Status status_;
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_ERROR_H
