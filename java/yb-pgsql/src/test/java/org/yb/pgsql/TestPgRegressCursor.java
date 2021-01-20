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
import static com.google.common.base.Preconditions.checkNotNull;
import static org.yb.AssertionWrappers.*;
import static org.yb.util.SanitizerUtil.isASAN;
import static org.yb.util.SanitizerUtil.isTSAN;

import java.sql.*;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.util.YBTestRunnerNonTsanOnly;

/**
 * Runs the pg_regress test suite on YB code.
 */
@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgRegressCursor extends BasePgSQLTest {
  @Override
  public int getTestMethodTimeoutSec() {
    return 1800;
  }

  // Counting the rows of a query.
  // Tests will use this rowcount to verify scanning result.
  private int getExpectedRowCount(String query) throws Exception {
    try (Statement stmt = connection.createStatement()) {
      return runQueryWithRowCount(stmt, query, -1);
    }
  }

  // Test scanning using a prepared query and check rowcount.
  private void testPreparedScan(String query, int expectedRowCount) throws Exception {
    // This test setup is following Postgres's document on JDBC.
    // "https://jdbc.postgresql.org/documentation/head/query.html".
    int rowCount = 0;
    try (PreparedStatement stmt = connection.prepareStatement(query)) {
      ResultSet rs = stmt.executeQuery();
      while (rs.next()) {
        rowCount++;
      }
    }
    if (expectedRowCount >= 0) {
      assertEquals(expectedRowCount, rowCount);
    }
  }

  // Test scanning using a cursor and check rowcount.
  private void testCursorScan(String query, int expectedRowCount) throws Exception {
    // This test setup is following Postgres's document on JDBC.
    // "https://jdbc.postgresql.org/documentation/head/query.html".

    // Turn auto commit OFF to instruct JDBC to use cursor.
    connection.setAutoCommit(false);

    // Scanning.
    try (Statement stmt = connection.createStatement()) {
      // Set fetch size to zero to force JDBC to select all rows into ResultSet at once.
      stmt.setFetchSize(0);
      int rowCount = 0;
      ResultSet rs = stmt.executeQuery(query);
      while (rs.next()) {
        rowCount++;
      }
      if (expectedRowCount >= 0) {
        assertEquals(expectedRowCount, rowCount);
      }

      // Set fetch size to 50 to force JDBC to select 50 rows at a time into ResultSet from server.
      stmt.setFetchSize(50);
      rowCount = 0;
      rs = stmt.executeQuery(query);

      // Delete all rows from the table after executing query to verify that JDBC can still read
      // the rest of the table after the deletion.
      try (Statement delete_stmt = connection.createStatement()) {
        delete_stmt.execute("DELETE FROM test_jdbc;");
      }
      while (rs.next()) {
        rowCount++;
      }
      rs.close();
      if (expectedRowCount >= 0) {
        assertEquals(expectedRowCount, rowCount);
      }
    }

    // Turn auto commit back ON (default setting).
    connection.setAutoCommit(true);
  }

  private void testCursor(String query) throws Exception {
    // Get rowcount.
    int expectedRowCount = getExpectedRowCount(query);

    // Run tests an prepared query and cursor.
    testPreparedScan(query, expectedRowCount);
    testCursorScan(query, expectedRowCount);
  }

  private void runTestCursor() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_jdbc AS SELECT * FROM onek");
      testCursor("SELECT * FROM test_jdbc");

      stmt.execute("DELETE FROM test_jdbc");
      stmt.execute("CREATE INDEX test_jdbc_idx ON test_jdbc(unique1)");
      stmt.execute("INSERT INTO test_jdbc SELECT * FROM onek");
      testCursor("SELECT * FROM test_jdbc WHERE unique1 > 500");
    }
  }

  @Test
  public void testPgRegressCursor() throws Exception {
    runPgRegressTest("yb_cursor_schedule");

    // Test CURSOR in JDBC.
    runTestCursor();
  }
}
