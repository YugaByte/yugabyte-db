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

package com.yugabyte.sample.common;

import java.util.Random;
import java.util.concurrent.atomic.AtomicLong;

import org.apache.log4j.Logger;

public class TimeseriesLoadGenerator {
  private static final Logger LOG = Logger.getLogger(TimeseriesLoadGenerator.class);

  Random random = new Random();
  // The data source id.
  String id;
  // The timestamp at which the data emit started.
  long dataEmitStartTs = -1;
  // State variable tracking the last timestamp emitted by this source (assumed to be the same
  // across all the nodes). -1 indicates no data point has been emitted.
  long lastEmittedTs = -1;
  // The time interval for generating data points. One data point is generated every
  // dataEmitRateMs milliseconds.
  long dataEmitRateMs;
  // The table level ttl used to calculate how many data points should be returned.
  long tableTTLMillis = -1;
  // If true, verification is enabled. Anytime the load generator detects that the load is not
  // keeping up with the DB writes, it turns verification off.
  Boolean verificationEnabled = true;
  // The write lag tracked as a variable.
  AtomicLong maxWriteLag = new AtomicLong(0);

  public TimeseriesLoadGenerator(int idx, long dataEmitRateMs, long tableTTLMillis) {
    // Add a unique uuid for each load tester.

    this.id = ((CmdLineOpts.loadTesterUUID != null)
                   ? idx + "-" + CmdLineOpts.loadTesterUUID
                   : "" + idx);
    this.dataEmitRateMs = dataEmitRateMs;
    this.tableTTLMillis = tableTTLMillis;
  }

  public String getId() {
    return id;
  }

  public boolean isVerificationEnabled() {
    return verificationEnabled;
  }

  public long getWriteLag() {
    return maxWriteLag.get();
  }

  /**
   * Returns the epoch time when the next data point should be emitted. Returns -1 if no data
   * point needs to be emitted in order to satisfy dataEmitRateMs.
   */
  public long getDataEmitTs() {
    long ts = System.currentTimeMillis();
    // Check if too little time has elapsed since the last data point was emitted.
    if (ts - lastEmittedTs < dataEmitRateMs) {
      return -1;
    }
    // Return the data point at the time boundary needed.
    if (lastEmittedTs == -1) {
      return ts - (ts % dataEmitRateMs);
    }
    // Check if we have skipped intervals, in which case we need to turn verification off.
    if ((ts - lastEmittedTs) / dataEmitRateMs > 1) {
      turnVerificationOff(ts);
    }
    return lastEmittedTs + dataEmitRateMs;
  }

  /**
   * @return true if this generator has emitted any data so far.
   */
  public boolean getHasEmittedData() {
    return (lastEmittedTs > -1);
  }

  /**
   * Callback from the user of this generator object to track the latest emitted data point. This
   * should be called after getting the next timestamp by calling getDataEmitTs() and persisting
   * the value in the db.
   * @param ts timestamp to set.
   */
  public synchronized void setLastEmittedTs(long ts) {
    // Set the time when we started emitting data.
    if (dataEmitStartTs == -1) {
      dataEmitStartTs = ts;
    }
    if (lastEmittedTs < ts) {
      lastEmittedTs = ts;
    }
  }

  /**
   * @return the timestamp when the latest data point was emitted.
   */
  public synchronized long getLastEmittedTs() {
    return lastEmittedTs;
  }

  /**
   * Returns the number of data points we expect to have written between the start and finish time.
   * Note that the start and end time are assumed to be exclusive - so any data point at those times
   * is not included in the calculation.
   * @param startTime the start time for the query.
   * @param endTime the end time for the query.
   * @return the number of data points between start and end.
   */
  public int getExpectedNumDataPoints(long startTime, long endTime) {
    // If verification is disabled, nothing to do.
    if (!verificationEnabled) {
      return -1;
    }
    // If we are too far in the future, we are not able to catch up. Disable verification.
    long ts = System.currentTimeMillis();
    if (ts - tableTTLMillis >= endTime) {
      turnVerificationOff(ts);
      return -1;
    }
    // Pick the more recent time between when we started emitting data and the start of our read
    // time interval, as there should be no data points returned before that.
    long effectiveStartTime = (startTime < dataEmitStartTs) ? dataEmitStartTs : startTime;
    if (endTime - effectiveStartTime > tableTTLMillis) {
      effectiveStartTime = endTime - tableTTLMillis;
    }
    long startTimeEmitBoundary = (effectiveStartTime / dataEmitRateMs + 1) * dataEmitRateMs;
    long endTimeEmitBoundary = endTime - endTime % dataEmitRateMs;
    long deltaT = endTimeEmitBoundary - startTimeEmitBoundary;
    int expectedRows = (int)(deltaT / dataEmitRateMs) + 1;
    LOG.debug("startTime: " + startTime + ", effectiveStartTime: " + effectiveStartTime +
              ", endTime: " + endTime + ", startTimeEmitBoundary : " + startTimeEmitBoundary +
              ", endTimeEmitBoundary: " + endTimeEmitBoundary + ", expectedRows: " + expectedRows);
    return expectedRows;
  }

  private void turnVerificationOff(long ts) {
    if (verificationEnabled) {
      synchronized (verificationEnabled) {
        verificationEnabled = false;
      }
    }
    maxWriteLag.set(ts - lastEmittedTs);
  }

  public String printDebugInfo(long startTime, long endTime) {
    // Pick the more recent time between when we started emitting data and the start of our read
    // time interval, as there should be no data points returned before that.
    long effectiveStartTime = (startTime < dataEmitStartTs) ? dataEmitStartTs : startTime;
    if (endTime - effectiveStartTime > tableTTLMillis) {
      effectiveStartTime = endTime - tableTTLMillis;
    }
    long startTimeEmitBoundary = (effectiveStartTime / dataEmitRateMs + 1) * dataEmitRateMs;
    long endTimeEmitBoundary = endTime - endTime % dataEmitRateMs;
    long deltaT = endTimeEmitBoundary - startTimeEmitBoundary;
    int expectedRows = (int)(deltaT / dataEmitRateMs) + 1;
    return "startTime: " + startTime + ", effectiveStartTime: " + effectiveStartTime +
           ", endTime: " + endTime + ", startTimeEmitBoundary : " + startTimeEmitBoundary +
           ", endTimeEmitBoundary: " + endTimeEmitBoundary + ", expectedRows: " + expectedRows;
  }
}
