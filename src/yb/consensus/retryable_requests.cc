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

#include "yb/consensus/retryable_requests.h"

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"

#include "yb/util/atomic.h"
#include "yb/util/metrics.h"
#include "yb/util/opid.h"

DEFINE_int32(retryable_request_timeout_secs, 120,
             "Amount of time to keep write request in index, to prevent duplicate writes.");

// We use this limit to prevent request range from infinite grow, because it will block log
// cleanup. I.e. even we have continous request range, it will be split by blocks, that could be
// dropped independently.
DEFINE_int32(retryable_request_range_time_limit_secs, 30,
             "Max delta in time for single op id range.");

METRIC_DEFINE_gauge_int64(tablet, running_retryable_requests,
                          "Number of running retryable requests.",
                          yb::MetricUnit::kRequests,
                          "Number of running retryable requests.");

METRIC_DEFINE_gauge_int64(tablet, replicated_retryable_request_ranges,
                          "Number of replicated retryable request ranges.",
                          yb::MetricUnit::kRequests,
                          "Number of replicated retryable request ranges.");

namespace yb {
namespace consensus {

namespace {

struct RunningRetryableRequest {
  RetryableRequestId request_id;
  yb::OpId op_id;
  RestartSafeCoarseTimePoint time;
  mutable std::vector<ConsensusRoundPtr> duplicate_rounds;

  RunningRetryableRequest(
      RetryableRequestId request_id_, const OpIdPB& op_id_, RestartSafeCoarseTimePoint time_)
      : request_id(request_id_), op_id(yb::OpId::FromPB(op_id_)), time(time_) {}

  std::string ToString() const {
    return Format("{ request_id: $0 op_id $1 time: $2 }", request_id, op_id, time);
  }
};

struct ReplicatedRetryableRequestRange {
  mutable RetryableRequestId first_id;
  RetryableRequestId last_id;
  yb::OpId min_op_id;
  mutable RestartSafeCoarseTimePoint min_time;
  mutable RestartSafeCoarseTimePoint max_time;

  ReplicatedRetryableRequestRange(RetryableRequestId id, const yb::OpId& op_id,
                              RestartSafeCoarseTimePoint time)
      : first_id(id), last_id(id), min_op_id(op_id), min_time(time),
        max_time(time) {}

  void InsertTime(const RestartSafeCoarseTimePoint& time) const {
    min_time = std::min(min_time, time);
    max_time = std::max(max_time, time);
  }

  void PrepareJoinWithPrev(const ReplicatedRetryableRequestRange& prev) const {
    min_time = std::min(min_time, prev.min_time);
    max_time = std::max(max_time, prev.max_time);
    first_id = prev.first_id;
  }

  std::string ToString() const {
    return Format("{ first_id: $0 last_id: $1 min_op_id: $2 min_time: $3 max_time: $4 }",
                  first_id, last_id, min_op_id, min_time, max_time);
  }
};

struct LastIdIndex;
struct OpIdIndex;
struct RequestIdIndex;

typedef boost::multi_index_container <
    RunningRetryableRequest,
    boost::multi_index::indexed_by <
        boost::multi_index::hashed_unique <
            boost::multi_index::tag<RequestIdIndex>,
            boost::multi_index::member <
                RunningRetryableRequest, RetryableRequestId, &RunningRetryableRequest::request_id
            >
        >,
        boost::multi_index::ordered_unique <
            boost::multi_index::tag<OpIdIndex>,
            boost::multi_index::member <
                RunningRetryableRequest, yb::OpId, &RunningRetryableRequest::op_id
            >
        >
    >
> RunningRetryableRequests;

typedef boost::multi_index_container <
    ReplicatedRetryableRequestRange,
    boost::multi_index::indexed_by <
        boost::multi_index::ordered_unique <
            boost::multi_index::tag<LastIdIndex>,
            boost::multi_index::member <
                ReplicatedRetryableRequestRange, RetryableRequestId,
                &ReplicatedRetryableRequestRange::last_id
            >
        >,
        boost::multi_index::ordered_unique <
            boost::multi_index::tag<OpIdIndex>,
            boost::multi_index::member <
                ReplicatedRetryableRequestRange, yb::OpId,
                &ReplicatedRetryableRequestRange::min_op_id
            >
        >
    >
> ReplicatedRetryableRequestRanges;

typedef ReplicatedRetryableRequestRanges::index<LastIdIndex>::type
    ReplicatedRetryableRequestRangesByLastId;

struct ClientRetryableRequests {
  RunningRetryableRequests running;
  ReplicatedRetryableRequestRanges replicated;
  RetryableRequestId min_running_request_id = 0;
  RestartSafeCoarseTimePoint empty_since;
};

std::chrono::seconds RangeTimeLimit() {
  return std::chrono::seconds(FLAGS_retryable_request_range_time_limit_secs);
}

class ReplicateData {
 public:
  ReplicateData() : client_id_(ClientId::Nil()), write_request_(nullptr) {}

