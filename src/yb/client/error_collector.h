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
#ifndef YB_CLIENT_ERROR_COLLECTOR_H
#define YB_CLIENT_ERROR_COLLECTOR_H

#include <memory>
#include <vector>

#include "yb/client/client_fwd.h"

#include "yb/gutil/ref_counted.h"
#include "yb/gutil/thread_annotations.h"

#include "yb/util/locks.h"

namespace yb {
namespace client {

class YBError;

namespace internal {

class ErrorCollector : public RefCountedThreadSafe<ErrorCollector> {
 public:
  ErrorCollector();

  void AddError(std::unique_ptr<YBError> error);

  void AddError(YBOperationPtr operation, Status status);

  // See YBSession for details.
  int CountErrors() const;

  // See YBSession for details.
  CollectedErrors GetAndClearErrors();

  void ClearErrors();

  // If there is only one error in the error collector, returns its associated status. Otherwise
  // returns Status::OK().
  Status GetSingleErrorStatus();

 private:
  friend class RefCountedThreadSafe<ErrorCollector>;
  ~ErrorCollector();

  mutable simple_spinlock mutex_;
  CollectedErrors errors_ GUARDED_BY(mutex_);

  DISALLOW_COPY_AND_ASSIGN(ErrorCollector);
};

} // namespace internal
} // namespace client
} // namespace yb
#endif /* YB_CLIENT_ERROR_COLLECTOR_H */
