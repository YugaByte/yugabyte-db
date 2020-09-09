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
#ifndef ENT_SRC_YB_CDC_CDC_METRICS_H
#define ENT_SRC_YB_CDC_CDC_METRICS_H

#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"

#include "yb/util/monotime.h"

#include "yb/tablet/tablet.h"

namespace yb {

class Counter;
template<class T>
class AtomicGauge;
class Histogram;
class MetricEntity;

namespace cdc {

// Container for all metrics specific to a single tablet.
class CDCTabletMetrics {
 public:
  explicit CDCTabletMetrics(const scoped_refptr<MetricEntity>& metric_entity_cdc);

  scoped_refptr<Histogram> rpc_payload_bytes_responded;
  scoped_refptr<Counter> rpc_heartbeats_responded;
  // For rpc_latency & rpcs_responded_count, use 'handler_latency_yb_cdc_CDCService_GetChanges'.

  // Info about ID last read by CDC Consumer.
  scoped_refptr<AtomicGauge<int64_t> > last_read_opid_term;
  scoped_refptr<AtomicGauge<int64_t> > last_read_opid_index;
  scoped_refptr<AtomicGauge<int64_t> > last_checkpoint_opid_index;
  scoped_refptr<AtomicGauge<uint64_t> > last_read_hybridtime;
  scoped_refptr<AtomicGauge<uint64_t> > last_read_physicaltime;
  scoped_refptr<AtomicGauge<uint64_t> > last_checkpoint_physicaltime;

  // Info about last majority-replicated OpID by CDC Producer (upon last poll).
  scoped_refptr<AtomicGauge<int64_t> > last_readable_opid_index;
  // For last_committed_hybridtime, use 'hybrid_clock_hybrid_time'.

  // Lag between commit time of last record polled and last record applied on producer.
  scoped_refptr<AtomicGauge<int64_t> > async_replication_sent_lag_micros;
  // Lag between last record applied on consumer and producer.
  scoped_refptr<AtomicGauge<int64_t> > async_replication_committed_lag_micros;

 private:
  scoped_refptr<MetricEntity> entity_;
};

class CDCServerMetrics {
 public:
  explicit CDCServerMetrics(const scoped_refptr<MetricEntity>& metric_entity_server);

  scoped_refptr<Counter> cdc_rpc_proxy_count;
  // Future Metric: scoped_refptr<Counter> cdc_rpc_error_count;

 private:
  scoped_refptr<MetricEntity> entity_;
};

} // namespace cdc
} // namespace yb
#endif // ENT_SRC_YB_CDC_CDC_METRICS_H