  explicit ReplicateData(const tserver::WriteRequestPB* write_request, const yb::OpIdPB& op_id)
      : client_id_(write_request->client_id1(), write_request->client_id2()),
        write_request_(write_request), op_id_(yb::OpId::FromPB(op_id)) {

  }

  static ReplicateData FromMsg(const ReplicateMsg& replicate_msg) {
    if (!replicate_msg.has_write_request()) {
      return ReplicateData();
    }

    return ReplicateData(&replicate_msg.write_request(), replicate_msg.id());
  }

  bool operator!() const {
    return client_id_.IsNil();
  }

  explicit operator bool() const {
    return !!*this;
  }

  const ClientId& client_id() const {
    return client_id_;
  }

  const tserver::WriteRequestPB& write_request() const {
    return *write_request_;
  }

  RetryableRequestId request_id() const {
    return write_request_->request_id();
  }

  const yb::OpId& op_id() const {
    return op_id_;
  }

 private:
  ClientId client_id_;
  const tserver::WriteRequestPB* write_request_;
  yb::OpId op_id_;
};

std::ostream& operator<<(std::ostream& out, const ReplicateData& data) {
  return out << data.client_id() << '/' << data.request_id() << ": "
             << data.write_request().ShortDebugString() << " op_id: " << data.op_id();
}

} // namespace

class RetryableRequests::Impl {
 public:
  explicit Impl(std::string log_prefix) : log_prefix_(std::move(log_prefix)) {
    VLOG_WITH_PREFIX(1) << "Start";
  }

  bool Register(const ConsensusRoundPtr& round, RestartSafeCoarseTimePoint entry_time) {
    auto data = ReplicateData::FromMsg(*round->replicate_msg());
    if (!data) {
      return true;
    }

    if (entry_time == RestartSafeCoarseTimePoint()) {
      entry_time = clock_.Now();
    }

    ClientRetryableRequests& client_retryable_requests = clients_[data.client_id()];

    CleanupReplicatedRequests(
        data.write_request().min_running_request_id(), &client_retryable_requests);

    if (data.request_id() < client_retryable_requests.min_running_request_id) {
      round->NotifyReplicationFinished(
          STATUS_EC_FORMAT(
              Expired,
              MinRunningRequestIdStatusData(client_retryable_requests.min_running_request_id),
              "Request id $0 is less than min running $1", data.request_id(),
              client_retryable_requests.min_running_request_id),
          round->bound_term(), nullptr /* applied_op_ids */);
      return false;
    }

    auto& replicated_indexed_by_last_id = client_retryable_requests.replicated.get<LastIdIndex>();
    auto it = replicated_indexed_by_last_id.lower_bound(data.request_id());
    if (it != replicated_indexed_by_last_id.end() && it->first_id <= data.request_id()) {
      round->NotifyReplicationFinished(
          STATUS(AlreadyPresent, "Duplicate request"), round->bound_term(),
          nullptr /* applied_op_ids */);
      return false;
    }

    auto& running_indexed_by_request_id = client_retryable_requests.running.get<RequestIdIndex>();
    auto emplace_result = running_indexed_by_request_id.emplace(
        data.request_id(), round->replicate_msg()->id(), entry_time);
    if (!emplace_result.second) {
      emplace_result.first->duplicate_rounds.push_back(round);
      return false;
    }

    VLOG_WITH_PREFIX(4) << "Running added " << data;
    if (running_requests_gauge_) {
      running_requests_gauge_->Increment();
    }

    return true;
  }

