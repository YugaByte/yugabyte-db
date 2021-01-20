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

package com.yugabyte.sample.common.metrics;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import org.apache.log4j.Logger;

public class MetricsTracker extends Thread {
  private static final Logger LOG = Logger.getLogger(MetricsTracker.class);

  // Interface to print custom messages.
  public static interface StatusMessageAppender {
    public String appenderName();
    public void appendMessage(StringBuilder sb);
  }

  // The type of metrics supported.
  public static enum MetricName {
    Read,
    Write,
  }
  // Map to store all the metrics objects.
  Map<MetricName, Metric> metrics = new ConcurrentHashMap<MetricName, Metric>();
  // Track reads.
  Metric reads = new Metric("Reads");
  // Track writes.
  Metric writes = new Metric("Writes");
  // State variable to make sure this thread is started exactly once.
  boolean hasStarted = false;
  Object initLock = new Object();
  // Map of custom appenders.
  Map<String, StatusMessageAppender> appenders =
      new ConcurrentHashMap<String, StatusMessageAppender>();

  public MetricsTracker() {
    this.setDaemon(true);
  }

  public void registerStatusMessageAppender(StatusMessageAppender appender) {
    appenders.put(appender.appenderName(), appender);
  }

  public void createMetric(MetricName metricName) {
    synchronized (initLock) {
      if (!metrics.containsKey(metricName)) {
        metrics.put(metricName, new Metric(metricName.name()));
      }
    }
  }

  public Metric getMetric(MetricName metricName) {
    return metrics.get(metricName);
  }

  public void getMetricsAndReset(StringBuilder sb) {
    for (MetricName metricName : MetricName.values()) {
      sb.append(String.format("%s  |  ", metrics.get(metricName).getMetricsAndReset()));
    }
  }

  @Override
  public void start() {
    synchronized (initLock) {
      if (!hasStarted) {
        hasStarted = true;
        super.start();
      }
    }
  }

  @Override
  public void run() {
    while (true) {
      try {
        Thread.sleep(5000);
        StringBuilder sb = new StringBuilder();
        getMetricsAndReset(sb);
        for (StatusMessageAppender appender : appenders.values()) {
          appender.appendMessage(sb);
        }
        LOG.info(sb.toString());
      } catch (InterruptedException e) {}
    }
  }
}
