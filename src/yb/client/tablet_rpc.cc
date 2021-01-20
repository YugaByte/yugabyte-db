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

#include "yb/client/tablet_rpc.h"

#include "yb/common/wire_protocol.h"

#include "yb/client/client.h"
#include "yb/client/client_error.h"
#include "yb/client/meta_cache.h"

#include "yb/tserver/tserver_service.proxy.h"
#include "yb/tserver/tserver_error.h"
#include "yb/util/flag_tags.h"
#include "yb/util/scope_exit.h"

DEFINE_test_flag(bool, assert_local_op, false,
                 "When set, we crash if we received an operation that cannot be served locally.");
DEFINE_int32(force_lookup_cache_refresh_secs, 0, "When non-zero, specifies how often we send a "
             "GetTabletLocations request to the master leader to update the tablet replicas cache. "
             "This request is only sent if we are processing a ConsistentPrefix read.");

DEFINE_int32(lookup_cache_refresh_secs, 60, "When non-zero, specifies how often we send a "
             "GetTabletLocations request to the master leader to update the tablet replicas cache. "
             "This request is only sent if we are processing a ConsistentPrefix read and the RPC "
             "layer has determined that its view of the replicas is inconsistent with what the "
             "master has reported");
DEFINE_test_flag(int32, assert_failed_replicas_less_than, 0,
                 "If greater than 0, this process will crash if the number of failed replicas for "
                 "a RemoteTabletServer is greater than the specified number.");

using namespace std::placeholders;