  yb::OpId CleanExpiredReplicatedAndGetMinOpId() {
    yb::OpId result(std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max());
    auto now = clock_.Now();
    auto clean_start =
        now - std::chrono::seconds(GetAtomicFlag(&FLAGS_retryable_request_timeout_secs));
    for (auto ci = clients_.begin(); ci != clients_.end();) {
      ClientRetryableRequests& client_retryable_requests = ci->second;
      auto& op_id_index = client_retryable_requests.replicated.get<OpIdIndex>();
      auto it = op_id_index.begin();
      int64_t count = 0;
      while (it != op_id_index.end() && it->max_time < clean_start) {
        ++it;
        ++count;
      }
      if (replicated_request_ranges_gauge_) {
        replicated_request_ranges_gauge_->DecrementBy(count);
      }
      if (it != op_id_index.end()) {
        result = std::min(result, it->min_op_id);
        op_id_index.erase(op_id_index.begin(), it);
      } else {
        op_id_index.clear();
      }
      if (op_id_index.empty() && client_retryable_requests.running.empty()) {
        // We delay deleting client with empty requests, to be able to filter requests with too
        // small request id.
        if (client_retryable_requests.empty_since == RestartSafeCoarseTimePoint()) {
          client_retryable_requests.empty_since = now;
        } else if (client_retryable_requests.empty_since < clean_start) {
          ci = clients_.erase(ci);
          continue;
        }
      }
      ++ci;
    }

    return result;
  }

  void ReplicationFinished(
      const ReplicateMsg& replicate_msg, const Status& status, int64_t leader_term) {
    auto data = ReplicateData::FromMsg(replicate_msg);
    if (!data) {
      return;
    }

    auto& client_retryable_requests = clients_[data.client_id()];
    auto& running_indexed_by_request_id = client_retryable_requests.running.get<RequestIdIndex>();
    auto running_it = running_indexed_by_request_id.find(data.request_id());
    if (running_it == running_indexed_by_request_id.end()) {
#ifndef NDEBUG
      LOG_WITH_PREFIX(ERROR) << "Running requests: "
                             << yb::ToString(running_indexed_by_request_id);
#endif
      LOG_WITH_PREFIX(DFATAL) << "Replication finished for request with unknown id " << data;
      return;
    }
    VLOG_WITH_PREFIX(4) << "Running " << (status.ok() ? "replicated " : "aborted ") << data
                        << ", " << status;

    static Status duplicate_write_status = STATUS(AlreadyPresent, "Duplicate request");
    auto status_for_duplicate = status.ok() ? duplicate_write_status : status;
    for (const auto& duplicate : running_it->duplicate_rounds) {
      duplicate->NotifyReplicationFinished(status_for_duplicate, leader_term,
                                           nullptr /* applied_op_ids */);
    }
    auto entry_time = running_it->time;
    running_indexed_by_request_id.erase(running_it);
    if (running_requests_gauge_) {
      running_requests_gauge_->Decrement();
    }

    if (status.ok()) {
      AddReplicated(
          yb::OpId::FromPB(replicate_msg.id()), data, entry_time, &client_retryable_requests);
    }
  }

  void Bootstrap(
      const ReplicateMsg& replicate_msg, RestartSafeCoarseTimePoint entry_time) {
    auto data = ReplicateData::FromMsg(replicate_msg);
    if (!data) {
      return;
    }

    auto& client_retryable_requests = clients_[data.client_id()];
    auto& running_indexed_by_request_id = client_retryable_requests.running.get<RequestIdIndex>();
    if (running_indexed_by_request_id.count(data.request_id()) != 0) {
#ifndef NDEBUG
      LOG_WITH_PREFIX(ERROR) << "Running requests: "
                             << yb::ToString(running_indexed_by_request_id);
#endif
      LOG_WITH_PREFIX(DFATAL) << "Bootstrapped running request " << data;
      return;
    }
    VLOG_WITH_PREFIX(4) << "Bootstrapped " << data;

    CleanupReplicatedRequests(
       data.write_request().min_running_request_id(), &client_retryable_requests);

    AddReplicated(
        yb::OpId::FromPB(replicate_msg.id()), data, entry_time, &client_retryable_requests);
  }

  RestartSafeCoarseMonoClock& Clock() {
    return clock_;
  }

  void SetMetricEntity(const scoped_refptr<MetricEntity>& metric_entity) {
    running_requests_gauge_ = METRIC_running_retryable_requests.Instantiate(metric_entity, 0);
    replicated_request_ranges_gauge_ = METRIC_replicated_retryable_request_ranges.Instantiate(
        metric_entity, 0);
  }

