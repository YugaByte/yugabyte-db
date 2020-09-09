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

import static org.yb.AssertionWrappers.*;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;

import static java.lang.Math.toIntExact;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgUpdate extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgUpdate.class);

  @Test
  public void testBasicUpdate() throws SQLException {
    String tableName = "test_basic_update";
    setupSimpleTable(tableName);

    // UPDATE with condition on partition columns.
    String query = String.format("SELECT h FROM %s WHERE h = 2 AND vi = 1000", tableName);
    try (Statement statement = connection.createStatement()) {
      assertNoRows(statement, query);
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("UPDATE %s SET vi = 1000 WHERE h = 2", tableName));
    }

    try (Statement statement = connection.createStatement()) {
      List<Row> rows = getRowList(statement.executeQuery(query));
      assertEquals(10, rows.size());
    }

    // UPDATE with condition on regular columns.
    query = String.format("SELECT h FROM %s WHERE vi = 2000", tableName);
    try (Statement statement = connection.createStatement()) {
      assertNoRows(statement, query);
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("UPDATE %s SET vi = 2*vi WHERE vi = 1000", tableName));
    }

    try (Statement statement = connection.createStatement()) {
      List<Row> rows = getRowList(statement.executeQuery(query));
      assertEquals(10, rows.size());
    }
  }

  @Test
  public void testUpdateWithSingleColumnKey() throws SQLException {
    String tableName = "test_update_single_column_key";
    try (Statement statement = connection.createStatement()) {
      createSimpleTableWithSingleColumnKey(tableName);
      String insertTemplate = "INSERT INTO %s(h, r, vi, vs) VALUES (%d, %f, %d, '%s')";

      for (int h = 0; h < 10; h++) {
        int r = h + 100;
        statement.execute(String.format(insertTemplate, tableName,
                                        h, r + 0.5, h * 10 + r, "v" + h + r));
      }
    }

    try (Statement statement = connection.createStatement()) {
      String query = String.format("SELECT h FROM %s WHERE h > 5 AND vi = 1000", tableName);
      assertNoRows(statement, query);
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("UPDATE %s SET vi = 1000 WHERE h > 5", tableName));
    }

    try (Statement statement = connection.createStatement()) {
      String query = String.format("SELECT h FROM %s WHERE h > 5 AND vi = 1000", tableName);
      List<Row> rows = getRowList(statement.executeQuery(query));
      assertEquals(4, rows.size());
    }
  }

  @Test
  public void testUpdateReturn() throws SQLException {
    String tableName = "test_update_return";
    createSimpleTable(tableName);

    List<Row> expectedRows = new ArrayList<>();
    try (Statement stmt = connection.createStatement()) {
      String insertFmt = "INSERT INTO %s(h, r, vi, vs) VALUES(%d, %f, %d, '%s')";
      for (long h = 0; h < 5; h++) {
        for (int r = 0; r < 5; r++) {
          String insertQuery = String.format(insertFmt, tableName,
                                             h, r + 0.5, h * 10 + r, "v" + h + r);
          if (h == 2 || h == 3) {
            // Constructring rows to be returned by UPDATE.
            expectedRows.add(new Row(h + 100L, r + 0.5 + 100, toIntExact(h * 10 + r + 2000)));
          }
          stmt.execute(insertQuery);
        }
      }
    }

    // Sort expected rows to match with result set.
    Collections.sort(expectedRows);

    try (Statement stmt = connection.createStatement()) {
      // Update with RETURNING clause.
      String updateQuery = String.format("UPDATE %s SET vi = vi + 1000 WHERE h = 2 OR h = 3 " +
                                         "RETURNING h + 100, r + 100, vi + 1000", tableName);
      stmt.execute(updateQuery);

      // Verify RETURNING clause.
      ResultSet returning = stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));
    }
  }

  @Test
  public void testUpdateEnforceConstraints() throws SQLException {
    String tableName = "test_update_enforce_constraints";

    List<Row> expectedRows = new ArrayList<>();

    try (Statement stmt = connection.createStatement()) {
      String createTableFmt = "CREATE TABLE %s(a INT PRIMARY KEY, b INT CHECK (b > 0))";
      stmt.execute(String.format(createTableFmt, tableName));

      String insertFmt = "INSERT INTO %s(a, b) VALUES(%d, %d) RETURNING a, b";

      // INSERT with invalid value will fail.
      String insertQuery = String.format(insertFmt, tableName, 1, -1);
      runInvalidQuery(stmt, insertQuery, "violates check constraint");

      ResultSet returning;
      // INSERT with valid value will succeed.
      for (int i = 1; i <= 5; ++i) {
        insertQuery = String.format(insertFmt, tableName, i, i);
        stmt.execute(insertQuery);
        expectedRows.add(new Row(i, i));
        returning = stmt.getResultSet();
        assertEquals(expectedRows.subList(i - 1, i), getSortedRowList(returning));
      }

      // UPDATE with invalid value will fail.
      runInvalidQuery(stmt, String.format("UPDATE %s SET b = -1 WHERE a = 1", tableName),
          "violates check constraint");
      stmt.execute(String.format("SELECT * FROM %s", tableName));
      returning = stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));

      // UPDATE multiple rows where some row will be invalid.
      runInvalidQuery(stmt, String.format("Update %s SET b = b - 2 WHERE a < 5", tableName),
          "violates check constraint");
      stmt.execute(String.format("SELECT * FROM %s", tableName));
      returning = stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));

      // UPDATE with valid value will succeed.
      stmt.execute(String.format("UPDATE %s SET b = b + 1", tableName));
      expectedRows.clear();
      for (int i = 1; i <= 5; ++i) {
        expectedRows.add(new Row(i, i + 1));
      }
      stmt.execute(String.format("SELECT * FROM %s", tableName));
      returning = stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));
    }
  }

  @Test
  public void testConcurrentUpdate() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_concurrent_update (k int primary key," +
                   " v1 int, v2 int, v3 int, v4 int)");
      stmt.execute("INSERT INTO test_concurrent_update VALUES" +
                                           " (0, 0, 0, 0, 0)");

      final List<Throwable> errors = new ArrayList<Throwable>();

      // Test concurrent update to individual columns from 1 to 100. They should not block one
      // another.
      List<Thread> threads = new ArrayList<Thread>();
      for (int i = 1; i <= 4; i++) {
        final int index = i;
        Thread thread = new Thread(() -> {
          try (PreparedStatement updateStmt = connection.prepareStatement(
                    String.format("UPDATE test_concurrent_update SET v%d = ? WHERE k = 0", index));
               PreparedStatement selectStmt = connection.prepareStatement(
                    String.format("SELECT v%d from test_concurrent_update WHERE k = 0", index))) {

            for (int j = 1; j <= 100; j++) {
              // Update column.
              updateStmt.setInt(1, j);
              updateStmt.execute();

              // Verify update.
              ResultSet rs = selectStmt.executeQuery();
              assertNextRow(rs, j);
            }

          } catch (Throwable e) {
            synchronized (errors) {
              errors.add(e);
            }
          }
        });
        thread.start();
        threads.add(thread);
      }

      for (Thread thread : threads) {
        thread.join();
      }

      // Verify final result of all columns.
      assertOneRow(stmt, "SELECT v1, v2, v3, v4 FROM test_concurrent_update WHERE k = 0",
                   100, 100, 100, 100);

      // Log the actual errors that occurred.
      for (Throwable e : errors) {
        LOG.error("Errors occurred", e);
      }
      assertTrue(errors.isEmpty());
    }
  }

  /*
   * Only test (the wire-protocol aspect of) prepared statements for expression pushdown here.
   * Rest of the tests for expression pushdown are in yb_dml_single_row (TestPgRegressDml).
   */
  @Test
  public void testExpressionPushdownPreparedStatements() throws Exception {
    String tableName = "test_update_expr_pushdown";
    setupSimpleTable(tableName);

    PreparedStatement updateStmt = connection.prepareStatement(
            "UPDATE test_update_expr_pushdown SET vi = vi + ?, vs = vs || ? " +
                    "WHERE h = 2 AND r = 2.5");
    int expectedVi = 22;
    String expectedVs = "v22";
    for (int i = 0; i < 20; i++) {
      // Use float instead of int to check bind param casting.
      float viInc = i * 10 + 5.4F;
      String vsConcat = "," + i;
      updateStmt.setFloat(1, viInc);
      updateStmt.setString(2, vsConcat);
      updateStmt.execute();
      expectedVi += viInc;
      expectedVs += vsConcat;
      assertOneRow(connection.createStatement(),
                   "SELECT vi,vs FROM test_update_expr_pushdown WHERE h = 2 AND r = 2.5",
                   expectedVi, expectedVs);
    }
  }

  @Test
  public void testUpdateFrom() throws SQLException {
    String tableName1 = "test_update_from";
    String tableName2 = "test_helper";
    createSimpleTable(tableName1);
    createSimpleTable(tableName2);

    // Fill in the helper table:
    try (Statement insert_stmt = connection.createStatement()) {
      insert_stmt.execute("INSERT INTO " + tableName2 + "(h, r, vi, vs) VALUES(1, 0.5, 10, 'v')");
    }

    List<Row> expectedRows = new ArrayList<>();
    try (Statement insert_stmt = connection.createStatement()) {
      String insert_format = "INSERT INTO %s(h, r, vi, vs) VALUES(%d, %f, %d, '%s')";
      for (long h = 0; h < 5; h++) {
        String insert_text = String.format(insert_format, tableName1,
                                           h, h + 0.5, h * 10 + h, "v" + h );
        if (h == 1) {
          // Constructing rows to be returned by UPDATE.
          expectedRows.add(new Row(h, h + 0.5, h * 10 + h, "l", 1, 0.5, 10, "v"));
        }
        insert_stmt.execute(insert_text);
      }
    }

    try (Statement update_stmt = connection.createStatement()) {
      // Testing FROM in UPDATE with RETURNING clause:
      update_stmt.execute("UPDATE " + tableName1 + " SET vs = 'l' FROM " + tableName2 +
                          " WHERE " + tableName1 + ".h  = " + tableName2 + ".h RETURNING *");

      // Verify RETURNING clause.
      ResultSet returning = update_stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));
    }
  }
}
