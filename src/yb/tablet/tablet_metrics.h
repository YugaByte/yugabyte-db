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
#ifndef YB_TABLET_TABLET_METRICS_H
#define YB_TABLET_TABLET_METRICS_H

#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"

#include "yb/util/monotime.h"

namespace yb {

class Counter;
template<class T>
class AtomicGauge;
class Histogram;
class MetricEntity;

namespace tablet {

// Container for all metrics specific to a single tablet.
struct TabletMetrics {
  explicit TabletMetrics(const scoped_refptr<MetricEntity>& metric_entity);

  // Probe stats
  scoped_refptr<Histogram> commit_wait_duration;
  scoped_refptr<Histogram> snapshot_read_inflight_wait_duration;
  scoped_refptr<Histogram> redis_read_latency;
  scoped_refptr<Histogram> ql_read_latency;
  scoped_refptr<Histogram> write_lock_latency;
  scoped_refptr<Histogram> write_op_duration_client_propagated_consistency;
  scoped_refptr<Histogram> write_op_duration_commit_wait_consistency;

  scoped_refptr<Counter> not_leader_rejections;
  scoped_refptr<Counter> leader_memory_pressure_rejections;
  scoped_refptr<Counter> majority_sst_files_rejections;
  scoped_refptr<Counter> transaction_conflicts;
  scoped_refptr<Counter> expired_transactions;
  scoped_refptr<Counter> restart_read_requests;
  scoped_refptr<Counter> consistent_prefix_read_requests;
  scoped_refptr<Counter> pgsql_consistent_prefix_read_rows;

  scoped_refptr<Counter> rows_inserted;
};

class ScopedTabletMetricsTracker {
 public:
  explicit ScopedTabletMetricsTracker(scoped_refptr<Histogram> latency);
  ~ScopedTabletMetricsTracker();

 private:
  scoped_refptr<Histogram> latency_;
  MonoTime start_time_;
};

} // namespace tablet
} // namespace yb
#endif /* YB_TABLET_TABLET_METRICS_H */
