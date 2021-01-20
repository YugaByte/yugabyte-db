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

import org.yb.minicluster.MiniYBDaemon;


import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.ResultSet;
import java.sql.Statement;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertGreaterThanOrEqualTo;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestYSQLMetrics extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(MiniYBDaemon.class);

  @Test
  public void testMetrics() throws Exception {
    Statement statement = connection.createStatement();

    // DDL is non-txn.
    verifyStatementMetric(statement, "CREATE TABLE test (k int PRIMARY KEY, v int)",
                          OTHER_STMT_METRIC, 1, 0, true);

    // Select uses txn.
    verifyStatementMetric(statement, "SELECT * FROM test",
                          SELECT_STMT_METRIC, 1, 1, true);

    // Non-txn insert.
    verifyStatementMetric(statement, "INSERT INTO test VALUES (1, 1)",
                          INSERT_STMT_METRIC, 1, 0, true);
    // Txn insert.
    statement.execute("BEGIN");
    verifyStatementMetric(statement, "INSERT INTO test VALUES (2, 2)",
                          INSERT_STMT_METRIC, 1, 1, true);
    statement.execute("END");

    // Limit query on complex view (issue #3811).
    verifyStatementMetric(statement, "SELECT * FROM information_schema.key_column_usage LIMIT 1",
                          SELECT_STMT_METRIC, 1, 1, true);

    // Non-txn update.
    verifyStatementMetric(statement, "UPDATE test SET v = 2 WHERE k = 1",
                          UPDATE_STMT_METRIC, 1, 0, true);
    // Txn update.
    verifyStatementMetric(statement, "UPDATE test SET v = 3",
                          UPDATE_STMT_METRIC, 1, 1, true);

    // Non-txn delete.
    verifyStatementMetric(statement, "DELETE FROM test WHERE k = 2",
                          DELETE_STMT_METRIC, 1, 0, true);
    // Txn delete.
    verifyStatementMetric(statement, "DELETE FROM test",
                          DELETE_STMT_METRIC, 1, 1, true);

    // Invalid statement should not update metrics.
    verifyStatementMetric(statement, "INSERT INTO invalid_table VALUES (1)",
                          INSERT_STMT_METRIC, 0, 0, false);
  }

  @Test
  public void testMetricRows() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      verifyStatementMetricRows(
        stmt,"CREATE TABLE test (k INT PRIMARY KEY, v INT)",
        OTHER_STMT_METRIC, 1, 0);

      verifyStatementMetricRows(
        stmt, "INSERT INTO test VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5)",
        INSERT_STMT_METRIC, 1, 5);

      verifyStatementMetricRows(
        stmt, "UPDATE test SET v = v + 1 WHERE v % 2 = 0",
        UPDATE_STMT_METRIC, 1, 2);

      verifyStatementMetricRows(
        stmt, "SELECT count(k) FROM test",
        AGGREGATE_PUSHDOWNS_METRIC, 1, 1);

      verifyStatementMetricRows(
        stmt, "SELECT * FROM test",
        SELECT_STMT_METRIC, 1, 5);

      verifyStatementMetricRows(
        stmt, "INSERT INTO test VALUES (6, 6), (7, 7)",
        TRANSACTIONS_METRIC, 1, 2);

      // Single row transaction.
      verifyStatementMetricRows(
        stmt, "INSERT INTO test VALUES (8, 8)",
        TRANSACTIONS_METRIC, 0, 0);

      verifyStatementMetricRows(
        stmt, "DELETE FROM test",
        DELETE_STMT_METRIC, 1, 8);
    }
  }

  @Test
  public void testStatementStats() throws Exception {
    Statement statement = connection.createStatement();

    String stmt, stat_stmt;

    // DDL is non-txn.
    stmt      = "CREATE TABLE test (k int PRIMARY KEY, v int)";
    stat_stmt = "CREATE TABLE test (k int PRIMARY KEY, v int)";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Select uses txn.
    stmt = "SELECT * FROM test";
    stat_stmt = stmt;
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Select uses txn - test reset.
    verifyStatementStatWithReset(statement, stmt, stat_stmt, 100, 200);

    // Non-txn insert.
    stmt      = "INSERT INTO test VALUES (1, 1)";
    stat_stmt = "INSERT INTO test VALUES ($1, $2)";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Multiple non-txn inserts to check statement fingerprinting.
    final int num_non_txn_inserts = 10;
    final int offset = 100;
    for (int i = 0; i < num_non_txn_inserts; i++) {
      int j = offset + i;
      stmt = "INSERT INTO test VALUES (" + j + ", " + j + ")";
      // Use same stat_stmt.
      verifyStatementStat(statement, stmt, stat_stmt, 1, true);
    }

    // Txn insert.
    statement.execute("BEGIN");
    stmt      = "INSERT INTO test VALUES (2, 2)";
    stat_stmt = "INSERT INTO test VALUES ($1, $2)";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);
    statement.execute("END");

    // Non-txn update.
    stmt      = "UPDATE test SET v = 2 WHERE k = 1";
    stat_stmt = "UPDATE test SET v = $1 WHERE k = $2";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Txn update.
    stmt      = "UPDATE test SET v = 3";
    stat_stmt = "UPDATE test SET v = $1";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Non-txn delete.
    stmt      = "DELETE FROM test WHERE k = 2";
    stat_stmt = "DELETE FROM test WHERE k = $1";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Txn delete.
    stmt      = "DELETE FROM test";
    stat_stmt = stmt;
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Limit query on complex view (issue #3811).
    stmt      = "SELECT * FROM information_schema.key_column_usage LIMIT 1";
    stat_stmt = "SELECT * FROM information_schema.key_column_usage LIMIT $1";
    verifyStatementStat(statement, stmt, stat_stmt, 1, true);

    // Invalid statement should not update metrics.
    stmt      = "INSERT INTO invalid_table VALUES (1)";
    stat_stmt = "INSERT INTO invalid_table VALUES ($1)";
    verifyStatementStat(statement, stmt, stat_stmt, 0, false);
  }

  @Test
  public void testStatementTime() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test(k INT, v VARCHAR)");
      String prepared_stmt =
          "PREPARE foo(INT, VARCHAR, INT, VARCHAR, INT, VARCHAR, INT, VARCHAR, INT, VARCHAR) " +
          "AS INSERT INTO test VALUES($1, $2), ($3, $4), ($5, $6), ($7, $8), ($9, $10)";
      statement.execute(prepared_stmt);
      statement.execute(
          "CREATE PROCEDURE proc(n INT) LANGUAGE PLPGSQL AS $$ DECLARE c INT := 0; BEGIN " +
          "WHILE c < n LOOP c := c + 1; INSERT INTO test VALUES(c, 'value'); END LOOP; END; $$");
      testStatement(statement,
          "INSERT INTO test VALUES(1, '1'), (2, '2'), (3, '3'), (4, '4'), (5, '5')",
          INSERT_STMT_METRIC,
          "INSERT INTO test VALUES($1, $2), ($3, $4), ($5, $6), ($7, $8), ($9, $10)");
      testStatement(statement,
                    "EXECUTE foo(1, '1', 2, '2', 3, '3', 4, '4', 5, '5')",
                    INSERT_STMT_METRIC,
                    prepared_stmt);
      testStatement(statement, "CALL proc(40)", OTHER_STMT_METRIC);
      testStatement(statement, "DO $$ BEGIN CALL proc(40); END $$", OTHER_STMT_METRIC);
    }
  }

  @Test
  public void testExplainTime() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test(k INT, v VARCHAR)");
      statement.execute(
          "CREATE OR REPLACE FUNCTION func(n INT) RETURNS INT AS $$ DECLARE c INT := 0; BEGIN " +
          "WHILE c < n LOOP c := c + 1; insert into test values(c, 'value'); END LOOP; " +
          "RETURN 0; END; $$ LANGUAGE PLPGSQL");
      final String query = "EXPLAIN(COSTS OFF, ANALYZE) SELECT func(500)";
      ResultSet result = statement.executeQuery(query);
      AgregatedValue stat = getStatementStat(query);
      assertEquals(1, stat.count);
      while(result.next()) {
        if(result.isLast()) {
          double query_time = Double.parseDouble(result.getString(1).replaceAll("[^\\d.]", ""));
          // As stat.total_time indicates total time of EXPLAIN query,
          // actual query total time is a little bit less.
          // It is expected that query time is not less than 90% of stat.total_time.
          assertQueryTime(query, query_time, 0.9 * stat.value);
        }
      }
    }
  }

  private void testStatement(Statement statement,
                             String query,
                             String metric_name) throws Exception {
    testStatement(statement, query, metric_name, query);
  }

  // Function executes query and compare time elapsed locally with query statistics.
  // Local time always will be greater due to client-server communication.
  // To reduce client-server communication delays query is executed multiple times as a batch.
  private void testStatement(Statement statement,
                             String query,
                             String metric_name,
                             String stat_name) throws Exception {
    final int count = 200;
    for(int i = 0; i < count; ++i) {
      statement.addBatch(query);
    }
    AgregatedValue metric_before = getMetric(metric_name);
    final long startTimeMillis = System.currentTimeMillis();
    statement.executeBatch();
    final double elapsed_local_time = System.currentTimeMillis() - startTimeMillis;
    AgregatedValue metric_after = getMetric(metric_name);
    AgregatedValue stat = getStatementStat(stat_name);
    assertEquals(String.format("Calls count for query %s", query), count, stat.count);
    assertEquals(String.format("'%s' count for query %s", metric_name, query),
                 count,
          metric_after.count - metric_before.count);
    // Due to client-server communications delay local time
    // is always greater than actual query time.
    // But we expect communications delay is less than 20%.
    final double time_lower_bound = 0.8 * elapsed_local_time;
    assertQueryTime(query, stat.value, time_lower_bound);
    final long metric_value = Math.round(metric_after.value - metric_before.value);
    // Make metric lower bound a little smaller than stat.value due to rounding
    final long metric_lower_bound = Math.round(0.95 * stat.value * 1000);
    assertGreaterThanOrEqualTo(
        String.format("Expected '%s' %d to be >= %d for query '%s'",
                      metric_name,
                      metric_value,
                      metric_lower_bound,
                      query),
        metric_value,
        metric_lower_bound);
  }

  private void assertQueryTime(String query, double query_time, double time_lower_bound) {
    assertGreaterThanOrEqualTo(
        String.format("Expected total time %f to be >= %f for query '%s'",
                      query_time,
                      time_lower_bound,
                      query),
        query_time,
        time_lower_bound);
  }
}