  RetryableRequestsCounts TEST_Counts() {
    RetryableRequestsCounts result;
    for (const auto& p : clients_) {
      result.running += p.second.running.size();
      result.replicated += p.second.replicated.size();
      LOG_WITH_PREFIX(INFO) << "Replicated: " << yb::ToString(p.second.replicated);
    }
    return result;
  }

  Result<RetryableRequestId> MinRunningRequestId(const ClientId& client_id) const {
    const auto it = clients_.find(client_id);
    if (it == clients_.end()) {
      return STATUS_FORMAT(NotFound, "Client requests data not found for client $0", client_id);
    }
    return it->second.min_running_request_id;
  }

 private:
  void CleanupReplicatedRequests(
      RetryableRequestId new_min_running_request_id,
      ClientRetryableRequests* client_retryable_requests) {
    auto& replicated_indexed_by_last_id = client_retryable_requests->replicated.get<LastIdIndex>();
    if (new_min_running_request_id > client_retryable_requests->min_running_request_id) {
      // We are not interested in ids below write_request.min_running_request_id() anymore.
      //
      // Request id intervals are ordered by last id of interval, and does not overlap.
      // So we are trying to find interval with last_id >= min_running_request_id
      // and trim it if necessary.
      auto it = replicated_indexed_by_last_id.lower_bound(new_min_running_request_id);
      if (it != replicated_indexed_by_last_id.end() &&
          it->first_id < new_min_running_request_id) {
        it->first_id = new_min_running_request_id;
      }
      if (replicated_request_ranges_gauge_) {
        replicated_request_ranges_gauge_->DecrementBy(
            std::distance(replicated_indexed_by_last_id.begin(), it));
      }
      // Remove all intervals that has ids below write_request.min_running_request_id().
      replicated_indexed_by_last_id.erase(replicated_indexed_by_last_id.begin(), it);
      client_retryable_requests->min_running_request_id = new_min_running_request_id;
    }
  }

  void AddReplicated(yb::OpId op_id, const ReplicateData& data, RestartSafeCoarseTimePoint time,
                     ClientRetryableRequests* client) {
    auto request_id = data.request_id();
    auto& replicated_indexed_by_last_id = client->replicated.get<LastIdIndex>();
    auto request_it = replicated_indexed_by_last_id.lower_bound(request_id);
    if (request_it != replicated_indexed_by_last_id.end() && request_it->first_id <= request_id) {
#ifndef NDEBUG
      LOG_WITH_PREFIX(ERROR)
          << "Replicated requests: " << yb::ToString(client->replicated);
#endif

      LOG_WITH_PREFIX(DFATAL) << "Request already replicated: " << data;
      return;
    }

    // Check that we have range right after this id, and we could extend it.
    // Requests rarely attaches to begin of interval, so we could don't check for
    // RangeTimeLimit() here.
    if (request_it != replicated_indexed_by_last_id.end() &&
        request_it->first_id == request_id + 1) {
      op_id = std::min(request_it->min_op_id, op_id);
      request_it->InsertTime(time);
      // If previous range is right before this id, then we could just join those ranges.
      if (!TryJoinRanges(request_it, op_id, &replicated_indexed_by_last_id)) {
        --(request_it->first_id);
        UpdateMinOpId(request_it, op_id, &replicated_indexed_by_last_id);
      }
      return;
    }

    if (TryJoinToEndOfRange(request_it, op_id, request_id, time, &replicated_indexed_by_last_id)) {
      return;
    }

    client->replicated.emplace(request_id, op_id, time);
    if (replicated_request_ranges_gauge_) {
      replicated_request_ranges_gauge_->Increment();
    }
  }

  void UpdateMinOpId(
      ReplicatedRetryableRequestRangesByLastId::iterator request_it,
      yb::OpId min_op_id,
      ReplicatedRetryableRequestRangesByLastId* replicated_indexed_by_last_id) {
    if (min_op_id < request_it->min_op_id) {
      replicated_indexed_by_last_id->modify(request_it, [min_op_id](auto& entry) { // NOLINT
        entry.min_op_id = min_op_id;
      });
    }
  }

