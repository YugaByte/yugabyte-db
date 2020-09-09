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
#include "yb/yql/cql/cqlserver/cql_rpc.h"

#include "yb/yql/cql/cqlserver/cql_message.h"
#include "yb/yql/cql/cqlserver/cql_service.h"
#include "yb/yql/cql/cqlserver/cql_statement.h"

#include "yb/rpc/connection.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/reactor.h"
#include "yb/rpc/rpc_introspection.pb.h"

#include "yb/util/debug/trace_event.h"
#include "yb/util/size_literals.h"

using yb::cqlserver::CQLMessage;
using namespace std::literals; // NOLINT
using namespace std::placeholders;
using yb::operator"" _KB;
using yb::operator"" _MB;

DECLARE_bool(rpc_dump_all_traces);
DECLARE_int32(rpc_slow_query_threshold_ms);
DEFINE_int32(rpcz_max_cql_query_dump_size, 4_KB,
             "The maximum size of the CQL query string in the RPCZ dump.");
DEFINE_int32(rpcz_max_cql_batch_dump_count, 4_KB,
             "The maximum number of CQL batch elements in the RPCZ dump.");
DEFINE_bool(throttle_cql_calls_on_soft_memory_limit, true,
            "Whether to reject CQL calls when soft memory limit is reached.");

constexpr int kDropPolicy = 1;
constexpr int kRejectPolicy = 0;

DEFINE_int32(throttle_cql_calls_policy, kRejectPolicy,
              "Policy for throttling CQL calls. 1 - drop throttled calls. "
              "0 - respond with OVERLOADED error.");

DECLARE_int32(rpc_max_message_size);

// Max msg length for CQL.
// Since yb_rpc limit is 255MB, we limit consensensus size to 254MB,
// and hence max cql message length to 253MB
// This length corresponds to 3 strings with size of 64MB along with any additional fields
// and overheads
DEFINE_int32(max_message_length, 254_MB,
             "The maximum message length of the cql message.");

// By default the CQL server sends CQL EVENTs (opcode=0x0c) only if the connection was
// subscribed (via REGISTER request) for particular events. The flag allows to send all
// available event always - even if the connection was not subscribed for events.
DEFINE_bool(cql_server_always_send_events, false,
            "All CQL connections automatically subscribed for all CQL events.");