namespace yb {
namespace client {
namespace internal {

TabletInvoker::TabletInvoker(const bool local_tserver_only,
                             const bool consistent_prefix,
                             YBClient* client,
                             rpc::RpcCommand* command,
                             TabletRpc* rpc,
                             RemoteTablet* tablet,
                             const std::shared_ptr<const YBTable>& table,
                             rpc::RpcRetrier* retrier,
                             Trace* trace)
      : client_(client),
        command_(command),
        rpc_(rpc),
        tablet_(tablet),
        tablet_id_(tablet != nullptr ? tablet->tablet_id() : std::string()),
        table_(table),
        retrier_(retrier),
        trace_(trace),
        local_tserver_only_(local_tserver_only),
        consistent_prefix_(consistent_prefix) {}

TabletInvoker::~TabletInvoker() {}

void TabletInvoker::SelectTabletServerWithConsistentPrefix() {
  TRACE_TO(trace_, "SelectTabletServerWithConsistentPrefix()");
  if (FLAGS_TEST_assert_failed_replicas_less_than) {
    if (tablet_->GetNumFailedReplicas() >= FLAGS_TEST_assert_failed_replicas_less_than) {
      LOG(FATAL) << "Invalid number of failed replicas: " << tablet_->ReplicasAsString();
    }
  }

  std::vector<RemoteTabletServer*> candidates;
  current_ts_ = client_->data_->SelectTServer(tablet_.get(),
                                              YBClient::ReplicaSelection::CLOSEST_REPLICA, {},
                                              &candidates);
  VLOG(1) << "Using tserver: " << yb::ToString(current_ts_);
}

void TabletInvoker::SelectLocalTabletServer() {
  TRACE_TO(trace_, "SelectLocalTabletServer()");

  current_ts_ = client_->data_->meta_cache_->local_tserver();
  VLOG(1) << "Using local tserver: " << current_ts_->ToString();
}

void TabletInvoker::SelectTabletServer()  {
  TRACE_TO(trace_, "SelectTabletServer()");

  assign_new_leader_ = false;
  // Choose a destination TS according to the following algorithm:
  // 1. Select the leader, provided:
  //    a. One exists, and
  //    b. It hasn't failed, and
  //    c. It isn't currently marked as a follower.
  // 2. If there's no good leader select another replica, provided:
  //    a. It hasn't failed, and
  //    b. It hasn't rejected our write due to being a follower.
  // 3. If we're out of appropriate replicas, force a lookup to the master
  //    to fetch new consensus configuration information.
  // 4. When the lookup finishes, forget which replicas were followers and
  //    retry the write (i.e. goto 1).
  // 5. If we issue the write and it fails because the destination was a
  //    follower, remember that fact and retry the write (i.e. goto 1).
  // 6. Repeat steps 1-5 until the write succeeds, fails for other reasons,
  //    or the write's deadline expires.
  current_ts_ = tablet_->LeaderTServer();
  if (current_ts_ && ContainsKey(followers_, current_ts_)) {
    VLOG(2) << "Tablet " << tablet_id_ << ": We have a follower for a leader: "
            << current_ts_->ToString();

    // Mark the node as a follower in the cache so that on the next go-round,
    // LeaderTServer() will not return it as a leader unless a full metadata
    // refresh has occurred. This also avoids LookupTabletByKey() going into
    // "fast path" mode and not actually performing a metadata refresh from the
    // Master when it needs to.
    tablet_->MarkTServerAsFollower(current_ts_);
    current_ts_ = nullptr;
  }
  if (!current_ts_) {
    // Try to "guess" the next leader.
    vector<RemoteTabletServer*> replicas;
    tablet_->GetRemoteTabletServers(&replicas);
    for (RemoteTabletServer* ts : replicas) {
      if (!ContainsKey(followers_, ts)) {
        current_ts_ = ts;
        break;
      }
    }
    if (current_ts_) {
      assign_new_leader_ = true;
    } else {
      YB_LOG_EVERY_N_SECS(INFO, 1)
          << "Unable to pick leader for " << tablet_id_ << ", replicas: " << AsString(replicas)
          << ", followers: " << AsString(followers_) << THROTTLE_MSG;
    }
  } else {
    VLOG(4) << "Selected TServer " << current_ts_->ToString() << " as leader for " << tablet_id_;
  }
}

void TabletInvoker::Execute(const std::string& tablet_id, bool leader_only) {
  if (tablet_id_.empty()) {
    if (!tablet_id.empty()) {
      tablet_id_ = tablet_id;
    } else {
      tablet_id_ = CHECK_NOTNULL(tablet_.get())->tablet_id();
    }
  }

  if (!tablet_) {
    client_->LookupTabletById(tablet_id_, table_, retrier_->deadline(),
                              std::bind(&TabletInvoker::InitialLookupTabletDone, this, _1),
                              UseCache::kTrue);
    return;
  }

  if (consistent_prefix_ && !leader_only) {
    bool refresh_cache = false;
    if (PREDICT_FALSE(FLAGS_force_lookup_cache_refresh_secs > 0) &&
        MonoTime::Now().GetDeltaSince(tablet_->refresh_time()).ToSeconds() >
        FLAGS_force_lookup_cache_refresh_secs) {

      refresh_cache = true;

      VLOG(1) << "Updating tablet " << tablet_->tablet_id() << " replicas cache "
              << "force_lookup_cache_refresh_secs: " << FLAGS_force_lookup_cache_refresh_secs
              << ". " << MonoTime::Now().GetDeltaSince(tablet_->refresh_time()).ToSeconds()
              << " seconds since the last update. Replicas in current cache: "
              << tablet_->ReplicasAsString();
    } else if (FLAGS_lookup_cache_refresh_secs > 0 &&
               MonoTime::Now().GetDeltaSince(tablet_->refresh_time()).ToSeconds() >
               FLAGS_lookup_cache_refresh_secs &&
               !tablet_->IsReplicasCountConsistent()) {
      refresh_cache = true;
      VLOG(1) << "Updating tablet " << tablet_->tablet_id() << " replicas cache "
              << "force_lookup_cache_refresh_secs: " << FLAGS_force_lookup_cache_refresh_secs
              << ". " << MonoTime::Now().GetDeltaSince(tablet_->refresh_time()).ToSeconds()
              << " seconds since the last update. Replicas in current cache: "
              << tablet_->ReplicasAsString();
    }


    if (refresh_cache) {
      client_->LookupTabletById(tablet_id_,
                                table_,
                                retrier_->deadline(),
                                std::bind(&TabletInvoker::LookupTabletCb, this, _1),
                                UseCache::kFalse);
      return;
    }
  }

  // Sets current_ts_.
  if (local_tserver_only_) {
    SelectLocalTabletServer();
  } else if (consistent_prefix_ && !leader_only) {
    SelectTabletServerWithConsistentPrefix();
  } else {
    SelectTabletServer();
  }

  // If we've tried all replicas, force a lookup to the master to find the
  // new leader. This relies on some properties of LookupTabletByKey():
  // 1. The fast path only works when there's a non-failed leader (which we
  //    know is untrue here).
  // 2. The slow path always fetches consensus configuration information and
  //    updates the looked-up tablet.
  // Put another way, we don't care about the lookup results at all; we're
  // just using it to fetch the latest consensus configuration information.
  //
  // TODO(tsplit): When we support tablet splits, we should let the lookup shift
  // the write to another tablet (i.e. if it's since been split).
  if (!current_ts_) {
    client_->LookupTabletById(tablet_id_,
                              table_,
                              retrier_->deadline(),
                              std::bind(&TabletInvoker::LookupTabletCb, this, _1),
                              UseCache::kTrue);
    return;
  }

  // Make sure we have a working proxy before sending out the RPC.
  auto status = current_ts_->InitProxy(client_);

  // Fail to a replica in the event of a DNS resolution failure.
  if (!status.ok()) {
    status = FailToNewReplica(status);
    if (!status.ok()) {
      command_->Finished(status);
    }
    return;
  }

  VLOG(2) << "Tablet " << tablet_id_ << ": Writing batch to replica "
          << current_ts_->ToString();

  rpc_->SendRpcToTserver(retrier_->attempt_num());
}

Status TabletInvoker::FailToNewReplica(const Status& reason,
                                       const tserver::TabletServerErrorPB* error_code) {
  if (ErrorCode(error_code) == tserver::TabletServerErrorPB::STALE_FOLLOWER) {
    VLOG(1) << "Stale follower for " << command_->ToString() << " just retry";
  } else if (ErrorCode(error_code) == tserver::TabletServerErrorPB::NOT_THE_LEADER) {
    VLOG(1) << "Not the leader for " << command_->ToString()
            << " retrying with a different replica";
    // In the past we were marking a replica as failed whenever an error was returned. The problem
    // with this approach is that not all type of errors mean that the replica has failed. Some
    // errors like NOT_THE_LEADER are only specific to certain type of requests (Write and
    // UpdateTransaction RPCs), but other type of requests don't need to be sent to the leader
    // (consistent prefix reads). So instead of marking a replica as failed for all the RPCs (since
    // the RemoteTablet object is shared across all the rpcs in the same batcher), this remote
    // tablet server is marked as a follower so that it's not used during a retry for requests that
    // need to contact the leader only. This has the same effect as marking the replica as failed
    // for this specific RPC, but without affecting other RPCs.
    followers_.emplace(current_ts_, FollowerData {
      .status = STATUS(IllegalState, "Not the leader"),
      .time = CoarseMonoClock::now()
    });
  } else {
    VLOG(1) << "Failing " << command_->ToString() << " to a new replica: " << reason
            << ", old replica: " << yb::ToString(current_ts_);

    bool found = !tablet_ || tablet_->MarkReplicaFailed(current_ts_, reason);
    if (!found) {
      // Its possible that current_ts_ is not part of replicas if RemoteTablet.Refresh() is invoked
      // which updates the set of replicas.
      LOG(WARNING) << "Tablet " << tablet_id_ << ": Unable to mark replica "
                   << current_ts_->ToString()
                   << " as failed. Replicas: " << tablet_->ReplicasAsString();
    }
  }

  auto status = retrier_->DelayedRetry(command_, reason);
  if (!status.ok()) {
    LOG(WARNING) << "Failed to schedule retry on new replica: " << status;
  }
  return status;
}

bool TabletInvoker::Done(Status* status) {
  TRACE_TO(trace_, "Done($0)", status->ToString(false));
  ADOPT_TRACE(trace_);

  bool assign_new_leader = assign_new_leader_;
  assign_new_leader_ = false;

  if (status->IsAborted() || retrier_->finished()) {
    if (status->ok()) {
      *status = retrier_->controller().status();
      if (status->ok()) {
        *status = STATUS(Aborted, "Retrier finished");
      }
    }
    return true;
  }

  // Prefer early failures over controller failures.
  if (status->ok() && retrier_->HandleResponse(command_, status)) {
    return false;
  }

  // Failover to a replica in the event of any network failure.
  //
  // TODO: This is probably too harsh; some network failures should be
  // retried on the current replica.
  if (status->IsNetworkError()) {
    // The whole operation is completed if we can't schedule a retry.
    return !FailToNewReplica(*status).ok();
  }

  // Prefer controller failures over response failures.
  auto rsp_err = rpc_->response_error();
  {
    Status resp_error_status = ErrorStatus(rsp_err);
    if (status->ok() && !resp_error_status.ok()) {
      *status = resp_error_status;
    } else if (status->IsRemoteError()) {
      if (!resp_error_status.ok()) {
        *status = resp_error_status;
      } else {
        const auto* error = retrier_->controller().error_response();
        if (error &&
            (error->code() == rpc::ErrorStatusPB::FATAL_SERVER_SHUTTING_DOWN ||
             error->code() == rpc::ErrorStatusPB::ERROR_NO_SUCH_SERVICE)) {
          *status = STATUS(ServiceUnavailable, error->message());
        }
      }
    }
  }

  const bool is_tablet_split = ErrorCode(rsp_err) == tserver::TabletServerErrorPB::TABLET_SPLIT;
  if (is_tablet_split || ClientError(*status) == ClientErrorCode::kTablePartitionsAreStale) {
    // Replace status error with TryAgain, so upper layer retry request after refreshing
    // table partitioning metadata.
    *status = status->CloneAndReplaceCode(Status::kTryAgain);
    if (is_tablet_split) {
      tablet_->MarkAsSplit();
    }
    rpc_->Failed(*status);
    return true;
  }

  // Oops, we failed over to a replica that wasn't a LEADER. Unlikely as
  // we're using consensus configuration information from the master, but still possible
  // (e.g. leader restarted and became a FOLLOWER). Try again.
  //
  // TODO: IllegalState is obviously way too broad an error category for
  // this case.
  if (status->IsIllegalState() || status->IsServiceUnavailable() || status->IsAborted() ||
      status->IsLeaderNotReadyToServe() || status->IsLeaderHasNoLease() ||
      TabletNotFoundOnTServer(rsp_err, *status) ||
      (status->IsTimedOut() && CoarseMonoClock::Now() < retrier_->deadline())) {
    VLOG(4) << "Retryable failure: " << *status
            << ", response: " << yb::ToString(rsp_err);

    const bool leader_is_not_ready =
        ErrorCode(rsp_err) ==
            tserver::TabletServerErrorPB::LEADER_NOT_READY_TO_SERVE ||
        status->IsLeaderNotReadyToServe();

    // If the leader just is not ready - let's retry the same tserver.
    // Else the leader became a follower and must be reset on retry.
    if (!leader_is_not_ready) {
      followers_.emplace(current_ts_, FollowerData {
        .status = *status,
        .time = CoarseMonoClock::now()
      });
    }

    if (PREDICT_FALSE(FLAGS_TEST_assert_local_op) && current_ts_->IsLocal() &&
        status->IsIllegalState()) {
      CHECK(false) << "Operation is not local";
    }

    // If only local tserver is requested and it is not the leader, respond error and done.
    // Otherwise, continue below to retry.
    if (local_tserver_only_ && current_ts_->IsLocal() && status->IsIllegalState()) {
      rpc_->Failed(*status);
      return true;
    }

    if (status->IsIllegalState() || TabletNotFoundOnTServer(rsp_err, *status)) {
      // The whole operation is completed if we can't schedule a retry.
      return !FailToNewReplica(*status, rsp_err).ok();
    } else {
      tserver::TabletServerDelay delay(*status);
      auto retry_status = delay.value().Initialized()
          ? retrier_->DelayedRetry(command_, *status, delay.value())
          : retrier_->DelayedRetry(command_, *status);
      if (!retry_status.ok()) {
        command_->Finished(retry_status);
      }
    }
    return false;
  }

  if (!status->ok()) {
    if (status->IsTimedOut()) {
      VLOG(1) << "Call to " << yb::ToString(tablet_) << " timed out. Marking replica "
              << yb::ToString(current_ts_) << " as failed.";
      if (tablet_ != nullptr && current_ts_ != nullptr) {
        tablet_->MarkReplicaFailed(current_ts_, *status);
      }
    }
    if (status->IsExpired() && rpc_->ShouldRetryExpiredRequest()) {
      client_->MaybeUpdateMinRunningRequestId(
          tablet_->tablet_id(), MinRunningRequestIdStatusData(*status).value());
      *status = STATUS(TryAgain, status->message());
    }
    std::string current_ts_string;
    if (current_ts_) {
      current_ts_string = Format("on tablet server $0", *current_ts_);
    } else {
      current_ts_string = "(no tablet server available)";
    }
    Status log_status = status->CloneAndPrepend(
        Format("Failed $0 to tablet $1 $2 after $3 attempt(s)",
               command_->ToString(),
               tablet_id_,
               current_ts_string,
               retrier_->attempt_num()));
    if (status->IsTryAgain() || status->IsExpired() || status->IsAlreadyPresent()) {
      YB_LOG_EVERY_N_SECS(INFO, 1) << log_status;
    } else {
      YB_LOG_EVERY_N_SECS(WARNING, 1) << log_status;
    }
    rpc_->Failed(*status);
  } else if (assign_new_leader && current_ts_) {
    bool assigned = tablet_->MarkTServerAsLeader(current_ts_);
    LOG_IF(INFO, !assigned)
        << "Unable to mark as leader: " << current_ts_->ToString() << " for "
        << tablet_->ToString();
  }

  return true;
}

void TabletInvoker::InitialLookupTabletDone(const Result<RemoteTabletPtr>& result) {
  VLOG(1) << "InitialLookupTabletDone(" << result << ")";

  if (result.ok()) {
    tablet_ = *result;
    Execute(std::string());
  } else {
    command_->Finished(result.status());
  }
}

bool TabletInvoker::IsLocalCall() const {
  return current_ts_ != nullptr && current_ts_->IsLocal();
}

std::shared_ptr<tserver::TabletServerServiceProxy> TabletInvoker::proxy() const {
  return current_ts_->proxy();
}

::yb::HostPort TabletInvoker::ProxyEndpoint() const {
  return current_ts_->ProxyEndpoint();
}

void TabletInvoker::LookupTabletCb(const Result<RemoteTabletPtr>& result) {
  VLOG(1) << "LookupTabletCb(" << yb::ToString(result) << ")";

  if (result.ok()) {
#ifndef DEBUG
    TRACE_TO(trace_, Format("LookupTabletCb($0)", *result));
#else
    TRACE_TO(trace_, "LookupTabletCb(OK)");
#endif
  } else {
    TRACE_TO(trace_, "LookupTabletCb($0)", result.status().ToString(false));
  }

  // We should retry the RPC regardless of the outcome of the lookup, as
  // leader election doesn't depend on the existence of a master at all.
  // Unless we know that this status is persistent.
  // For instance if tablet was deleted, we would always receive "Not found".
  if (!result.ok() && (result.status().IsNotFound() ||
                       ClientError(result.status()) == ClientErrorCode::kTablePartitionsAreStale)) {
    command_->Finished(result.status());
    return;
  }

  // Retry() imposes a slight delay, which is desirable in a lookup loop,
  // but unnecessary the first time through. Seeing as leader failures are
  // rare, perhaps this doesn't matter.
  followers_.clear();
  auto retry_status = retrier_->DelayedRetry(
      command_, result.ok() ? Status::OK() : result.status());
  if (!retry_status.ok()) {
    command_->Finished(!result.ok() ? result.status() : retry_status);
  }
}

Status ErrorStatus(const tserver::TabletServerErrorPB* error) {
  return error == nullptr ? Status::OK()
                          : StatusFromPB(error->status());
}

tserver::TabletServerErrorPB_Code ErrorCode(const tserver::TabletServerErrorPB* error) {
  return error == nullptr ? tserver::TabletServerErrorPB::UNKNOWN_ERROR
                          : error->code();
}

} // namespace internal
} // namespace client
} // namespace yb
