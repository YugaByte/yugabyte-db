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

#include "yb/yql/pggate/pggate.h"
#include "yb/yql/pggate/pggate_flags.h"
#include "yb/yql/pggate/pg_txn_manager.h"

#include "yb/client/session.h"
#include "yb/client/transaction.h"

#include "yb/common/common.pb.h"
#include "yb/common/transaction_priority.h"

#include "yb/tserver/tserver_shared_mem.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/random_util.h"
#include "yb/util/status.h"

namespace {

constexpr uint64_t txn_priority_highpri_upper_bound = yb::kHighPriTxnUpperBound;
constexpr uint64_t txn_priority_highpri_lower_bound = yb::kHighPriTxnLowerBound;

// Local copies that can be modified.
uint64_t txn_priority_regular_upper_bound = yb::kRegularTxnUpperBound;
uint64_t txn_priority_regular_lower_bound = yb::kRegularTxnLowerBound;

// Converts double value in range 0..1 to uint64_t value in range
// 0..(txn_priority_highpri_lower_bound - 1)
uint64_t ConvertBound(double value) {
  if (value <= 0.0) {
    return 0;
  }
  if (value >= 1.0) {
    return txn_priority_highpri_lower_bound - 1;
  }
  // Have to cast to double to avoid a warning on implicit cast that changes the value.
  return value * (static_cast<double>(txn_priority_highpri_lower_bound) - 1);
}

} // namespace

extern "C" {

void YBCAssignTransactionPriorityLowerBound(double newval, void* extra) {
  txn_priority_regular_lower_bound = ConvertBound(newval);
  // YSQL layer checks (guc.c) should ensure this.
  DCHECK_LE(txn_priority_regular_lower_bound, txn_priority_regular_upper_bound);
}

void YBCAssignTransactionPriorityUpperBound(double newval, void* extra) {
  txn_priority_regular_upper_bound = ConvertBound(newval);
  // YSQL layer checks (guc.c) should ensure this.
  DCHECK_LE(txn_priority_regular_lower_bound, txn_priority_regular_upper_bound);
}

}

using namespace std::literals;
using namespace std::placeholders;


