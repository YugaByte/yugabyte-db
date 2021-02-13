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

#include "yb/consensus/log_metrics.h"

#include "yb/util/metrics.h"

METRIC_DEFINE_counter(tablet, log_bytes_logged, "Bytes Written to WAL",
                      yb::MetricUnit::kBytes,
                      "Number of bytes logged since service start");

METRIC_DEFINE_counter(tablet, log_wal_files, "Number of Wal Files",
                      yb::MetricUnit::kUnits,
                      "Number of wal files");

METRIC_DEFINE_histogram(tablet, log_sync_latency, "Log Sync Latency",
                        yb::MetricUnit::kMicroseconds,
                        "Microseconds spent on synchronizing the log segment file",
                        60000000LU, 2);

METRIC_DEFINE_histogram(tablet, log_append_latency, "Log Append Latency",
                        yb::MetricUnit::kMicroseconds,
                        "Microseconds spent on appending to the log segment file",
                        60000000LU, 2);

METRIC_DEFINE_histogram(tablet, log_group_commit_latency, "Log Group Commit Latency",
                        yb::MetricUnit::kMicroseconds,
                        "Microseconds spent on committing an entire group",
                        60000000LU, 2);

METRIC_DEFINE_histogram(tablet, log_roll_latency, "Log Roll Latency",
                        yb::MetricUnit::kMicroseconds,
                        "Microseconds spent on rolling over to a new log segment file",
                        60000000LU, 2);

METRIC_DEFINE_histogram(tablet, log_entry_batches_per_group, "Log Group Commit Batch Size",
                        yb::MetricUnit::kRequests,
                        "Number of log entry batches in a group commit group",
                        1024, 2);

namespace yb {
namespace log {

#define MINIT(x) x(METRIC_log_##x.Instantiate(metric_entity))
LogMetrics::LogMetrics(const scoped_refptr<MetricEntity>& metric_entity)
    : MINIT(bytes_logged),
      MINIT(wal_files),
      MINIT(sync_latency),
      MINIT(append_latency),
      MINIT(group_commit_latency),
      MINIT(roll_latency),
      MINIT(entry_batches_per_group) {
}
#undef MINIT

} // namespace log
} // namespace yb
