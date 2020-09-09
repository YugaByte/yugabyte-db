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

#ifndef YB_CONSENSUS_RETRYABLE_REQUESTS_H
#define YB_CONSENSUS_RETRYABLE_REQUESTS_H

#include "yb/consensus/consensus_fwd.h"

#include "yb/util/restart_safe_clock.h"
#include "yb/util/status.h"

namespace yb {

class MetricEntity;
struct OpId;

namespace consensus {

struct RetryableRequestsCounts {
  size_t running = 0;
  size_t replicated = 0;
};

// Holds information about retryable requests.
class RetryableRequests {
 public:
  explicit RetryableRequests(std::string log_prefix = std::string());
  ~RetryableRequests();

  RetryableRequests(RetryableRequests&& rhs);
  void operator=(RetryableRequests&& rhs);

  // Tries to register a new running retryable request.
  // Returns false if request with such id is already present.
  bool Register(const ConsensusRoundPtr& round,
                RestartSafeCoarseTimePoint entry_time = RestartSafeCoarseTimePoint());

  // Cleans expires replicated requests and returns min op id of running request.
  yb::OpId CleanExpiredReplicatedAndGetMinOpId();

  // Mark appropriate request as replicated, i.e. move it from set of running requests to
  // replicated.
  void ReplicationFinished(
      const ReplicateMsg& replicate_msg, const Status& status, int64_t leader_term);

  // Adds new replicated request that was loaded during tablet bootstrap.
  void Bootstrap(const ReplicateMsg& replicate_msg, RestartSafeCoarseTimePoint entry_time);

  RestartSafeCoarseMonoClock& Clock();

  // Returns number or running requests and number of ranges of replicated requests.
  RetryableRequestsCounts TEST_Counts();

  void SetMetricEntity(const scoped_refptr<MetricEntity>& metric_entity);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace consensus
} // namespace yb

#endif // YB_CONSENSUS_RETRYABLE_REQUESTS_H