namespace yb {
namespace cqlserver {

CQLConnectionContext::CQLConnectionContext(
    size_t receive_buffer_size, const MemTrackerPtr& buffer_tracker,
    const MemTrackerPtr& call_tracker)
    : ql_session_(new ql::QLSession()),
      parser_(buffer_tracker, CQLMessage::kMessageHeaderLength, CQLMessage::kHeaderPosLength,
              FLAGS_max_message_length, rpc::IncludeHeader::kTrue, rpc::SkipEmptyMessages::kFalse,
              this),
      read_buffer_(receive_buffer_size, buffer_tracker),
      call_tracker_(call_tracker) {
  VLOG(1) << "CQL Connection Context: FLAGS_cql_server_always_send_events = " <<
      FLAGS_cql_server_always_send_events;

  if (FLAGS_cql_server_always_send_events) {
    registered_events_ = CQLMessage::kAllEvents;
  }
}

Result<rpc::ProcessDataResult> CQLConnectionContext::ProcessCalls(
    const rpc::ConnectionPtr& connection, const IoVecs& data,
    rpc::ReadBufferFull read_buffer_full) {
  return parser_.Parse(connection, data, read_buffer_full, nullptr /* tracker_for_throttle */);
}

Status CQLConnectionContext::HandleCall(
    const rpc::ConnectionPtr& connection, rpc::CallData* call_data) {
  auto reactor = connection->reactor();
  DCHECK(reactor->IsCurrentThread());

  auto call = rpc::InboundCall::Create<CQLInboundCall>(
      connection, call_processed_listener(), ql_session_);

  Status s = call->ParseFrom(call_tracker_, call_data);
  if (!s.ok()) {
    LOG(WARNING) << connection->ToString() << ": received bad data: " << s.ToString();
    return STATUS_SUBSTITUTE(NetworkError, "Bad data: $0", s.ToUserMessage());
  }

  if (FLAGS_throttle_cql_calls_on_soft_memory_limit) {
    if (!CheckMemoryPressureWithLogging(call_tracker_, /* score= */ 0.0, "Rejecting CQL call: ")) {
      if (FLAGS_throttle_cql_calls_policy != kDropPolicy) {
        static Status status = STATUS(ServiceUnavailable, "Server is under memory pressure");
        // We did not store call yet, so should not notify that it was processed.
        call->ResetCallProcessedListener();
        call->RespondFailure(rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY, Status::OK());
      } // Otherwise silently drop the call without queueing it. Clients will get a timeout.
      return Status::OK();
    }
  }

  s = Store(call.get());
  if (!s.ok()) {
    return s;
  }

  reactor->messenger()->QueueInboundCall(call);

  return Status::OK();
}

uint64_t CQLConnectionContext::ExtractCallId(rpc::InboundCall* call) {
  return down_cast<CQLInboundCall*>(call)->stream_id();
}

void CQLConnectionContext::DumpPB(const rpc::DumpRunningRpcsRequestPB& req,
                                  rpc::RpcConnectionPB* resp) {
  const string keyspace = ql_session_->current_keyspace();
  if (!keyspace.empty()) {
    resp->mutable_connection_details()->mutable_cql_connection_details()->set_keyspace(keyspace);
  }
  ConnectionContextWithCallId::DumpPB(req, resp);
}

CQLInboundCall::CQLInboundCall(rpc::ConnectionPtr conn,
                               CallProcessedListener call_processed_listener,
                               ql::QLSession::SharedPtr ql_session)
    : InboundCall(std::move(conn), nullptr /* rpc_metrics */, std::move(call_processed_listener)),
      ql_session_(std::move(ql_session)) {
}

Status CQLInboundCall::ParseFrom(const MemTrackerPtr& call_tracker, rpc::CallData* call_data) {
  TRACE_EVENT_FLOW_BEGIN0("rpc", "CQLInboundCall", this);
  TRACE_EVENT0("rpc", "CQLInboundCall::ParseFrom");

  consumption_ = ScopedTrackedConsumption(call_tracker, call_data->size());

  // Parsing of CQL message is deferred to CQLServiceImpl::Handle. Just save the serialized data.
  request_data_ = std::move(*call_data);
  serialized_request_ = Slice(request_data_.data(), request_data_.size());

  // Fill the service name method name to transfer the call to. The method name is for debug
  // tracing only. Inside CQLServiceImpl::Handle, we rely on the opcode to dispatch the execution.
  stream_id_ = cqlserver::CQLRequest::ParseStreamId(serialized_request_);

  return Status::OK();
}

const std::string& CQLInboundCall::service_name() const {
  static std::string result = "yb.cqlserver.CQLServerService"s;
  return result;
}

const std::string& CQLInboundCall::method_name() const {
  static std::string result = "ExecuteRequest"s;
  return result;
}

void CQLInboundCall::Serialize(boost::container::small_vector_base<RefCntBuffer>* output) {
  TRACE_EVENT0("rpc", "CQLInboundCall::Serialize");
  CHECK_GT(response_msg_buf_.size(), 0);

  output->push_back(std::move(response_msg_buf_));
}

void CQLInboundCall::RespondFailure(rpc::ErrorStatusPB::RpcErrorCodePB error_code,
                                    const Status& status) {
  const auto& context = static_cast<const CQLConnectionContext&>(connection()->context());
  const auto compression_scheme = context.compression_scheme();
  faststring msg;
  switch (error_code) {
    case rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY: {
      // Return OVERLOADED error to redirect CQL client to the next host.
      ErrorResponse(stream_id_, ErrorResponse::Code::OVERLOADED, status.message().ToBuffer())
          .Serialize(compression_scheme, &msg);
      break;
    }
    case rpc::ErrorStatusPB::FATAL_SERVER_SHUTTING_DOWN: {
      // Return OVERLOADED error to redirect CQL client to the next host.
      ErrorResponse(stream_id_, ErrorResponse::Code::OVERLOADED, "CQL shutting down")
          .Serialize(compression_scheme, &msg);
      break;
    }
    case rpc::ErrorStatusPB::ERROR_APPLICATION: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::ERROR_NO_SUCH_METHOD: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::ERROR_NO_SUCH_SERVICE: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::ERROR_INVALID_REQUEST: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::FATAL_DESERIALIZING_REQUEST: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::FATAL_VERSION_MISMATCH: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::FATAL_UNAUTHORIZED: FALLTHROUGH_INTENDED;
    case rpc::ErrorStatusPB::FATAL_UNKNOWN: {
      LOG(ERROR) << "Unexpected error status: "
                 << rpc::ErrorStatusPB::RpcErrorCodePB_Name(error_code);
      ErrorResponse(stream_id_, ErrorResponse::Code::SERVER_ERROR, "Server error")
          .Serialize(compression_scheme, &msg);
      break;
    }
  }
  response_msg_buf_ = RefCntBuffer(msg);

  QueueResponse(/* is_success */ false);
}

void CQLInboundCall::RespondSuccess(const RefCntBuffer& buffer,
                                    const yb::rpc::RpcMethodMetrics& metrics) {
  RecordHandlingCompleted(metrics.handler_latency);
  response_msg_buf_ = buffer;

  QueueResponse(/* is_success */ true);
}

void CQLInboundCall::GetCallDetails(rpc::RpcCallInProgressPB *call_in_progress_pb) const {
  std::shared_ptr<const CQLRequest> request =
#ifdef THREAD_SANITIZER
      request_;
#else
      std::atomic_load_explicit(&request_, std::memory_order_acquire);
#endif
  if (request == nullptr) {
    return;
  }
  rpc::CQLCallDetailsPB* call_in_progress = call_in_progress_pb->mutable_cql_details();
  rpc::CQLStatementsDetailsPB* details_pb;
  std::shared_ptr<const CQLStatement> statement_ptr;
  string query_id;
  int j = 0;
  switch (request->opcode()) {
    case CQLMessage::Opcode::PREPARE:
      call_in_progress->set_type("PREPARE");
      details_pb = call_in_progress->add_call_details();
      details_pb->set_sql_string((static_cast<const PrepareRequest&>(*request)).query()
                                    .substr(0, FLAGS_rpcz_max_cql_query_dump_size));
      return;
    case CQLMessage::Opcode::EXECUTE:
      call_in_progress->set_type("EXECUTE");
      details_pb = call_in_progress->add_call_details();
      query_id = (static_cast<const ExecuteRequest&>(*request)).query_id();
      details_pb->set_sql_id(b2a_hex(query_id));
      statement_ptr = service_impl_->GetPreparedStatement(query_id);
      if (statement_ptr != nullptr) {
        details_pb->set_sql_string(statement_ptr->text()
                                       .substr(0, FLAGS_rpcz_max_cql_query_dump_size));
      }
      return;
    case CQLMessage::Opcode::QUERY:
      call_in_progress->set_type("QUERY");
      details_pb = call_in_progress->add_call_details();
      details_pb->set_sql_string((static_cast<const QueryRequest&>(*request)).query()
                                    .substr(0, FLAGS_rpcz_max_cql_query_dump_size));
      return;
    case CQLMessage::Opcode::BATCH:
      call_in_progress->set_type("BATCH");
      for (const BatchRequest::Query& batchQuery :
          (static_cast<const BatchRequest&>(*request)).queries()) {
        details_pb = call_in_progress->add_call_details();
        if (batchQuery.is_prepared) {
          details_pb->set_sql_id(b2a_hex(batchQuery.query_id));
          statement_ptr = service_impl_->GetPreparedStatement(batchQuery.query_id);
          if (statement_ptr != nullptr) {
            if (statement_ptr->text().size() > FLAGS_rpcz_max_cql_query_dump_size) {
              string short_text = statement_ptr->text()
                  .substr(0, FLAGS_rpcz_max_cql_query_dump_size);
              details_pb->set_sql_string(short_text);
            } else {
              details_pb->set_sql_string(statement_ptr->text());
            }
          }
        } else {
          details_pb->set_sql_string(batchQuery.query
                                         .substr(0, FLAGS_rpcz_max_cql_query_dump_size));
        }
        if (++j >= FLAGS_rpcz_max_cql_batch_dump_count) {
          // Showing only rpcz_max_cql_batch_dump_count queries
          break;
        }
      }
      return;
    default:
      return;
  }
}


void CQLInboundCall::LogTrace() const {
  MonoTime now = MonoTime::Now();
  int total_time = now.GetDeltaSince(timing_.time_received).ToMilliseconds();
  if (PREDICT_FALSE(FLAGS_rpc_dump_all_traces || total_time > FLAGS_rpc_slow_query_threshold_ms)) {
      LOG(WARNING) << ToString() << " took " << total_time << "ms. Details:";
      rpc::RpcCallInProgressPB call_in_progress_pb;
      GetCallDetails(&call_in_progress_pb);
      LOG(WARNING) << call_in_progress_pb.DebugString() << "Trace: ";
      trace_->Dump(&LOG(WARNING), /* include_time_deltas */ true);
  }
}

std::string CQLInboundCall::ToString() const {
  return Format("CQL Call from $0, stream id: $1", connection()->remote(), stream_id_);
}

bool CQLInboundCall::DumpPB(const rpc::DumpRunningRpcsRequestPB& req,
                            rpc::RpcCallInProgressPB* resp) {

  if (req.include_traces() && trace_) {
    resp->set_trace_buffer(trace_->DumpToString(true));
  }
  resp->set_elapsed_millis(
      MonoTime::Now().GetDeltaSince(timing_.time_received).ToMilliseconds());
  GetCallDetails(resp);

  return true;
}

CoarseTimePoint CQLInboundCall::GetClientDeadline() const {
  // TODO(Robert) - fill in CQL timeout
  return CoarseTimePoint::max();
}

} // namespace cqlserver
} // namespace yb
