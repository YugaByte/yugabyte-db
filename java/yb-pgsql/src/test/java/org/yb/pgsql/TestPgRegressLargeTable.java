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
import java.util.Map;
import java.time.Instant;

import static org.yb.AssertionWrappers.*;

/**
 * Runs the pg_regress test suite on YB code.
 */
@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgRegressLargeTable extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgRegressLargeTable.class);

  private static final String TURN_OFF_COPY_FROM_BATCH_TRANSACTION =
      "yb_default_copy_from_rows_per_transaction=0";

  @Override
  public int getTestMethodTimeoutSec() {
    return 1800;
  }

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flags = super.getTServerFlags();
    flags.put("ysql_pg_conf", TURN_OFF_COPY_FROM_BATCH_TRANSACTION);
    return flags;
  }

  @Test
  public void testPgRegressLargeTable() throws Exception {
    // Run schedule, check time for release build.
    runPgRegressTest("yb_large_table_serial_schedule");

    // Number of executions for each statement.
    // Performance number is the average run time for each execution.
    final int execCount = 3;

    try (Statement stmt = connection.createStatement()) {
      // Check elapsed time.
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT 1 FROM airports LIMIT 1",
                                     1 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     2000 /* maxTotalMillis */);

      // Check time when selecting less than 4096 rows (YugaByte default prefetch limit).
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT 1 FROM airports LIMIT 1 OFFSET 1000",
                                     1 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     2000 /* maxTotalMillis */);

      // Check time when selecting more than 4096 rows (YugaByte default prefetch limit).
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT 1 FROM airports LIMIT 1 OFFSET 5000",
                                     1 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     3000 /* maxTotalMillis */);

      // Check aggregate functions.
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT count(*) FROM airports",
                                     1 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     4000 /* maxTotalMillis */);

      // Check large result set.
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT * FROM airports",
                                     9999 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     10000 /* maxTotalMillis */);

      // Check large result set with WHERE clause.
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT * FROM airports WHERE ident < '04' AND ident > '01'",
                                     188 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     10000 /* maxTotalMillis */);

      // Check large result set with WHERE clause.
      assertQueryRuntimeWithRowCount(stmt,
                                     "SELECT * FROM airports WHERE iso_region = 'US-CA'",
                                     488 /* expectedRowCount */,
                                     execCount /* numberOfRuns */,
                                     10000 /* maxTotalMillis */);
    }
  }
}