  bool TryJoinRanges(
      ReplicatedRetryableRequestRangesByLastId::iterator request_it,
      yb::OpId min_op_id,
      ReplicatedRetryableRequestRangesByLastId* replicated_indexed_by_last_id) {
    if (request_it == replicated_indexed_by_last_id->begin()) {
      return false;
    }

    auto request_prev_it = request_it;
    --request_prev_it;

    // We could join ranges if there is exactly one id between them, and request with that id was
    // just replicated...
    if (request_prev_it->last_id + 2 != request_it->first_id) {
      return false;
    }

    // ...and time range will fit into limit.
    if (request_it->max_time > request_prev_it->min_time + RangeTimeLimit()) {
      return false;
    }

    min_op_id = std::min(min_op_id, request_prev_it->min_op_id);
    request_it->PrepareJoinWithPrev(*request_prev_it);
    replicated_indexed_by_last_id->erase(request_prev_it);
    if (replicated_request_ranges_gauge_) {
      replicated_request_ranges_gauge_->Decrement();
    }
    UpdateMinOpId(request_it, min_op_id, replicated_indexed_by_last_id);

    return true;
  }

  bool TryJoinToEndOfRange(
      ReplicatedRetryableRequestRangesByLastId::iterator request_it,
      yb::OpId op_id, RetryableRequestId request_id, RestartSafeCoarseTimePoint time,
      ReplicatedRetryableRequestRangesByLastId* replicated_indexed_by_last_id) {
    if (request_it == replicated_indexed_by_last_id->begin()) {
      return false;
    }

    --request_it;

    if (request_it->last_id + 1 != request_id) {
      return false;
    }

    // It is rare case when request is attaches to end of range, but his time is lower than
    // min_time. So we could avoid checking for the case when
    // time + RangeTimeLimit() > request_prev_it->max_time
    if (time > request_it->min_time + RangeTimeLimit()) {
      return false;
    }

    op_id = std::min(request_it->min_op_id, op_id);
    request_it->InsertTime(time);
    // Actually we should use the modify function on client.replicated, but since the order of
    // ranges should not be changed, we could update last_id directly.
    ++const_cast<ReplicatedRetryableRequestRange&>(*request_it).last_id;

    UpdateMinOpId(request_it, op_id, replicated_indexed_by_last_id);

    return true;
  }

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

  const std::string log_prefix_;
  std::unordered_map<ClientId, ClientRetryableRequests, ClientIdHash> clients_;
  RestartSafeCoarseMonoClock clock_;
  scoped_refptr<AtomicGauge<int64_t>> running_requests_gauge_;
  scoped_refptr<AtomicGauge<int64_t>> replicated_request_ranges_gauge_;
};

RetryableRequests::RetryableRequests(std::string log_prefix)
    : impl_(new Impl(std::move(log_prefix))) {
}

RetryableRequests::~RetryableRequests() {
}

RetryableRequests::RetryableRequests(RetryableRequests&& rhs) : impl_(std::move(rhs.impl_)) {}

void RetryableRequests::operator=(RetryableRequests&& rhs) {
  impl_ = std::move(rhs.impl_);
}

bool RetryableRequests::Register(
    const ConsensusRoundPtr& round, RestartSafeCoarseTimePoint entry_time) {
  return impl_->Register(round, entry_time);
}

yb::OpId RetryableRequests::CleanExpiredReplicatedAndGetMinOpId() {
  return impl_->CleanExpiredReplicatedAndGetMinOpId();
}

void RetryableRequests::ReplicationFinished(
    const ReplicateMsg& replicate_msg, const Status& status, int64_t leader_term) {
  impl_->ReplicationFinished(replicate_msg, status, leader_term);
}

void RetryableRequests::Bootstrap(
    const ReplicateMsg& replicate_msg, RestartSafeCoarseTimePoint entry_time) {
  impl_->Bootstrap(replicate_msg, entry_time);
}

RestartSafeCoarseMonoClock& RetryableRequests::Clock() {
  return impl_->Clock();
}

RetryableRequestsCounts RetryableRequests::TEST_Counts() {
  return impl_->TEST_Counts();
}

Result<RetryableRequestId> RetryableRequests::MinRunningRequestId(
    const ClientId& client_id) const {
  return impl_->MinRunningRequestId(client_id);
}

void RetryableRequests::SetMetricEntity(const scoped_refptr<MetricEntity>& metric_entity) {
  impl_->SetMetricEntity(metric_entity);
}

} // namespace consensus
} // namespace yb
