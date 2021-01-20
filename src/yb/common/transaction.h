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

#ifndef YB_COMMON_TRANSACTION_H
#define YB_COMMON_TRANSACTION_H

#include <boost/container/small_vector.hpp>
#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/nil_generator.hpp>

#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
#include "yb/common/hybrid_time.h"

#include "yb/util/async_util.h"
#include "yb/util/enums.h"
#include "yb/util/monotime.h"
#include "yb/util/logging.h"
#include "yb/util/result.h"
#include "yb/util/strongly_typed_bool.h"
#include "yb/util/strongly_typed_uuid.h"
#include "yb/util/uuid.h"

namespace rocksdb {

class DB;

}

namespace yb {

YB_STRONGLY_TYPED_UUID(TransactionId);
using TransactionIdSet = std::unordered_set<TransactionId, TransactionIdHash>;

// Decodes transaction id from its binary representation.
// Checks that slice contains only TransactionId.
Result<TransactionId> FullyDecodeTransactionId(const Slice& slice);

// Decodes transaction id from slice which contains binary encoding. Consumes corresponding bytes
// from slice.
Result<TransactionId> DecodeTransactionId(Slice* slice);

struct TransactionStatusResult {
  TransactionStatus status;

  // Meaning of status_time is related to status value.
  // PENDING - status_time reflects maximal guaranteed PENDING time, i.e. transaction cannot be
  // committed before this time.
  // COMMITTED - status_time is a commit time.
  // ABORTED - not used.
  HybridTime status_time;

  TransactionStatusResult(TransactionStatus status_, HybridTime status_time_);

  static TransactionStatusResult Aborted() {
    return TransactionStatusResult(TransactionStatus::ABORTED, HybridTime());
  }

  std::string ToString() const {
    return Format("{ status: $0 status_time: $1 }", status, status_time);
  }
};

inline std::ostream& operator<<(std::ostream& out, const TransactionStatusResult& result) {
  return out << "{ status: " << TransactionStatus_Name(result.status)
             << " status_time: " << result.status_time << " }";
}

typedef std::function<void(Result<TransactionStatusResult>)> TransactionStatusCallback;
struct TransactionMetadata;

YB_DEFINE_ENUM(TransactionLoadFlag, (kMustExist)(kCleanup));
typedef EnumBitSet<TransactionLoadFlag> TransactionLoadFlags;

// Used by RequestStatusAt.
struct StatusRequest {
  const TransactionId* id;
  HybridTime read_ht;
  HybridTime global_limit_ht;
  int64_t serial_no;
  const std::string* reason;
  TransactionLoadFlags flags;
  TransactionStatusCallback callback;

  std::string ToString() const {
    return Format("{ id: $0 read_ht: $1 global_limit_ht: $2 serial_no: $3 reason: $4 flags: $5}",
                  *id, read_ht, global_limit_ht, serial_no, *reason, flags);
  }
};

class RequestScope;

class TransactionStatusManager {
 public:
  virtual ~TransactionStatusManager() {}

  // Checks whether this tablet knows that transaction is committed.
  // In case of success returns commit time of transaction, otherwise returns invalid time.
  virtual HybridTime LocalCommitTime(const TransactionId& id) = 0;

  // Fetches status of specified transaction at specified time from transaction coordinator.
  // Callback would be invoked in any case.
  // There are the following potential cases:
  // 1. Status tablet knows transaction id and could determine it's status at this time. In this
  // case status structure is filled with transaction status with corresponding status time.
  // 2. Status tablet don't know this transaction id, in this case status structure contains
  // ABORTED status.
  // 3. Status tablet could not determine transaction status at this time. In this case callback
  // will be invoked with TryAgain result.
  // 4. Any kind of network/timeout errors would be reflected in error passed to callback.
  virtual void RequestStatusAt(const StatusRequest& request) = 0;

  // Prepares metadata for provided protobuf. Either trying to extract it from pb, or fetch
  // from existing metadatas.
  virtual Result<TransactionMetadata> PrepareMetadata(const TransactionMetadataPB& pb) = 0;

  virtual void Abort(const TransactionId& id, TransactionStatusCallback callback) = 0;

  virtual void Cleanup(TransactionIdSet&& set) = 0;

