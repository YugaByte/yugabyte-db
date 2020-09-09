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
//                       PRIMARY KEY (iso_region HASH, ident ASC));
//
// The test also runs on the following indexes beside the PRIMARY index.
// CREATE INDEX airports_idx1 ON airports(iso_region hash, name DESC);
// CREATE INDEX airports_idx2 ON airports(iso_region ASC, gps_code ASC);
// CREATE INDEX airports_idx3 ON airports((iso_region, elevation_ft) HASH,
//                                        coordinates, ident, name);

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgRegressHashIndex extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgRegressHashIndex.class);

  @Override
  public int getTestMethodTimeoutSec() {
    return 1800;
  }

  @Test
  public void testPgRegressHashIndex() throws Exception {
    // Run schedule, check time for release build.
    runPgRegressTest("yb_hash_index_serial_schedule",
                     getPerfMaxRuntime(60000, 0, 0, 0, 0) /* maxRuntimeMillis */);

    // Number of executions for each statement and the total run time for all executions.
    final int execCount = 3;
    long scanTime;

    try (Statement stmt = connection.createStatement()) {
      // Find the full-scan time.
      long fullscanTime = timeQueryWithRowCount(stmt,
                                                "SELECT gps_code FROM airports",
                                                -1 /* expectedRowCount */,
                                                execCount);
      LOG.info(String.format("Full scan: %d ms", fullscanTime));

      //--------------------------------------------------------------------------------------------
      // Check elapsed time for various statements that use PRIMARY INDEX.
      //   PRIMARY KEY (iso_region HASH, ident ASC)
      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("PRIMARY-KEY-SCAN Test Start - PRIMARY KEY (iso_region HASH, ident ASC)");
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT count(*) FROM airports" +
                                       "  WHERE iso_region = 'US-CA'",
                                       -1 /* expectedRowCount */,
                                       execCount);
      perfAssertLessThan(scanTime * 10, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' LIMIT 1",
                                       1 /* expectedRowCount */,
                                       execCount);
      perfAssertLessThan(scanTime * 20, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                        "  WHERE iso_region = 'US-CA'" +
                                        "  ORDER BY ident LIMIT 1",
                                        1 /* expectedRowCount */,
                                        execCount);
      perfAssertLessThan(scanTime * 25, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA'" +
                                       "  ORDER BY ident ASC LIMIT 1",
                                       1 /* expectedRowCount */,
                                       execCount);
      perfAssertLessThan(scanTime * 25, fullscanTime);

      LOG.info("Reverse scan is expected to be slower");
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                        "  WHERE iso_region = 'US-CA'" +
                                        "  ORDER BY ident DESC LIMIT 1",
                                        1 /* expectedRowCount */,
                                        execCount);
      perfAssertLessThan(scanTime * 10, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA'" +
                                       "  ORDER BY iso_region, ident LIMIT 1",
                                       1 /* expectedRowCount */,
                                       execCount);
      perfAssertLessThan(scanTime * 25, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND ident >= '4' LIMIT 2",
                                       2 /* expectedRowCount */,
                                       execCount);
      perfAssertLessThan(scanTime * 25, fullscanTime);

      long opTime = timeQueryWithRowCount(stmt,
                                          "SELECT gps_code FROM airports" +
                                          "  WHERE iso_region = 'US-CA' AND ident < '4' LIMIT 2",
                                          2 /* expectedRowCount */,
                                          execCount);
      perfAssertLessThan(opTime * 25, fullscanTime);

      //--------------------------------------------------------------------------------------------
      // Check elapsed time for various statements that use the following INDEX.
      //   CREATE INDEX airports_idx1 ON airports(iso_region hash, name DESC);
      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("SECONDARY-INDEX-SCAN Start - airports_idx1 (iso_region hash, name DESC)");
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' ORDER BY name DESC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 25, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' ORDER BY name ASC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 25, fullscanTime);

      //--------------------------------------------------------------------------------------------
      // Check elapsed time for various statements that use the following INDEX.
      //   CREATE INDEX airports_idx2 ON airports(iso_region ASC, gps_code ASC);
      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("SECONDARY-INDEX-SCAN Start - airports_idx2 (iso_region ASC, gps_code ASC)");
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       " WHERE iso_region = 'US-CA' ORDER BY gps_code ASC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       " WHERE iso_region = 'US-CA' ORDER BY gps_code DESC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // Forward scan is a lot slower than reverse scan.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  ORDER BY iso_region, gps_code LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 5, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  ORDER BY iso_region ASC, gps_code ASC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 5, fullscanTime);

      // Reverse scan is faster.
      long reverseTime = timeQueryWithRowCount(stmt,
                                               "SELECT gps_code FROM airports" +
                                               "  ORDER BY iso_region DESC, gps_code DESC LIMIT 1",
                                               1,
                                               execCount);
      perfAssertLessThan(reverseTime * 25, fullscanTime);

      // The following statement is doing full scan due to ordering in different direction.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  ORDER BY iso_region ASC, gps_code DESC LIMIT 1",
                                       1,
                                       execCount);

      //--------------------------------------------------------------------------------------------
      // Check elapsed time for various statements that use the following INDEX.
      //   CREATE INDEX airports_idx3 ON airports((iso_region, type) HASH,
      //                                          coordinates, ident, name) INCLUDE (gps_code);
      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("SECONDARY-INDEX-SCAN (Has LIMIT, Fully covered) Start - airports_idx3" +
               " ((iso_region, type) HASH, coordinates, ident, name) INCLUDE (gps_code)");
      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates ASC, ident, name LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates DESC, ident DESC, name DESC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name ASC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // Wrong order direction for name (Must be all ASC or all DESC).
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name DESC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime, fullscanTime);

      // Wrong order direction for ident and name (Must be all ASC or all DESC).
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates DESC, ident, name LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime, fullscanTime);

      // This ORDER BY should not be optimized. Missing hash column.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA'" +
                                       "  ORDER BY type, coordinates LIMIT 1",
                                       1,
                                       execCount);

      // This ORDER BY statement behavior is dependent on the cost estimator. It might choose the
      // PRIMARY INDEX over this index.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY ident LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime, fullscanTime);

      //--------------------------------------------------------------------------------------------
      // The following test cases are also for airports_idx3 but they use "SELECT *", so the
      // selected fields are not fully covered by the index.
      // Check elapsed time for various statements that use the following INDEX.
      //   CREATE INDEX airports_idx3 ON airports((iso_region, type) HASH,
      //                                          coordinates, ident, name) INCLUDE (gps_code);

      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("SECONDARY-INDEX-SCAN (Has LIMIT, Not Fully covered) Start - airports_idx3" +
               " ((iso_region, type) HASH, coordinates, ident, name) INCLUDE (gps_code)");
      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates ASC, ident, name LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates DESC, ident DESC, name DESC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name ASC LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime * 15, fullscanTime);

      // Wrong order direction for name (Must be all ASC or all DESC).
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name DESC LIMIT 1",
                                       1,
                                       execCount);

      // Wrong order direction for ident and name (Must be all ASC or all DESC).
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates DESC, ident, name LIMIT 1",
                                       1,
                                       execCount);

      // This ORDER BY should not be optimized. Missing hash column.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA'" +
                                       "  ORDER BY type, coordinates LIMIT 1",
                                       1,
                                       execCount);

      // This ORDER BY statement behavior is dependent on the cost estimator. It might choose the
      // PRIMARY INDEX over this index.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY ident LIMIT 1",
                                       1,
                                       execCount);
      perfAssertLessThan(scanTime, fullscanTime);

      //--------------------------------------------------------------------------------------------
      // The following test cases are also for airports_idx3 but wihtout LIMIT clause.
      // Check elapsed time for various statements that use the following INDEX.
      //   CREATE INDEX airports_idx3 ON airports((iso_region, type) HASH,
      //                                          coordinates, ident, name) INCLUDE (gps_code);
      // Without LIMIT, the queries should be a bit slower.
      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("SECONDARY-INDEX-SCAN (No LIMIT, Fully covered) Start - airports_idx3" +
               " ((iso_region, type) HASH, coordinates, ident, name) INCLUDE (gps_code)");
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name",
                                       -1,
                                       execCount);
      perfAssertLessThan(scanTime * 10, fullscanTime);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident",
                                       -1,
                                       execCount);
      perfAssertLessThan(scanTime * 10, fullscanTime);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates",
                                       -1,
                                       execCount);
      perfAssertLessThan(scanTime * 10, fullscanTime);

      // This ORDER BY should not be optimized. Missing hash column.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA'" +
                                       "  ORDER BY type, coordinates",
                                       -1,
                                       execCount);

      // This ORDER BY statement behavior is dependent on the cost estimator. It might choose the
      // PRIMARY INDEX over this index.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT gps_code FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY ident",
                                       -1,
                                       execCount);
      perfAssertLessThan(scanTime, fullscanTime);

      //--------------------------------------------------------------------------------------------
      // The following test cases are also for airports_idx3 but not fully covered and no LIMIT.
      // Check elapsed time for various statements that use the following INDEX.
      //   CREATE INDEX airports_idx3 ON airports((iso_region, type) HASH,
      //                                          coordinates, ident, name) INCLUDE (gps_code);
      // Without LIMIT and not-fully-covered by the index, the queries should be slow.
      LOG.info("---------------------------------------------------------------------------------");
      LOG.info("SECONDARY-INDEX-SCAN (No LIMIT, Not Fully covered) Start - airports_idx3" +
               " ((iso_region, type) HASH, coordinates, ident, name) INCLUDE (gps_code)");
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident, name",
                                       -1,
                                       execCount);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates, ident",
                                       -1,
                                       execCount);

      // This ORDER BY should be HASH + RANGE optimized.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY coordinates",
                                       -1,
                                       execCount);

      // This ORDER BY should not be optimized. Missing hash column.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA'" +
                                       "  ORDER BY type, coordinates",
                                       -1,
                                       execCount);

      // This ORDER BY statement behavior is dependent on the cost estimator. It might choose the
      // PRIMARY INDEX over this index.
      scanTime = timeQueryWithRowCount(stmt,
                                       "SELECT * FROM airports" +
                                       "  WHERE iso_region = 'US-CA' AND type = 'small_airport'" +
                                       "  ORDER BY ident",
                                       -1,
                                       execCount);
      perfAssertLessThan(scanTime, fullscanTime);
    }
  }
}
