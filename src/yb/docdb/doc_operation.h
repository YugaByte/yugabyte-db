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

#ifndef YB_DOCDB_DOC_OPERATION_H_
#define YB_DOCDB_DOC_OPERATION_H_

#include <boost/container/small_vector.hpp>

#include "yb/common/common.pb.h"
#include "yb/common/pgsql_protocol.pb.h"
#include "yb/common/read_hybrid_time.h"

#include "yb/docdb/docdb_fwd.h"

#include "yb/util/monotime.h"
#include "yb/util/ref_cnt_buffer.h"

namespace yb {
namespace docdb {

struct DocOperationApplyData {
  DocWriteBatch* doc_write_batch;
  CoarseTimePoint deadline;
  ReadHybridTime read_time;
  HybridTime* restart_read_ht;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(deadline, read_time, restart_read_ht);
  }
};

// When specifiying the parent key, the constant -1 is used for the subkey index.
const int kNilSubkeyIndex = -1;

typedef boost::container::small_vector_base<RefCntPrefix> DocPathsToLock;

YB_DEFINE_ENUM(GetDocPathsMode, (kLock)(kIntents));
YB_DEFINE_ENUM(DocOperationType,
               (PGSQL_WRITE_OPERATION)(QL_WRITE_OPERATION)(REDIS_WRITE_OPERATION));

class DocOperation {
 public:
  typedef DocOperationType Type;

  virtual ~DocOperation() {}

  // Does the operation require a read snapshot to be taken before being applied? If so, a
  // clean snapshot hybrid_time will be supplied when Apply() is called. For example,
  // QLWriteOperation for a DML with a "... IF <condition> ..." clause needs to read the row to
  // evaluate the condition before the write and needs a read snapshot for a consistent read.
  virtual bool RequireReadSnapshot() const = 0;

  // Returns doc paths for this operation and isolation level this operation.
  // Doc paths are added to the end of paths, i.e. paths content is not cleared before it.
  //
  // Returned doc paths are controlled by mode argument:
  //   kLock - paths should be locked for this operation.
  //   kIntents - paths that should be used when writing intents, i.e. for conflict resolution.
  virtual CHECKED_STATUS GetDocPaths(
      GetDocPathsMode mode, DocPathsToLock *paths, IsolationLevel *level) const = 0;

  virtual CHECKED_STATUS Apply(const DocOperationApplyData& data) = 0;
  virtual Type OpType() = 0;
  virtual void ClearResponse() = 0;

  virtual std::string ToString() const = 0;
};

template <DocOperationType OperationType, class RequestPB>
class DocOperationBase : public DocOperation {
 public:
  Type OpType() override {
    return OperationType;
  }

  std::string ToString() const override {
    return Format("$0 { request: $1 }", OperationType, request_);
  }

 protected:
  RequestPB request_;
};

typedef std::vector<std::unique_ptr<DocOperation>> DocOperations;

}  // namespace docdb
}  // namespace yb

#endif // YB_DOCDB_DOC_OPERATION_H_