  // For each pair fills second with priority of transaction with id equals to first.
  virtual void FillPriorities(
      boost::container::small_vector_base<std::pair<TransactionId, uint64_t>>* inout) = 0;

  // Returns minimal running hybrid time of all running transactions.
  virtual HybridTime MinRunningHybridTime() const = 0;

 private:
  friend class RequestScope;

  // Registers new request assigning next serial no to it. So this serial no could be used
  // to check whether one request happened before another one.
  virtual int64_t RegisterRequest() = 0;

  // request_id - is request id returned by RegisterRequest, that should be unregistered.
  virtual void UnregisterRequest(int64_t request_id) = 0;
};

// Utility class that invokes RegisterRequest on creation and UnregisterRequest on deletion.
class RequestScope {
 public:
  RequestScope() noexcept : status_manager_(nullptr), request_id_(0) {}

  explicit RequestScope(TransactionStatusManager* status_manager)
      : status_manager_(status_manager), request_id_(status_manager->RegisterRequest()) {
  }

  RequestScope(RequestScope&& rhs) noexcept
      : status_manager_(rhs.status_manager_), request_id_(rhs.request_id_) {
    rhs.status_manager_ = nullptr;
  }

  void operator=(RequestScope&& rhs) {
    Reset();
    status_manager_ = rhs.status_manager_;
    request_id_ = rhs.request_id_;
    rhs.status_manager_ = nullptr;
  }

  ~RequestScope() {
    Reset();
  }

  int64_t request_id() const { return request_id_; }

  RequestScope(const RequestScope&) = delete;
  void operator=(const RequestScope&) = delete;

 private:
  void Reset() {
    if (status_manager_) {
      status_manager_->UnregisterRequest(request_id_);
      status_manager_ = nullptr;
    }
  }

  TransactionStatusManager* status_manager_;
  int64_t request_id_;
};

struct TransactionOperationContext {
  TransactionOperationContext(
      const TransactionId& transaction_id_, TransactionStatusManager* txn_status_manager_)
      : transaction_id(transaction_id_),
        txn_status_manager(*(DCHECK_NOTNULL(txn_status_manager_))) {}

  bool transactional() const;

  TransactionId transaction_id;
  TransactionStatusManager& txn_status_manager;
};

typedef boost::optional<TransactionOperationContext> TransactionOperationContextOpt;

inline std::ostream& operator<<(std::ostream& out, const TransactionOperationContext& context) {
  if (context.transactional()) {
    out << context.transaction_id;
  } else {
    out << "<non transactional>";
  }
  return out;
}

struct TransactionMetadata {
  TransactionId transaction_id = TransactionId::Nil();
  IsolationLevel isolation = IsolationLevel::NON_TRANSACTIONAL;
  TabletId status_tablet;

  // By default, a random value is picked for a newly created transaction.
  uint64_t priority = 0;

  // Used for snapshot isolation (as read time and for conflict resolution).
  // start_time is used only for backward compability during rolling update.
  HybridTime start_time;

  static Result<TransactionMetadata> FromPB(const TransactionMetadataPB& source);

  void ToPB(TransactionMetadataPB* dest) const;

  void TransactionIdToPB(TransactionMetadataPB* dest) const;

  // Fill dest with full metadata even when isolation is non transactional.
  void ForceToPB(TransactionMetadataPB* dest) const;

  std::string ToString() const {
    return Format(
        "{ transaction_id: $0 isolation: $1 status_tablet: $2 priority: $3 start_time: $4 }",
        transaction_id, IsolationLevel_Name(isolation), status_tablet, priority, start_time);
  }
};

bool operator==(const TransactionMetadata& lhs, const TransactionMetadata& rhs);

inline bool operator!=(const TransactionMetadata& lhs, const TransactionMetadata& rhs) {
  return !(lhs == rhs);
}

std::ostream& operator<<(std::ostream& out, const TransactionMetadata& metadata);

MonoDelta TransactionRpcTimeout();
CoarseTimePoint TransactionRpcDeadline();

extern const std::string kTransactionsTableName;
extern const std::string kMetricsSnapshotsTableName;

YB_DEFINE_ENUM(CleanupType, (kGraceful)(kImmediate))

} // namespace yb

#endif // YB_COMMON_TRANSACTION_H
