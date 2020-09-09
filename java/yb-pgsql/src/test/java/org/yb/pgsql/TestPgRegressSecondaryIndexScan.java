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
package org.yb.pgsql;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.client.TestUtils;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.time.Instant;

import static org.yb.AssertionWrappers.*;

// Runs the pg_regress test suite on YB code.
// This test is running on our large table, "airports".
// CREATE TABLE airports(ident TEXT,
//                       type TEXT,
//                       name TEXT,
//                       elevation_ft INT,
//                       continent TEXT,
//                       iso_country TEXT,
//                       iso_region TEXT,
//                       municipality TEXT,
//                       gps_code TEXT,
//                       iata_code TEXT,
//                       local_code TEXT,
//                       coordinates TEXT,
//                       PRIMARY KEY (iso_region, name, type, ident));
//
// Index for SELECTing ybctid of the same airport country using HASH key.
//   CREATE INDEX airport_type_hash_idx ON airports(type HASH, iso_country ASC, ident ASC);
// Index for SELECTing ybctid of the same airport type using RANGE key.
//   CREATE INDEX airport_type_range_idx ON airports(name ASC, type ASC, ident ASC);

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgRegressSecondaryIndexScan extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgRegressSecondaryIndexScan.class);

  // Number of executions for each statement and the total run time for all executions.
  private final int execCount = 3;

  @Override
  public int getTestMethodTimeoutSec() {
    return 1800;
  }

  private void runSecondaryHashIndexScan(String airportType, int rowCount) throws Exception {
    // The following queries run index scan on "airport_type_hash_idx" to find ybctid, which is
    // then use to query data from "airports".
    final String rowQuery =
        "SELECT elevation_ft FROM airports WHERE ident IN " +
        "  (SELECT ident FROM airports WHERE type = '%s' AND iso_country > '0');";
    final String batchQuery =
        "SELECT elevation_ft FROM airports WHERE type = '%s' AND iso_country > '0';";

    LOG.info(String.format("Query: type = '%s', expected row count = %d", airportType, rowCount));

    try (Statement stmt = connection.createStatement()) {
      // Scan one row at a time.
      long rowScanTime =
          timeQueryWithRowCount(stmt, String.format(rowQuery, airportType), rowCount, execCount);
      LOG.info(String.format("Query one row at a time (row count = %d). Scan time = %d",
                             rowCount, rowScanTime));

      // Scan rows in batches.
      long batchScanTime =
          timeQueryWithRowCount(stmt, String.format(batchQuery, airportType), rowCount, execCount);
      LOG.info(String.format("Query rows in batches (row count = %d). Scan time = %d",
                             rowCount, batchScanTime));

      // Batch select run about 10x better, but the perf number for smaller data-set can varry.
      if (rowCount < 10000) {
        perfAssertLessThan(batchScanTime * 3, rowScanTime);
      } else {
        perfAssertLessThan(batchScanTime * 7, rowScanTime);
      }
    }
  }

  private void runSecondaryRangeIndexScan(String airportType, int rowCount) throws Exception {
    // The following queries run index scan on "airport_type_range_idx" to find ybctid, which is
    // then use to query data from "airports".
    final String rowQuery =
        "SELECT elevation_ft FROM airports WHERE ident IN" +
        "  (SELECT ident FROM airports WHERE type = '%s' AND name > '!');";
    final String batchQuery =
        "SELECT elevation_ft FROM airports WHERE type = '%s' AND name > '!';";

    LOG.info(String.format("Query: type = '%s', expected row count = %d", airportType, rowCount));

    try (Statement stmt = connection.createStatement()) {
      // Scan one row at a time.
      long rowScanTime =
          timeQueryWithRowCount(stmt, String.format(rowQuery, airportType), rowCount, execCount);
      LOG.info(String.format("Query one row at a time (row count = %d). Scan time = %d",
                             rowCount, rowScanTime));

      // Scan rows in batches.
      long batchScanTime =
          timeQueryWithRowCount(stmt, String.format(batchQuery, airportType), rowCount, execCount);
      LOG.info(String.format("Query rows in batches (row count = %d). Scan time = %d",
                             rowCount, batchScanTime));

      // Batch select run about 5 to 10x better, but the perf number for smaller data-set can varry.
      if (rowCount < 10000) {
        perfAssertLessThan(batchScanTime, rowScanTime);
      } else {
        perfAssertLessThan(batchScanTime * 4, rowScanTime);
      }
    }
  }

  private void testScan() throws Exception {
    // Number of 'large_airport' = 613 (Prefetch size is 1024).
    runSecondaryHashIndexScan("large_airport" /* airportType */,
                              613 /* rowCount */);

    // Number of 'medium_airport' = 4531 (Prefetch size is 1024).
    runSecondaryHashIndexScan("medium_airport" /* airportType */,
                              4531 /* rowCount */);

    // For non release build we don't need to run larger set of data, which is meant to check
    // performance instead of correctness.
    if (TestUtils.isReleaseBuild()) {
      // Number of 'heliport' = 11316 (Prefetch size is 1024).
      runSecondaryHashIndexScan("heliport" /* airportType */,
                                11316 /* rowCount */);

      // Number of 'small_airport' = 33998 (Prefetch size is 1024).
      runSecondaryHashIndexScan("small_airport" /* airportType */,
                                33998 /* rowCount */);
    }

    // Number of 'large_airport' = 613 (Prefetch size is 1024).
    runSecondaryRangeIndexScan("large_airport" /* airportType */,
                               613 /* rowCount */);

    // Number of 'medium_airport' = 4531 (Prefetch size is 1024).
    runSecondaryRangeIndexScan("medium_airport" /* airportType */,
                               4531 /* rowCount */);

    // For non release build we don't need to run larger set of data, which is meant to check
    // performance instead of correctness.
    if (TestUtils.isReleaseBuild()) {
      // Number of 'heliport' = 11316 (Prefetch size is 1024).
      runSecondaryRangeIndexScan("heliport" /* airportType */,
                                 11316 /* rowCount */);

      // Number of 'small_airport' = 33998 (Prefetch size is 1024).
      runSecondaryRangeIndexScan("small_airport" /* airportType */,
                                 33998 /* rowCount */);
    }
  }

  @Test
  public void testPgRegressSecondaryIndexScan() throws Exception {
    // Run schedule, check time for release build.
    runPgRegressTest("yb_secondary_index_scan_serial_schedule",
                     getPerfMaxRuntime(0, 0, 0, 0, 0) /* maxRuntimeMillis */);

    // Test secondary index scan.
    testScan();
  }
}
