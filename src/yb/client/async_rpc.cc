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

#include "yb/client/async_rpc.h"
#include "yb/client/batcher.h"
#include "yb/client/client.h"
#include "yb/client/client_error.h"
#include "yb/client/client-internal.h"
#include "yb/client/in_flight_op.h"
#include "yb/client/meta_cache.h"
#include "yb/client/table.h"
#include "yb/client/yb_op.h"

#include "yb/common/pgsql_error.h"
#include "yb/common/transaction.h"
#include "yb/common/transaction_error.h"
#include "yb/common/wire_protocol.h"

#include "yb/gutil/strings/substitute.h"

#include "yb/util/cast.h"
#include "yb/util/debug-util.h"
#include "yb/util/logging.h"
#include "yb/util/yb_pg_errcodes.h"

// TODO: do we need word Redis in following two metrics? ReadRpc and WriteRpc objects emitting
// these metrics are used not only in Redis service.
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_write_remote, "yb.client.Write remote call time",
    yb::MetricUnit::kMicroseconds, "Microseconds spent in the remote Write call ", 60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_read_remote, "yb.client.Read remote call time",
    yb::MetricUnit::kMicroseconds, "Microseconds spent in the remote Read call ", 60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_write_local, "yb.client.Write local call time",
    yb::MetricUnit::kMicroseconds, "Microseconds spent in the local Write call ", 60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_read_local, "yb.client.Read local call time",
    yb::MetricUnit::kMicroseconds, "Microseconds spent in the local Read call ", 60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_time_to_send,
    "Time taken for a Write/Read rpc to be sent to the server", yb::MetricUnit::kMicroseconds,
    "Microseconds spent before sending the request to the server", 60000000LU, 2);

METRIC_DEFINE_counter(server, consistent_prefix_successful_reads,
    "Number of consistent prefix reads that were served by the closest replica.",
    yb::MetricUnit::kRequests,
    "Number of consistent prefix reads that were served by the closest replica.");

METRIC_DEFINE_counter(server, consistent_prefix_failed_reads,
    "Number of consistent prefix reads that failed to be served by the closest replica.",
    yb::MetricUnit::kRequests,
    "Number of consistent prefix reads that failed to be served by the closest replica.");

DECLARE_bool(rpc_dump_all_traces);
DECLARE_bool(collect_end_to_end_traces);

DEFINE_bool(forward_redis_requests, true, "If false, the redis op will not be served if it's not "
            "a local request. The op response will be set to the redis error "
            "'-MOVED partition_key 0.0.0.0:0'. This works with jedis which only looks at the MOVED "
            "part of the reply and ignores the rest. For now, if this flag is true, we will only "
            "attempt to read from leaders, so redis_allow_reads_from_followers will be ignored.");

DEFINE_bool(detect_duplicates_for_retryable_requests, true,
            "Enable tracking of write requests that prevents the same write from being applied "
                "twice.");

DEFINE_CAPABILITY(PickReadTimeAtTabletServer, 0x8284d67b);

using namespace std::placeholders;