namespace yb {
namespace pggate {

using client::YBTransaction;
using client::AsyncClientInitialiser;
using client::TransactionManager;
using client::YBTransactionPtr;
using client::YBSession;
using client::YBSessionPtr;
using client::LocalTabletFilter;

PgTxnManager::PgTxnManager(
    AsyncClientInitialiser* async_client_init,
    scoped_refptr<ClockBase> clock,
    const tserver::TServerSharedObject* tserver_shared_object)
    : async_client_init_(async_client_init),
      clock_(std::move(clock)),
      tserver_shared_object_(tserver_shared_object) {
}

PgTxnManager::~PgTxnManager() {
  // Abort the transaction before the transaction manager gets destroyed.
  if (txn_) {
    txn_->Abort();
  }
  ResetTxnAndSession();
}

Status PgTxnManager::BeginTransaction() {
  VLOG(2) << "BeginTransaction: txn_in_progress_=" << txn_in_progress_;
  if (txn_in_progress_) {
    return STATUS(IllegalState, "Transaction is already in progress");
  }
  return RecreateTransaction(SavePriority::kFalse /* save_priority */);
}

Status PgTxnManager::RecreateTransaction() {
  VLOG(2) << "RecreateTransaction: txn_in_progress_=" << txn_in_progress_;
  if (!txn_) {
    return Status::OK();
  }
  return RecreateTransaction(SavePriority::kTrue /* save_priority */);
}

Status PgTxnManager::RecreateTransaction(const SavePriority save_priority) {
  use_saved_priority_ = save_priority;
  if (save_priority) {
    saved_priority_ = txn_->GetPriority();
  }

  ResetTxnAndSession();
  txn_in_progress_ = true;
  StartNewSession();
  return Status::OK();
}

Status PgTxnManager::SetIsolationLevel(int level) {
  isolation_level_ = static_cast<PgIsolationLevel>(level);
  return Status::OK();
}

Status PgTxnManager::SetReadOnly(bool read_only) {
  read_only_ = read_only;
  return Status::OK();
}

Status PgTxnManager::SetDeferrable(bool deferrable) {
  deferrable_ = deferrable;
  return Status::OK();
}

void PgTxnManager::StartNewSession() {
  session_ = std::make_shared<YBSession>(async_client_init_->client(), clock_);
  session_->SetReadPoint(client::Restart::kFalse);
  session_->SetForceConsistentRead(client::ForceConsistentRead::kTrue);
}

uint64_t PgTxnManager::GetPriority(const NeedsPessimisticLocking needs_pessimistic_locking) {
  if (use_saved_priority_) {
    return saved_priority_;
  }

  // Use high priority for transactions that need pessimistic locking.
  if (needs_pessimistic_locking) {
    return RandomUniformInt(txn_priority_highpri_lower_bound,
                            txn_priority_highpri_upper_bound);
  }
  return RandomUniformInt(txn_priority_regular_lower_bound,
                          txn_priority_regular_upper_bound);
}

Status PgTxnManager::BeginWriteTransactionIfNecessary(bool read_only_op,
                                                      bool needs_pessimistic_locking) {
  if (ddl_txn_) {
    return Status::OK();
  }

  VLOG(2) << "BeginWriteTransactionIfNecessary: txn_in_progress_="
          << txn_in_progress_ << ", txn_=" << txn_.get();

  // Using Postgres isolation_level_, read_only_, and deferrable_, determine the internal isolation
  // level and defer effect.
  IsolationLevel isolation = (isolation_level_ == PgIsolationLevel::SERIALIZABLE) && !read_only_
      ? IsolationLevel::SERIALIZABLE_ISOLATION : IsolationLevel::SNAPSHOT_ISOLATION;
  bool defer = read_only_ && deferrable_;

  if (txn_) {
    // Sanity check: query layer should ensure that this does not happen.
    if (txn_->isolation() != isolation) {
      return STATUS(IllegalState, "Changing txn isolation level in the middle of a transaction");
    }
  } else if (read_only_op && isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
    if (defer) {
      // This call is idempotent, meaning it has no affect after the first call.
      session_->DeferReadPoint();
    }
  } else {
    if (tserver_shared_object_) {
      if (!tablet_server_proxy_) {
        LOG(INFO) << "Using TServer endpoint: " << (**tserver_shared_object_).endpoint();
        tablet_server_proxy_ = std::make_unique<tserver::TabletServerServiceProxy>(
          &async_client_init_->client()->proxy_cache(),
          HostPort((**tserver_shared_object_).endpoint()));
      }
      tserver::TakeTransactionRequestPB req;
      tserver::TakeTransactionResponsePB resp;
      rpc::RpcController controller;
      // TODO(dtxn) propagate timeout from higher level
      controller.set_timeout(10s);
      RETURN_NOT_OK(tablet_server_proxy_->TakeTransaction(req, &resp, &controller));
      txn_ = YBTransaction::Take(
          GetOrCreateTransactionManager(),
          VERIFY_RESULT(TransactionMetadata::FromPB(resp.metadata())));
    } else {
      txn_ = std::make_shared<YBTransaction>(GetOrCreateTransactionManager());
    }

    txn_->SetPriority(GetPriority(NeedsPessimisticLocking(needs_pessimistic_locking)));

    if (isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
      txn_->InitWithReadPoint(isolation, std::move(*session_->read_point()));
    } else {
      RETURN_NOT_OK(txn_->Init(isolation));
    }
    session_->SetTransaction(txn_);
  }
  return Status::OK();
}

Status PgTxnManager::RestartTransaction() {
  if (!txn_in_progress_ || !txn_) {
    CHECK_NOTNULL(session_);
    if (!session_->IsRestartRequired()) {
      return STATUS(IllegalState, "Attempted to restart when session does not require restart");
    }
    session_->SetReadPoint(client::Restart::kTrue);
    return Status::OK();
  }
  if (!txn_->IsRestartRequired()) {
    return STATUS(IllegalState, "Attempted to restart when transaction does not require restart");
  }
  txn_ = VERIFY_RESULT(txn_->CreateRestartedTransaction());
  session_->SetTransaction(txn_);

  DCHECK(can_restart_.load(std::memory_order_acquire));

  return Status::OK();
}

Status PgTxnManager::CommitTransaction() {
  if (!txn_in_progress_) {
    VLOG(2) << "No transaction in progress, nothing to commit.";
    return Status::OK();
  }

  if (!txn_) {
    VLOG(2) << "This was a read-only transaction, nothing to commit.";
    ResetTxnAndSession();
    return Status::OK();
  }
  VLOG(2) << "Committing transaction.";
  Status status = txn_->CommitFuture().get();
  VLOG(2) << "Transaction commit status: " << status;
  ResetTxnAndSession();
  return status;
}

Status PgTxnManager::AbortTransaction() {
  // If a DDL operation during a DDL txn fails the txn will be aborted before we get here.
  // However if there are failures afterwards (i.e. during COMMIT or catalog version increment),
  // then we might get here with a ddl_txn_. Clean it up in that case.
  if (ddl_txn_) {
    RETURN_NOT_OK(ExitSeparateDdlTxnMode(false));
  }

  if (!txn_in_progress_) {
    return Status::OK();
  }
  if (!txn_) {
    // This was a read-only transaction, nothing to commit.
    ResetTxnAndSession();
    return Status::OK();
  }
  // TODO: how do we report errors if the transaction has already committed?
  txn_->Abort();
  ResetTxnAndSession();
  return Status::OK();
}

// TODO: dedup with similar logic in CQLServiceImpl.
// TODO: do we need lazy initialization of the txn manager?
TransactionManager* PgTxnManager::GetOrCreateTransactionManager() {
  auto result = transaction_manager_.load(std::memory_order_acquire);
  if (result) {
    return result;
  }
  std::lock_guard<decltype(transaction_manager_mutex_)> lock(transaction_manager_mutex_);
  if (transaction_manager_holder_) {
    return transaction_manager_holder_.get();
  }

  transaction_manager_holder_ = std::make_unique<client::TransactionManager>(
      async_client_init_->client(), clock_, LocalTabletFilter());

  transaction_manager_.store(transaction_manager_holder_.get(), std::memory_order_release);
  return transaction_manager_holder_.get();
}

Result<client::YBSession*> PgTxnManager::GetTransactionalSession() {
  if (ddl_session_) {
    VLOG(2) << "Using the DDL session: " << ddl_session_.get();
    return ddl_session_.get();
  }
  if (!txn_in_progress_) {
    RETURN_NOT_OK(BeginTransaction());
  }
  VLOG(2) << "Using the non-DDL transactional session: " << session_.get();
  return session_.get();
}

std::shared_future<Result<TransactionMetadata>> PgTxnManager::GetDdlTxnMetadata() const {
  return ddl_txn_->GetMetadata();
}

void PgTxnManager::ResetTxnAndSession() {
  txn_in_progress_ = false;
  session_ = nullptr;
  txn_ = nullptr;
  can_restart_.store(true, std::memory_order_release);
}

Status PgTxnManager::EnterSeparateDdlTxnMode() {
  RSTATUS_DCHECK(!ddl_txn_,
          IllegalState, "EnterSeparateDdlTxnMode called when already in a DDL transaction");
  VLOG(2) << __PRETTY_FUNCTION__;
  ddl_session_ = std::make_shared<YBSession>(async_client_init_->client(), clock_);
  ddl_session_->SetForceConsistentRead(client::ForceConsistentRead::kTrue);
  ddl_txn_ = std::make_shared<YBTransaction>(GetOrCreateTransactionManager());
  ddl_session_->SetTransaction(ddl_txn_);
  RETURN_NOT_OK(ddl_txn_->Init(
      FLAGS_ysql_serializable_isolation_for_ddl_txn ? IsolationLevel::SERIALIZABLE_ISOLATION
                                                    : IsolationLevel::SNAPSHOT_ISOLATION));
  VLOG(2) << __PRETTY_FUNCTION__ << ": ddl_txn_=" << ddl_txn_.get();
  return Status::OK();
}

Status PgTxnManager::ExitSeparateDdlTxnMode(bool is_success) {
  VLOG(2) << __PRETTY_FUNCTION__ << ": ddl_txn_=" << ddl_txn_.get() << ",is_success=" << is_success;
  RSTATUS_DCHECK(!!ddl_txn_,
          IllegalState, "ExitSeparateDdlTxnMode called when not in a DDL transaction");
  if (is_success) {
    RETURN_NOT_OK(ddl_txn_->CommitFuture().get());
  } else {
    ddl_txn_->Abort();
  }
  ddl_txn_.reset();
  ddl_session_.reset();
  return Status::OK();
}

}  // namespace pggate
}  // namespace yb
