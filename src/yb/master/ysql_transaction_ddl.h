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

#ifndef YB_MASTER_YSQL_TRANSACTION_DDL_H
#define YB_MASTER_YSQL_TRANSACTION_DDL_H

#include <functional>
#include <memory>

#include "yb/common/entity_ids.h"
#include "yb/common/transaction.h"
#include "yb/rpc/rpc.h"
#include "yb/util/result.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"

namespace yb {
namespace tserver {
class GetTransactionStatusResponsePB;
}
namespace master {
class CatalogManager;
class Master;

class YsqlTransactionDdl {
 public:
  explicit YsqlTransactionDdl(CatalogManager* catalog_manager,
                              Master* master)
      : catalog_manager_(catalog_manager),
        master_(master),
        thread_pool_(nullptr) { }
  ~YsqlTransactionDdl();

  void set_thread_pool(yb::ThreadPool* thread_pool) {
    thread_pool_ = thread_pool;
  }

  void VerifyTransaction(TransactionMetadata transaction,
                         std::function<Status(bool /* is_success */)> complete_callback);
  Result<bool> PgEntryExists(TableId tableId, Result<uint32_t> entry_oid);
 protected:
  void TransactionReceived(TransactionMetadata transaction,
                           std::function<Status(bool)> complete_callback,
                           Status txn_status,
                           const tserver::GetTransactionStatusResponsePB& response);

  CatalogManager* catalog_manager_;
  Master *master_;
  yb::rpc::Rpcs rpcs_;
  yb::ThreadPool* thread_pool_;
};

}  // namespace master
}  // namespace yb
#endif // YB_MASTER_YSQL_TRANSACTION_DDL_H