namespace yb {

using std::shared_ptr;
using rpc::ErrorStatusPB;
using rpc::Messenger;
using rpc::Rpc;
using rpc::RpcController;
using tserver::WriteRequestPB;
using tserver::WriteResponsePB;
using tserver::WriteResponsePB_PerRowErrorPB;
using strings::Substitute;

namespace client {

namespace internal {

bool IsTracingEnabled() {
  return FLAGS_collect_end_to_end_traces;
}

namespace {

bool LocalTabletServerOnly(const InFlightOps& ops) {
  const auto op_type = ops.front()->yb_op->type();
  return ((op_type == YBOperation::Type::REDIS_READ || op_type == YBOperation::Type::REDIS_WRITE) &&
          !FLAGS_forward_redis_requests);
}

}

AsyncRpcMetrics::AsyncRpcMetrics(const scoped_refptr<yb::MetricEntity>& entity)
    : remote_write_rpc_time(METRIC_handler_latency_yb_client_write_remote.Instantiate(entity)),
      remote_read_rpc_time(METRIC_handler_latency_yb_client_read_remote.Instantiate(entity)),
      local_write_rpc_time(METRIC_handler_latency_yb_client_write_local.Instantiate(entity)),
      local_read_rpc_time(METRIC_handler_latency_yb_client_read_local.Instantiate(entity)),
      time_to_send(METRIC_handler_latency_yb_client_time_to_send.Instantiate(entity)),
      consistent_prefix_successful_reads(
          METRIC_consistent_prefix_successful_reads.Instantiate(entity)),
      consistent_prefix_failed_reads(METRIC_consistent_prefix_failed_reads.Instantiate(entity)) {
}

AsyncRpc::AsyncRpc(AsyncRpcData* data, YBConsistencyLevel yb_consistency_level)
    : Rpc(data->batcher->deadline(), data->batcher->messenger(), &data->batcher->proxy_cache()),
      batcher_(data->batcher),
      trace_(new Trace),
      ops_(std::move(data->ops)),
      tablet_invoker_(LocalTabletServerOnly(ops_),
                      yb_consistency_level == YBConsistencyLevel::CONSISTENT_PREFIX,
                      data->batcher->client_,
                      this,
                      this,
                      data->tablet,
                      table(),
                      mutable_retrier(),
                      trace_.get()),
      start_(MonoTime::Now()),
      async_rpc_metrics_(data->batcher->async_rpc_metrics()) {

  mutable_retrier()->mutable_controller()->set_allow_local_calls_in_curr_thread(
      data->allow_local_calls_in_curr_thread);
  if (Trace::CurrentTrace()) {
    Trace::CurrentTrace()->AddChildTrace(trace_.get());
  }
}

AsyncRpc::~AsyncRpc() {
  if (PREDICT_FALSE(FLAGS_rpc_dump_all_traces)) {
    LOG(INFO) << ToString() << " took "
              << MonoTime::Now().GetDeltaSince(start_).ToMicroseconds()
              << "us. Trace:";
    trace_->Dump(&LOG(INFO), true);
  }
}

void AsyncRpc::SendRpc() {
  TRACE_TO(trace_, "SendRpc() called.");

  retained_self_ = shared_from_this();
  // For now, if this is a retry, execute this rpc on the leader even if
  // the consistency level is YBConsistencyLevel::CONSISTENT_PREFIX or
  // FLAGS_redis_allow_reads_from_followers is set to true.
  // TODO(hector): Temporarily blacklist the follower that couldn't serve the read so we can retry
  // on another follower.
  if (async_rpc_metrics_ && num_attempts() > 1 && tablet_invoker_.is_consistent_prefix()) {
    IncrementCounter(async_rpc_metrics_->consistent_prefix_failed_reads);
  }
  tablet_invoker_.Execute(std::string(), num_attempts() > 1);
}

std::string AsyncRpc::ToString() const {
  return Format("$0(tablet: $1, num_ops: $2, num_attempts: $3, txn: $4)",
                ops_.front()->yb_op->read_only() ? "Read" : "Write",
                tablet().tablet_id(), ops_.size(), num_attempts(),
                batcher_->transaction_metadata().transaction_id);
}

std::shared_ptr<const YBTable> AsyncRpc::table() const {
  // All of the ops for a given tablet obviously correspond to the same table,
  // so we'll just grab the table from the first.
  return ops_[0]->yb_op->table();
}

void AsyncRpc::Finished(const Status& status) {
  Status new_status = status;
  if (tablet_invoker_.Done(&new_status)) {
    if (tablet().is_split() ||
        ClientError(new_status) == ClientErrorCode::kTablePartitionsAreStale) {
      ops_[0]->yb_op->MarkTablePartitionsAsStale();
    }
    if (async_rpc_metrics_ && status.ok() && tablet_invoker_.is_consistent_prefix()) {
      IncrementCounter(async_rpc_metrics_->consistent_prefix_successful_reads);
    }
    ProcessResponseFromTserver(new_status);
    batcher_->RemoveInFlightOpsAfterFlushing(ops_, new_status, MakeFlushExtraResult());
    batcher_->CheckForFinishedFlush();
    retained_self_.reset();
  }
}

void AsyncRpc::Failed(const Status& status) {
  std::string error_message = status.message().ToBuffer();
  auto redis_error_code = status.IsInvalidCommand() || status.IsInvalidArgument() ?
      RedisResponsePB_RedisStatusCode_PARSING_ERROR : RedisResponsePB_RedisStatusCode_SERVER_ERROR;
  for (auto op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_WRITE: {
        RedisResponsePB* resp;
        if (yb_op->type() == YBOperation::Type::REDIS_READ) {
          resp = down_cast<YBRedisReadOp*>(yb_op)->mutable_response();
        } else {
          resp = down_cast<YBRedisWriteOp*>(yb_op)->mutable_response();
        }
        resp->Clear();
        // If the tserver replied it is not the leader, respond that the key has moved. We do not
        // need to return the address of the new leader because the Redis client will refresh the
        // cluster map instead.
        if (status.IsIllegalState()) {
          resp->set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
          resp->set_error_message(Substitute("MOVED $0 0.0.0.0:0",
                                             down_cast<YBRedisOp*>(yb_op)->hash_code()));
        } else {
          resp->set_code(redis_error_code);
          resp->set_error_message(error_message);
        }
        break;
      }
      case YBOperation::Type::QL_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_WRITE: {
        QLResponsePB* resp = down_cast<YBqlOp*>(yb_op)->mutable_response();
        resp->Clear();
        resp->set_status(status.IsTryAgain() ? QLResponsePB::YQL_STATUS_RESTART_REQUIRED_ERROR
                                             : QLResponsePB::YQL_STATUS_RUNTIME_ERROR);
        resp->set_error_message(error_message);
        break;
      }
      case YBOperation::Type::PGSQL_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::PGSQL_WRITE: {
        PgsqlResponsePB* resp = down_cast<YBPgsqlOp*>(yb_op)->mutable_response();
        resp->set_status(status.IsTryAgain() ? PgsqlResponsePB::PGSQL_STATUS_RESTART_REQUIRED_ERROR
                                             : PgsqlResponsePB::PGSQL_STATUS_RUNTIME_ERROR);
        resp->set_error_message(error_message);
        const uint8_t* pg_err_ptr = status.ErrorData(PgsqlErrorTag::kCategory);
        if (pg_err_ptr != nullptr) {
          resp->set_pg_error_code(static_cast<uint32_t>(PgsqlErrorTag::Decode(pg_err_ptr)));
        } else {
          resp->set_pg_error_code(static_cast<uint32_t>(YBPgErrorCode::YB_PG_INTERNAL_ERROR));
        }
        const uint8_t* txn_err_ptr = status.ErrorData(TransactionErrorTag::kCategory);
        if (txn_err_ptr != nullptr) {
          resp->set_txn_error_code(static_cast<uint16_t>(TransactionErrorTag::Decode(txn_err_ptr)));
        } else {
          resp->set_txn_error_code(static_cast<uint16_t>(TransactionErrorCode::kNone));
        }
        break;
      }
      default:
        LOG(FATAL) << "Unsupported operation " << yb_op->type();
        break;
    }
  }
}

bool AsyncRpc::IsLocalCall() const {
  return tablet_invoker_.IsLocalCall();
}

namespace {

template<class T>
void SetTransactionMetadata(const TransactionMetadata& metadata,
                            bool need_full_metadata,
                            T* dest) {
  auto* transaction = dest->mutable_transaction();
  if (need_full_metadata) {
    metadata.ToPB(transaction);
  } else {
    metadata.TransactionIdToPB(transaction);
  }
  dest->set_deprecated_may_have_metadata(true);
}

void SetTransactionMetadata(const TransactionMetadata& metadata,
                            bool need_full_metadata,
                            tserver::WriteRequestPB* req) {
  SetTransactionMetadata(metadata, need_full_metadata, req->mutable_write_batch());
}

} // namespace

void AsyncRpc::SendRpcToTserver(int attempt_num) {
  MonoTime end_time = MonoTime::Now();
  if (async_rpc_metrics_) {
    async_rpc_metrics_->time_to_send->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
  }

  CallRemoteMethod();
}

template <class Req, class Resp>
AsyncRpcBase<Req, Resp>::AsyncRpcBase(AsyncRpcData* data,
                                      YBConsistencyLevel consistency_level)
    : AsyncRpc(data, consistency_level) {

  req_.set_tablet_id(tablet_invoker_.tablet()->tablet_id());
  req_.set_include_trace(IsTracingEnabled());
  const ConsistentReadPoint* read_point = batcher_->read_point();
  bool has_read_time = false;
  if (read_point) {
    req_.set_propagated_hybrid_time(read_point->Now().ToUint64());
    // Set read time for consistent read only if the table is transaction-enabled and
    // consistent read is required.
    if (data->need_consistent_read &&
        table()->InternalSchema().table_properties().is_transactional()) {
      auto read_time = read_point->GetReadTime(tablet_invoker_.tablet()->tablet_id());
      if (read_time) {
        has_read_time = true;
        read_time.AddToPB(&req_);
      }
    }
  }
  if (!ops_.empty()) {
    req_.set_batch_idx(ops_.front()->batch_idx);
  }
  auto& transaction_metadata = batcher_->transaction_metadata();
  if (!transaction_metadata.transaction_id.IsNil()) {
    SetTransactionMetadata(transaction_metadata, data->need_metadata, &req_);
    bool serializable = transaction_metadata.isolation == IsolationLevel::SERIALIZABLE_ISOLATION;
    LOG_IF(DFATAL, has_read_time && serializable)
        << "Read time should NOT be specified for serializable isolation: "
        << read_point->GetReadTime().ToString();
  }
}

template <class Req, class Resp>
bool AsyncRpcBase<Req, Resp>::CommonResponseCheck(const Status& status) {
  if (!status.ok()) {
    return false;
  }
  if (resp_.has_error()) {
    LOG(WARNING) << ToString() << " has error:" << resp_.error().DebugString()
                 << ". Requests not processed.";
    // If there is an error at the Rpc itself, there should be no individual responses.
    // All of them need to be marked as failed.
    Failed(StatusFromPB(resp_.error().status()));
    return false;
  }
  auto restart_read_time = ReadHybridTime::FromRestartReadTimePB(resp_);
  if (restart_read_time) {
    auto read_point = batcher_->read_point();
    if (read_point) {
      read_point->RestartRequired(req_.tablet_id(), restart_read_time);
    }
    Failed(STATUS(TryAgain, Format("Restart read required at: $0", restart_read_time), Slice(),
                  TransactionError(TransactionErrorCode::kReadRestartRequired)));
    return false;
  }
  return true;
}

template <class Req, class Resp>
void AsyncRpcBase<Req, Resp>::SendRpcToTserver(int attempt_num) {
  if (!tablet_invoker_.current_ts().HasCapability(CAPABILITY_PickReadTimeAtTabletServer)) {
    ConsistentReadPoint* read_point = batcher_->read_point();
    if (read_point && !read_point->GetReadTime()) {
      auto txn = batcher_->transaction();
      // If txn is not set, this is a consistent scan across multiple tablets of a
      // non-transactional YCQL table.
      if (!txn || txn->isolation() == IsolationLevel::SNAPSHOT_ISOLATION) {
        read_point->SetCurrentReadTime();
        read_point->GetReadTime().AddToPB(&req_);
      }
    }
  }

  req_.set_rejection_score(batcher_->RejectionScore(attempt_num));
  AsyncRpc::SendRpcToTserver(attempt_num);
}

WriteRpc::WriteRpc(AsyncRpcData* data)
    : AsyncRpcBase(data, YBConsistencyLevel::STRONG) {

  TRACE_TO(trace_, "WriteRpc initiated to $0", data->tablet->tablet_id());

  if (data->write_time_for_backfill_.is_valid()) {
    req_.set_external_hybrid_time(data->write_time_for_backfill_.ToUint64());
    ReadHybridTime::SingleTime(data->write_time_for_backfill_).ToPB(req_.mutable_read_time());
  }
  // Add the rows
  int ctr = 0;
  for (auto& op : ops_) {
    // Move write request PB into tserver write request PB for performance.
    // Will restore in ProcessResponseFromTserver.
    switch (op->yb_op->type()) {
      case YBOperation::Type::REDIS_WRITE: {
        CHECK_EQ(table()->table_type(), YBTableType::REDIS_TABLE_TYPE);
        auto* redis_op = down_cast<YBRedisWriteOp*>(op->yb_op.get());
        req_.add_redis_write_batch()->Swap(redis_op->mutable_request());
        break;
      }
      case YBOperation::Type::QL_WRITE: {
        CHECK_EQ(table()->table_type(), YBTableType::YQL_TABLE_TYPE);
        auto* ql_op = down_cast<YBqlWriteOp*>(op->yb_op.get());
        req_.add_ql_write_batch()->Swap(ql_op->mutable_request());
        break;
      }
      case YBOperation::Type::PGSQL_WRITE: {
        CHECK_EQ(table()->table_type(), YBTableType::PGSQL_TABLE_TYPE);
        auto* pgsql_op = down_cast<YBPgsqlWriteOp*>(op->yb_op.get());
        req_.add_pgsql_write_batch()->Swap(pgsql_op->mutable_request());
        if (pgsql_op->write_time()) {
          req_.set_external_hybrid_time(pgsql_op->write_time().ToUint64());
        }
        break;
      }
      case YBOperation::Type::PGSQL_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_READ:
        LOG(FATAL) << "Not a write operation " << op->yb_op->type();
        break;
      default:
        LOG(FATAL) << "Unsupported write operation " << op->yb_op->type();
        break;
    }

    // Set the state now, even though we haven't yet sent it -- at this point
    // there is no return, and we're definitely going to send it. If we waited
    // until after we sent it, the RPC callback could fire before we got a chance
    // to change its state to 'sent'.
    op->state = InFlightOpState::kRequestSent;
    VLOG(4) << ++ctr << ". Encoded row " << op->yb_op->ToString();
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Created batch for " << data->tablet->tablet_id() << ":\n"
            << req_.ShortDebugString();
  }

  const auto& client_id = batcher_->client_id();
  if (!client_id.IsNil() && FLAGS_detect_duplicates_for_retryable_requests) {
    auto temp = client_id.ToUInt64Pair();
    req_.set_client_id1(temp.first);
    req_.set_client_id2(temp.second);
    auto request_pair = batcher_->NextRequestIdAndMinRunningRequestId(data->tablet->tablet_id());
    req_.set_request_id(request_pair.first);
    req_.set_min_running_request_id(request_pair.second);
  }
}

WriteRpc::~WriteRpc() {
  // Check that we sent request id info, i.e. (client_id, request_id, min_running_request_id).
  if (req_.has_client_id1()) {
    batcher_->RequestFinished(tablet().tablet_id(), req_.request_id());
  }

  MonoTime end_time = MonoTime::Now();
  if (async_rpc_metrics_) {
    scoped_refptr<Histogram> write_rpc_time = IsLocalCall() ?
                                              async_rpc_metrics_->local_write_rpc_time :
                                              async_rpc_metrics_->remote_write_rpc_time;
    write_rpc_time->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
  }
}

void WriteRpc::CallRemoteMethod() {
  auto trace = trace_; // It is possible that we receive reply before returning from WriteAsync.
                       // Since send happens before we return from WriteAsync.
                       // So under heavy load it is possible that our request is handled and
                       // reply is received before WriteAsync returned.
  TRACE_TO(trace, "SendRpcToTserver");
  ADOPT_TRACE(trace.get());

  tablet_invoker_.proxy()->WriteAsync(
      req_, &resp_, PrepareController(),
      std::bind(&WriteRpc::Finished, this, Status::OK()));
  TRACE_TO(trace, "RpcDispatched Asynchronously");
}

void WriteRpc::SwapRequestsAndResponses(bool skip_responses = false) {
  size_t redis_idx = 0;
  size_t ql_idx = 0;
  size_t pgsql_idx = 0;

  for (auto& op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_WRITE: {
        // Restore Redis write request PB.
        auto* redis_op = down_cast<YBRedisWriteOp*>(yb_op);
        redis_op->mutable_request()->Swap(req_.mutable_redis_write_batch(redis_idx));
        redis_idx++;
        break;
      }
      case YBOperation::Type::QL_WRITE: {
        // Restore QL write request PB.
        auto* ql_op = down_cast<YBqlWriteOp*>(yb_op);
        ql_op->mutable_request()->Swap(req_.mutable_ql_write_batch(ql_idx));
        ql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_WRITE: {
        // Restore QL write request PB.
        auto* pgsql_op = down_cast<YBPgsqlWriteOp*>(yb_op);
        pgsql_op->mutable_request()->Swap(req_.mutable_pgsql_write_batch(pgsql_idx));
        pgsql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_READ:
        LOG(FATAL) << "Not a write operation " << op->yb_op->type();
        break;
    }
  }

  if (skip_responses) return;

  // Retrieve Redis and QL responses and make sure we received all the responses back.
  redis_idx = 0;
  ql_idx = 0;
  pgsql_idx = 0;
  for (auto& op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_WRITE: {
        if (redis_idx >= resp_.redis_response_batch().size()) {
          ++redis_idx;
          continue;
        }
        // Restore Redis write request PB and extract response.
        auto* redis_op = down_cast<YBRedisWriteOp*>(yb_op);
        redis_op->mutable_response()->Swap(resp_.mutable_redis_response_batch(redis_idx));
        redis_idx++;
        break;
      }
      case YBOperation::Type::QL_WRITE: {
        if (ql_idx >= resp_.ql_response_batch().size()) {
          ++ql_idx;
          continue;
        }
        // Restore QL write request PB and extract response.
        auto* ql_op = down_cast<YBqlWriteOp*>(yb_op);
        ql_op->mutable_response()->Swap(resp_.mutable_ql_response_batch(ql_idx));
        const auto& ql_response = ql_op->response();
        if (ql_response.has_rows_data_sidecar()) {
          Slice rows_data = CHECK_RESULT(
              retrier().controller().GetSidecar(ql_response.rows_data_sidecar()));
          ql_op->mutable_rows_data()->assign(rows_data.cdata(), rows_data.size());
        }
        ql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_WRITE: {
        if (pgsql_idx >= resp_.pgsql_response_batch().size()) {
          ++pgsql_idx;
          continue;
        }
        // Restore QL write request PB and extract response.
        auto* pgsql_op = down_cast<YBPgsqlWriteOp*>(yb_op);
        pgsql_op->mutable_response()->Swap(resp_.mutable_pgsql_response_batch(pgsql_idx));
        const auto& pgsql_response = pgsql_op->response();
        if (pgsql_response.has_rows_data_sidecar()) {
          Slice rows_data = CHECK_RESULT(retrier().controller().GetSidecar(
              pgsql_response.rows_data_sidecar()));
          down_cast<YBPgsqlWriteOp*>(yb_op)->mutable_rows_data()->assign(
              util::to_char_ptr(rows_data.data()), rows_data.size());
        }
        pgsql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_READ:
        LOG(FATAL) << "Not a write operation " << op->yb_op->type();
        break;
    }
  }

  if (redis_idx != resp_.redis_response_batch().size() ||
      ql_idx != resp_.ql_response_batch().size() ||
      pgsql_idx != resp_.pgsql_response_batch().size()) {
    LOG(ERROR) << Substitute("Write response count mismatch: "
                             "$0 Redis requests sent, $1 responses received. "
                             "$2 Apache CQL requests sent, $3 responses received. "
                             "$4 PostgreSQL requests sent, $5 responses received.",
                             redis_idx, resp_.redis_response_batch().size(),
                             ql_idx, resp_.ql_response_batch().size(),
                             pgsql_idx, resp_.pgsql_response_batch().size());
    auto status = STATUS(IllegalState, "Write response count mismatch");
    LOG(ERROR) << status << ", request: " << req_.ShortDebugString()
               << ", response: " << resp_.ShortDebugString();
    batcher_->AddOpCountMismatchError();
    Failed(status);
  }
}

void WriteRpc::ProcessResponseFromTserver(const Status& status) {
  TRACE_TO(trace_, "ProcessResponseFromTserver($0)", status.ToString(false));
  if (resp_.has_trace_buffer()) {
    TRACE_TO(trace_, "Received from server: $0", resp_.trace_buffer());
  }
  batcher_->ProcessWriteResponse(*this, status);
  if (!CommonResponseCheck(status)) {
    SwapRequestsAndResponses(true);
    return;
  }

  SwapRequestsAndResponses(false);
}

bool WriteRpc::ShouldRetryExpiredRequest() {
  return req_.min_running_request_id() == kInitializeFromMinRunning;
}

ReadRpc::ReadRpc(AsyncRpcData* data, YBConsistencyLevel yb_consistency_level)
    : AsyncRpcBase(data, yb_consistency_level) {

  TRACE_TO(trace_, "ReadRpc initiated to $0", data->tablet->tablet_id());
  req_.set_consistency_level(yb_consistency_level);
  req_.set_proxy_uuid(data->batcher->proxy_uuid());

  int ctr = 0;
  for (auto& op : ops_) {
    switch (op->yb_op->type()) {
      case YBOperation::Type::REDIS_READ: {
        CHECK_EQ(table()->table_type(), YBTableType::REDIS_TABLE_TYPE);
        // Move Redis read request PB into tserver read request PB for performance. Will restore
        // in ProcessResponseFromTserver.
        auto* redis_op = down_cast<YBRedisReadOp*>(op->yb_op.get());
        req_.add_redis_batch()->Swap(redis_op->mutable_request());
        break;
      }
      case YBOperation::Type::QL_READ: {
        CHECK_EQ(table()->table_type(), YBTableType::YQL_TABLE_TYPE);
        // Move QL read request PB into tserver read request PB for performance. Will restore
        // in ProcessResponseFromTserver.
        auto* ql_op = down_cast<YBqlReadOp*>(op->yb_op.get());
        req_.add_ql_batch()->Swap(ql_op->mutable_request());
        if (ql_op->read_time()) {
          ql_op->read_time().AddToPB(&req_);
        }
        break;
      }
      case YBOperation::Type::PGSQL_READ: {
        CHECK_EQ(table()->table_type(), YBTableType::PGSQL_TABLE_TYPE);
        auto* pgsql_op = down_cast<YBPgsqlReadOp*>(op->yb_op.get());
        req_.add_pgsql_batch()->Swap(pgsql_op->mutable_request());
        if (pgsql_op->read_time()) {
          pgsql_op->read_time().AddToPB(&req_);
        }
        break;
      }
      case YBOperation::Type::PGSQL_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_WRITE:
        LOG(FATAL) << "Not a read operation " << op->yb_op->type();
        break;
      default:
        LOG(FATAL) << "Unsupported read operation " << op->yb_op->type();
        break;
    }
    op->state = InFlightOpState::kRequestSent;
    VLOG(4) << ++ctr << ". Encoded row " << op->yb_op->ToString();
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Created batch for " << data->tablet->tablet_id() << ":\n"
            << req_.ShortDebugString();
  }
}

ReadRpc::~ReadRpc() {
  MonoTime end_time = MonoTime::Now();

  // Get locality metrics if enabled, but skip for system tables as those go to the master.
  if (async_rpc_metrics_ && !table()->name().is_system()) {
    scoped_refptr<Histogram> read_rpc_time = IsLocalCall() ?
                                             async_rpc_metrics_->local_read_rpc_time :
                                             async_rpc_metrics_->remote_read_rpc_time;

    read_rpc_time->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
  }
}

void ReadRpc::CallRemoteMethod() {
  auto trace = trace_; // It is possible that we receive reply before returning from ReadAsync.
                       // Detailed explanation in WriteRpc::SendRpcToTserver.
  TRACE_TO(trace, "SendRpcToTserver");
  ADOPT_TRACE(trace.get());

  tablet_invoker_.proxy()->ReadAsync(
      req_, &resp_, PrepareController(),
      std::bind(&ReadRpc::Finished, this, Status::OK()));
  TRACE_TO(trace, "RpcDispatched Asynchronously");
}

void ReadRpc::SwapRequestsAndResponses(bool skip_responses) {
  size_t redis_idx = 0;
  size_t ql_idx = 0;
  size_t pgsql_idx = 0;
  for (auto& op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_READ: {
        auto* redis_op = down_cast<YBRedisReadOp*>(yb_op);
        redis_op->mutable_request()->Swap(req_.mutable_redis_batch(redis_idx));
        redis_idx++;
        break;
      }
      case YBOperation::Type::QL_READ: {
        // Restore QL read request PB and extract response.
        auto* ql_op = down_cast<YBqlReadOp*>(yb_op);
        ql_op->mutable_request()->Swap(req_.mutable_ql_batch(ql_idx));
        ql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_READ: {
        // Restore PGSQL read request PB and extract response.
        auto* pgsql_op = down_cast<YBPgsqlReadOp*>(yb_op);
        pgsql_op->mutable_request()->Swap(req_.mutable_pgsql_batch(pgsql_idx));
        pgsql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_WRITE:
        LOG(FATAL) << "Not a read operation " << op->yb_op->type();
        break;
    }
  }

  if (skip_responses) return;

  // Retrieve Redis and QL responses and make sure we received all the responses back.
  redis_idx = 0;
  ql_idx = 0;
  pgsql_idx = 0;
  for (auto& op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_READ: {
        if (redis_idx >= resp_.redis_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        // Restore Redis read request PB and extract response.
        auto* redis_op = down_cast<YBRedisReadOp*>(yb_op);
        redis_op->mutable_response()->Swap(resp_.mutable_redis_batch(redis_idx));
        redis_idx++;
        break;
      }
      case YBOperation::Type::QL_READ: {
        if (ql_idx >= resp_.ql_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        // Restore QL read request PB and extract response.
        auto* ql_op = down_cast<YBqlReadOp*>(yb_op);
        ql_op->mutable_response()->Swap(resp_.mutable_ql_batch(ql_idx));
        const auto& ql_response = ql_op->response();
        if (ql_response.has_rows_data_sidecar()) {
          Slice rows_data = CHECK_RESULT(retrier().controller().GetSidecar(
              ql_response.rows_data_sidecar()));
          ql_op->mutable_rows_data()->assign(util::to_char_ptr(rows_data.data()), rows_data.size());
        }
        ql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_READ: {
        if (pgsql_idx >= resp_.pgsql_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        // Restore PGSQL read request PB and extract response.
        auto* pgsql_op = down_cast<YBPgsqlReadOp*>(yb_op);
        pgsql_op->mutable_response()->Swap(resp_.mutable_pgsql_batch(pgsql_idx));
        const auto& pgsql_response = pgsql_op->response();
        if (pgsql_response.has_rows_data_sidecar()) {
          Slice rows_data = CHECK_RESULT(retrier().controller().GetSidecar(
              pgsql_response.rows_data_sidecar()));
          down_cast<YBPgsqlReadOp*>(yb_op)->mutable_rows_data()->assign(
              util::to_char_ptr(rows_data.data()), rows_data.size());
        }
        pgsql_idx++;
        break;
      }
      case YBOperation::Type::PGSQL_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::QL_WRITE:
        LOG(FATAL) << "Not a read operation " << op->yb_op->type();
        break;
    }
  }

  if (redis_idx != resp_.redis_batch().size() ||
      ql_idx != resp_.ql_batch().size() ||
      pgsql_idx != resp_.pgsql_batch().size()) {
    LOG(ERROR) << Substitute("Read response count mismatch: "
                             "$0 Redis requests sent, $1 responses received. "
                             "$2 QL requests sent, $3 responses received. "
                             "$4 QL requests sent, $5 responses received.",
                             redis_idx, resp_.redis_batch().size(),
                             ql_idx, resp_.ql_batch().size(),
                             pgsql_idx, resp_.pgsql_batch().size());
    batcher_->AddOpCountMismatchError();
    Failed(STATUS(IllegalState, "Read response count mismatch"));
  }

}

void ReadRpc::ProcessResponseFromTserver(const Status& status) {
  TRACE_TO(trace_, "ProcessResponseFromTserver($0)", status.ToString(false));
  if (resp_.has_trace_buffer()) {
    TRACE_TO(trace_, "Received from server: $0", resp_.trace_buffer());
  }
  batcher_->ProcessReadResponse(*this, status);
  if (!CommonResponseCheck(status)) {
    SwapRequestsAndResponses(true);
    return;
  }
  SwapRequestsAndResponses(false);
}

}  // namespace internal
}  // namespace client
}  // namespace yb
