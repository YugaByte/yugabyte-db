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
import org.yb.util.RegexMatcher;

import java.math.BigDecimal;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;
import java.util.stream.Collectors;

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgSelect extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSelect.class);

  @Test
  public void testWhereClause() throws Exception {
    List<Row> allRows = setupSimpleTable("test_where");
    final String PRIMARY_KEY = "test_where_pkey";
    try (Statement statement = connection.createStatement()) {
      // Test no where clause -- select all rows.
      String query = "SELECT * FROM test_where";
      try (ResultSet rs = statement.executeQuery(query)) {
        assertEquals(allRows, getSortedRowList(rs));
      }
      assertFalse(isIndexScan(statement, query, PRIMARY_KEY));

      // Test fixed hash key.
      query = "SELECT * FROM test_where WHERE h = 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getLong(0).equals(2L))
            .collect(Collectors.toList());
        assertEquals(10, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertTrue(isIndexScan(statement, query, PRIMARY_KEY));

      // Test fixed primary key.
      query = "SELECT * FROM test_where WHERE h = 2 AND r = 3.5";
      try (ResultSet rs = statement.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getLong(0).equals(2L) &&
                row.getDouble(1).equals(3.5))
            .collect(Collectors.toList());
        assertEquals(1, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertTrue(isIndexScan(statement, query, PRIMARY_KEY));

      // Test fixed range key without fixed hash key.
      query = "SELECT * FROM test_where WHERE r = 6.5";
      try (ResultSet rs = statement.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getDouble(1).equals(6.5))
            .collect(Collectors.toList());
        assertEquals(10, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertFalse(isIndexScan(statement, query, PRIMARY_KEY));

      // Test range scan.
      query = "SELECT * FROM test_where WHERE h = 2 AND r >= 3.5 AND r < 8.5";
      try (ResultSet rs = statement.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getLong(0).equals(2L) &&
                row.getDouble(1) >= 3.5 &&
                row.getDouble(1) < 8.5)
            .collect(Collectors.toList());
        assertEquals(5, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertTrue(isIndexScan(statement, query, PRIMARY_KEY));

      // Test conditions on regular (non-primary-key) columns.
      query = "SELECT * FROM test_where WHERE vi < 14 AND vs != 'v09'";
      try (ResultSet rs = statement.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getInt(2) < 14 &&
                !row.getString(3).equals("v09"))
            .collect(Collectors.toList());
        // 14 options (for hash key) minus [9,'v09'].
        assertEquals(13, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertFalse(isIndexScan(statement, query, PRIMARY_KEY));

      // Test other WHERE operators (IN, OR, LIKE).
      query = "SELECT * FROM test_where WHERE h = 2 OR h = 3 OR vs LIKE 'v_2'";
      try (ResultSet rs = statement.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getLong(0).equals(2L) ||
                row.getLong(0).equals(3L) ||
                row.getString(3).matches("v.2"))
            .collect(Collectors.toList());
        // 20 plus 10 options but 2 common ones ('v22' and 'v32').
        assertEquals(28, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertFalse(isIndexScan(statement, query, PRIMARY_KEY));
    }
  }

  @Test
  public void testSelectTargets() throws SQLException {
    List<Row> allRows = setupSimpleTable("test_target");
    Statement statement = connection.createStatement();

    // Test all columns -- different order.
    try (ResultSet rs = statement.executeQuery("SELECT vs,vi,r,h FROM test_target")) {
      List<Row> expectedRows = allRows.stream()
          .map(row -> new Row(row.get(3), row.get(2), row.get(1), row.get(0)))
          .collect(Collectors.toList());
      assertEquals(expectedRows, getSortedRowList(rs));
    }

    // Test partial columns -- different order.
    try (ResultSet rs = statement.executeQuery("SELECT vs,r FROM test_target")) {
      List<Row> expectedRows = allRows.stream()
          .map(row -> new Row(row.get(3), row.get(1)))
          .collect(Collectors.toList());
      assertEquals(expectedRows, getSortedRowList(rs));
    }

    // Test aggregates.
    assertOneRow(statement, "SELECT avg(r) FROM test_target", 5.0D);
    assertOneRow(statement, "SELECT count(*) FROM test_target", 100L);
    assertOneRow(statement, "SELECT count(test_target.*) FROM test_target", 100L);

    // Test distinct.
    try (ResultSet rs = statement.executeQuery("SELECT distinct(h) FROM test_target")) {
      List<Row> expectedRows = allRows.stream()
          .map(row -> new Row(row.get(0)))
          .distinct()
          .collect(Collectors.toList());
      assertEquals(expectedRows, getSortedRowList(rs));
    }

    // Test selecting non-existent column.
    runInvalidQuery(statement, "SELECT v FROM test_target", "column \"v\" does not exist");

    // Test mistyped function.
    runInvalidQuery(statement, "SELECT vs * r FROM test_target", "operator does not exist");

    // Test aggregates from table without primary key.
    statement.execute("CREATE TABLE test_target_no_pkey(v1 int, v2 int)");
    statement.execute("INSERT INTO test_target_no_pkey(v1, v2) VALUES (1,2)");
    statement.execute("INSERT INTO test_target_no_pkey(v1, v2) VALUES (2,3)");
    statement.execute("INSERT INTO test_target_no_pkey(v1, v2) VALUES (3,4)");
    assertOneRow(statement, "SELECT sum(v1) FROM test_target_no_pkey", 6L);
    assertOneRow(statement, "SELECT count(*) FROM test_target_no_pkey", 3L);
    assertOneRow(statement, "SELECT sum(test_target_no_pkey.v2) FROM test_target_no_pkey", 9L);
  }

  @Test
  public void testComplexSelect() throws Exception {
    setupSimpleTable("test_clauses");
    Statement statement = connection.createStatement();

    // Test ORDER BY, OFFSET, and LIMIT clauses
    try (ResultSet rs = statement.executeQuery("SELECT h, r FROM test_clauses" +
                                                   " ORDER BY r ASC, h DESC LIMIT 27 OFFSET 17")) {
      int count = 0;
      int start = 17; // offset.
      int end = start + 27; // offset + limit.
      for (double r = 0.5; r < 10.5 && count < end; r += 1) {
        for (long h = 9; h >= 0 && count < end; h--) {
          if (count >= start) {
            assertTrue(rs.next());
            assertEquals(h, rs.getLong("h"));
            assertEquals(r, rs.getDouble("r"));
          }
          count++;
        }
      }
      assertFalse(rs.next());
    }

    // Test WITH clause (with RECURSIVE modifier).
    assertOneRow(statement,
                 "WITH RECURSIVE t(n) AS (" +
                     "    VALUES (1)" +
                     "  UNION ALL" +
                     "    SELECT n+1 FROM t WHERE n < 100" +
                     ")" +
                     "SELECT sum(n) FROM t",
                 5050L);
  }

  @Test
  public void testJoins() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE t1(h bigint, r float, v text, PRIMARY KEY (h, r))");
      statement.execute("CREATE TABLE t2(h bigint, r float, v text, PRIMARY KEY (h, r))");

      statement.execute("INSERT INTO t1(h, r, v) VALUES (1, 2.5, 'abc')");
      statement.execute("INSERT INTO t1(h, r, v) VALUES (1, 3.5, 'def')");
      statement.execute("INSERT INTO t1(h, r, v) VALUES (1, 4.5, 'xyz')");

      statement.execute("INSERT INTO t2(h, r, v) VALUES (1, 2.5, 'foo')");
      statement.execute("INSERT INTO t2(h, r, v) VALUES (1, 4.5, 'bar')");

      // Test simple join.
      String joinStmt = "SELECT a.h, a.r, b.h, b.r, a.v as av, b.v as bv " +
          "FROM t1 a JOIN t2 b ON (a.h = b.h and a.r = b.r)";
      try (ResultSet rs = statement.executeQuery(joinStmt)) {
        assertNextRow(rs, 1L, 2.5D, 1L, 2.5D, "abc", "foo");
        assertNextRow(rs, 1L, 4.5D, 1L, 4.5D, "xyz", "bar");
        assertFalse(rs.next());
      }

      // Test join with WHERE clause.
      joinStmt = "SELECT a.h, a.r, a.v as av, b.v as bv FROM t1 a LEFT JOIN t2 b " +
          "ON (a.h = b.h and a.r = b.r) WHERE a.h = 1 AND (a.r = 2.5 OR a.r = 3.5)";
      try (ResultSet rs = statement.executeQuery(joinStmt)) {
        assertNextRow(rs, 1L, 2.5D, "abc", "foo");
        assertNextRow(rs, 1L, 3.5D, "def", null);
        assertFalse(rs.next());
      }

      // Test views from join.
      statement.execute("CREATE VIEW t1_and_t2 AS " + joinStmt);
      assertOneRow(statement, "SELECT * FROM t1_and_t2 WHERE r > 3", 1L, 3.5D, "def", null);
    }
  }

  /**
   * Regression test for #1827.
   */
  @Test
  public void testJoinWithArraySearch() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int, name varchar, PRIMARY KEY (id))");
      statement.execute("CREATE TABLE join_table(id int, tid int, PRIMARY KEY (id))");

      statement.execute("INSERT INTO test_table VALUES (0, 'name 1')");
      statement.execute("INSERT INTO test_table VALUES (1, 'name 2')");
      statement.execute("INSERT INTO test_table VALUES (2, 'name 3')");

      statement.execute("INSERT INTO join_table VALUES (0, 0)");
      statement.execute("INSERT INTO join_table VALUES (1, 0)");
      statement.execute("INSERT INTO join_table VALUES (2, 1)");
      statement.execute("INSERT INTO join_table VALUES (3, 1)");
      statement.execute("INSERT INTO join_table VALUES (4, 2)");
      statement.execute("INSERT INTO join_table VALUES (5, 2)");

      assertQuery(statement, "SELECT tt.name, jt.id FROM test_table tt" +
              " INNER JOIN join_table jt ON tt.id = jt.tid" +
              " WHERE tt.id IN (0, 1)" +
              " ORDER BY jt.id",
          new Row("name 1", 0),
          new Row("name 1", 1),
          new Row("name 2", 2),
          new Row("name 2", 3));
    }
  }

  @Test
  public void testExpressions() throws Exception {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable("test_expr");

      // Insert a sample row: Row[2, 3.0, 4, 'abc'].
      statement.execute("INSERT INTO test_expr(h, r, vi, vs) VALUES (2, 3.0, 4, 'abc')");
      assertOneRow(statement, "SELECT * FROM test_expr", 2L, 3.0D, 4, "abc");

      // Test expressions in SELECT targets.
      assertOneRow(statement,
                   "SELECT h + 1.5, pow(r, 2), vi * h, 7 FROM test_expr WHERE h = 2",
                   new BigDecimal(3.5), 9.0D, 8L, 7);

      // Test expressions in SELECT WHERE clause.
      assertOneRow(statement,
                   "SELECT * FROM test_expr WHERE h + r <= 10 AND substring(vs from 2) = 'bc'",
                   2L, 3.0D, 4, "abc");
    }
  }

  @Test
  public void testPgsqlVersion() throws Exception {
    try (Statement statement = connection.createStatement()) {
      try (ResultSet rs = statement.executeQuery("SELECT version();")) {
          assertTrue(rs.next());
          assertThat(String.valueOf(rs.getArray(1)),
                     RegexMatcher.matchesRegex("PostgreSQL.*-YB-.*"));
          assertFalse(rs.next());
      }
      try (ResultSet rs = statement.executeQuery("show server_version;")) {
        assertTrue(rs.next());
        assertThat(String.valueOf(rs.getArray(1)),
                RegexMatcher.matchesRegex(".*-YB-.*"));
        assertFalse(rs.next());
      }
    }
  }

  private void verifyStatementPushdownMetric(Statement statement,
                                             String stmt,
                                             boolean pushdown_expected) throws Exception {
    verifyStatementMetric(statement, stmt, AGGREGATE_PUSHDOWNS_METRIC,
                          pushdown_expected ? 1 : 0, 1, true);
  }

  @Test
  public void testAggregatePushdowns() throws Exception {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable("aggtest");

      // Pushdown COUNT/MAX/MIN/SUM for INTEGER/FLOAT.
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(vi), MAX(vi), MIN(vi), SUM(vi) FROM aggtest", true);
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(r), MAX(r), MIN(r), SUM(r) FROM aggtest", true);

      // Don't pushdown if non-supported aggregate is provided (e.g. AVG, at least for now).
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(vi), AVG(vi) FROM aggtest", false);

      // Pushdown COUNT(*).
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(*) FROM aggtest", true);

      // Don't pushdown if there's a WHERE condition.
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(*) FROM aggtest WHERE h > 0", false);

      // Pushdown for BIGINT COUNT/MAX/MIN.
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(h), MAX(h), MIN(h) FROM aggtest", true);

      // Don't pushdown for BIGINT SUM.
      verifyStatementPushdownMetric(
          statement, "SELECT SUM(h) FROM aggtest", false);

      // Pushdown COUNT/MIN/MAX for text.
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(vs), MAX(vs), MIN(vs) FROM aggtest", true);

      // Pushdown shared aggregates.
      verifyStatementPushdownMetric(
          statement, "SELECT MAX(vi), MAX(vi) + 1 FROM aggtest", true);

      // Don't pushdown complicated expression in aggregate.
      verifyStatementPushdownMetric(
          statement, "SELECT MAX(vi + 1) FROM aggtest", false);

      // Don't pushdown window functions.
      verifyStatementPushdownMetric(
          statement, "SELECT h, COUNT(h) OVER (PARTITION BY h) FROM aggtest", false);

      // Don't pushdown if DISTINCT present.
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(DISTINCT vi) FROM aggtest", false);

      // Create table with NUMERIC/DECIMAL types.
      statement.execute("CREATE TABLE aggtest2 (n numeric, d decimal)");

      // Pushdown COUNT for NUMERIC/DECIMAL types.
      verifyStatementPushdownMetric(
          statement, "SELECT COUNT(n), COUNT(d) FROM aggtest2", true);

      // Don't pushdown SUM/MAX/MIN for NUMERIC/DECIMAL types.
      for (String col : Arrays.asList("n", "d")) {
        for (String agg : Arrays.asList("SUM", "MAX", "MIN")) {
          verifyStatementPushdownMetric(
              statement, "SELECT " + agg + "(" + col + ") FROM aggtest2", false);
        }
      }
    }
  }

  @Test
  public void testReverseScanMultiRangeCol() throws Exception {
    try (Statement statement = connection.createStatement()) {

      statement.execute("CREATE TABLE test_reverse_scan_multicol (h int, r1 int, r2 int, r3 int," +
                              " PRIMARY KEY (h, r1, r2, r3))");
      String insert_stmt = "INSERT INTO test_reverse_scan_multicol VALUES (1, %d, %d, %d)";

      for (int r1 = 1; r1 <= 5; r1++) {
        for (int r2 = 1; r2 <= 5; r2++) {
          for (int r3 = 1; r3 <= 5; r3++) {
            statement.execute(String.format(insert_stmt, r1, r2, r3));
          }
        }
      }

      // Test reverse scan with prefix bounds: r1[2, 4], r2(1,4).
      String select_stmt = "SELECT * FROM test_reverse_scan_multicol WHERE h = 1" +
                                          "AND r1 >= 2 AND r1 <= 4 AND r2 > 1 and r2 < 4" +
                                          "ORDER BY r1 DESC, r2 DESC, r3 DESC";
      ResultSet rs = statement.executeQuery(select_stmt);

      for (int r1 = 4; r1 >= 2; r1--) {
        for (int r2 = 3; r2 > 1; r2--) {
          for (int r3 = 5; r3 >= 1; r3--) {
            assertTrue(rs.next());
            assertEquals(r1, rs.getInt("r1"));
            assertEquals(r2, rs.getInt("r2"));
            assertEquals(r3, rs.getInt("r3"));
          }
        }
      }
      assertFalse(rs.next());

      // Test reverse scan with non-prefix bounds and LIMIT: r1[2, 4], r3[2, 3].
      // Total 3 * 5 * 2 = 30 rows but set LIMIT to 25.
      select_stmt = "SELECT * FROM test_reverse_scan_multicol WHERE h = 1" +
              "AND r1 >= 2 AND r1 <= 4 AND r3 > 1 and r3 < 4" +
              "ORDER BY r1 DESC, r2 DESC, r3 DESC LIMIT 25";
      rs = statement.executeQuery(select_stmt);

      int idx = 0;
      for (int r1 = 4; r1 >= 2 && idx < 25; r1--) {
        for (int r2 = 5; r2 >= 1 && idx < 25; r2--) {
          for (int r3 = 3; r3 > 1 && idx < 25; r3--) {
            assertTrue(rs.next());
            assertEquals(r1, rs.getInt("r1"));
            assertEquals(r2, rs.getInt("r2"));
            assertEquals(r3, rs.getInt("r3"));
            idx++;
          }
        }
      }
      assertFalse(rs.next());
    }
  }

  public void testNullPushdownUtil(String colOrder) throws Exception {
    String createTable = "CREATE TABLE %s(a int, b int, PRIMARY KEY(a %s))";
    String createIndex = "CREATE INDEX ON %s(b %s)";

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format(createTable, "t1", colOrder));
      statement.execute(String.format(createTable, "t2", colOrder));
      statement.execute("insert into t1 values (1,1), (2,2), (3,3)");
      statement.execute("insert into t2 values (1,1), (2,2), (3,null)");

      //--------------------------------------------------------------------------------------------
      // Test join where one join column is null.

      // Inner join, expect no rows.
      String query = "select * from t2 inner join t1 on t2.b = t1.a where t2.a = 3";
      assertNoRows(statement, query);
      String explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for t1 pkey",
                 explainOutput.contains("Index Cond: (a = 3)"));
      assertTrue("Expect pushdown for t2 pkey",
                 explainOutput.contains("Index Cond: (a = t2.b)"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // Outer join, expect one row.
      query = "select * from t2 full outer join t1 on t2.b = t1.a where t2.a = 3";
      assertOneRow(statement, query, 3, null, null, null);
      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for t1 pkey",
                 explainOutput.contains("Index Cond: (a = 3)"));
      assertTrue("Expect pushdown for t2 pkey",
                 explainOutput.contains("Index Cond: (t2.b = a)"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // -------------------------------------------------------------------------------------------
      // Test IS NULL and IS NOT NULL.

      // Add an index on t1.b that contains null value in its key.
      statement.execute("insert into t1 values (4,null), (5,null)");
      statement.execute(String.format(createIndex, "t1", colOrder));

      // Test IS NULL on pkey column.
      query = "select * from t1 where a IS NULL";
      assertNoRows(statement, query);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL",
                 explainOutput.contains("Index Cond: (a IS NULL)"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // Test IS NULL on index column.
      query = "select * from t1 where b IS NULL";
      Set<Row> expectedRows = new HashSet<>();
      expectedRows.add(new Row(4, null));
      expectedRows.add(new Row(5, null));
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL",
                 explainOutput.contains("Index Cond: (b IS NULL)"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // Test IS NOT NULL.
      query = "select * from t1 where b IS NOT NULL";
      expectedRows.clear();
      expectedRows.add(new Row(1, 1));
      expectedRows.add(new Row(2, 2));
      expectedRows.add(new Row(3, 3));
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect no pushdown for IS NOT NULL",
                 explainOutput.contains("Filter: (b IS NOT NULL)"));
      assertTrue("Expect YSQL-level filter",
                  explainOutput.contains("Rows Removed by Filter: 2"));

      // Test IN with NULL (should not match null row because null == null is not true).
      query = "select * from t1 where b IN (NULL, 2, 3)";
      expectedRows.clear();
      expectedRows.add(new Row(2, 2));
      expectedRows.add(new Row(3, 3));
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IN condition",
                 explainOutput.contains("Index Cond: (b = ANY ('{NULL,2,3}'::integer[]))"));
      assertFalse("Expect DocDB to filter fully",
                 explainOutput.contains("Rows Removed by"));

      // Test NOT IN with NULL (should not match anything because v1 != null is never true).
      query = "select * from t1 where b NOT IN (NULL, 2)";
      expectedRows.clear();
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect no pushdown for NOT IN condition",
                 explainOutput.contains("Filter: (b <> ALL ('{NULL,2}'::integer[]))"));
      assertTrue("Expect YSQL-level filtering",
                 explainOutput.contains("Rows Removed by Filter: 5"));

      // Test BETWEEN.
      query = "select * from t1 where b between 1 and 3";
      expectedRows.clear();
      expectedRows.add(new Row(1, 1));
      expectedRows.add(new Row(2, 2));
      expectedRows.add(new Row(3, 3));
      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertRowSet(statement, query, expectedRows);

      if (colOrder.equals("HASH")) {
        assertTrue("Expect no pushdown for BETWEEN condition on HASH",
                   explainOutput.contains("Filter: ((b >= 1) AND (b <= 3))"));
        assertTrue("Expect YSQL-level filtering for HASH",
                    explainOutput.contains("Rows Removed by Filter: 2"));
      } else {
        assertTrue("Expect pushdown for BETWEEN condition on ASC/DESC",
                   explainOutput.contains("Index Cond: ((b >= 1) AND (b <= 3))"));
        assertFalse("Expect no YSQL-level filtering for ASC/DESC",
                    explainOutput.contains("Rows Removed by"));
      }

      // Test BETWEEN with NULL.
      query = "select * from t1 where b BETWEEN 1 AND NULL";
      expectedRows.clear();
      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertRowSet(statement, query, expectedRows);
      assertTrue("YSQL will auto-eval condition to false",
                 explainOutput.contains("One-Time Filter: false"));
      assertFalse("Expect no YSQL-level filtering",
                  explainOutput.contains("Rows Removed by"));

      //--------------------------------------------------------------------------------------------
      // Test join where one join column is null *and* the other table has null rows for it.
      // TODO This should not matter because null == null is false per SQL semantics, but in DocDB
      //      null == null is true, so we still require filtering here (but only for rows where the
      //      respective column is null, not for the rest.

      // Inner join (on t1.b this time), expect no rows.
      query = "select * from t2 inner join t1 on t2.b = t1.b where t2.a = 3";
      assertNoRows(statement, query);
      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for t1 pkey",
                 explainOutput.contains("Index Cond: (a = 3)"));
      assertTrue("Expect pushdown for t2 pkey",
                 explainOutput.contains("Index Cond: (b = t2.b)"));
      assertTrue("Expect to filter only the 2 null rows",
                 explainOutput.contains("Rows Removed by Index Recheck: 2"));

      // Outer join (on t1.b this time), expect one row.
      query = "select * from t2 full outer join t1 on t2.b = t1.b where t2.a = 3";
      assertOneRow(statement, query, 3, null, null, null);
      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for t1 pkey",
                 explainOutput.contains("Index Cond: (a = 3)"));
      assertTrue("Expect pushdown for t2 pkey",
                 explainOutput.contains("Index Cond: (t2.b = b)"));
      assertTrue("Expect to filter only the 2 null rows",
                  explainOutput.contains("Rows Removed by Index Recheck: 2"));

      statement.execute("DROP TABLE t1");
      statement.execute("DROP TABLE t2");
    }
  }

  @Test
  public void testNullPushdown() throws Exception {
    testNullPushdownUtil("ASC");
    testNullPushdownUtil("HASH");
    testNullPushdownUtil("DESC");
  }

  @Test
  public void testMulticolumnNullPushdown() throws Exception {
    try (Statement statement = connection.createStatement()) {

      statement.execute("CREATE TABLE test(h int, r int, vh1 int, vh2 int, vr1 int, vr2 int)");
      statement.execute("CREATE INDEX on test((vh1, vh2) HASH, vr1 ASC, vr2 ASC)");
      statement.execute("INSERT INTO test values (1,1,1,1,1,1)");
      statement.execute("INSERT INTO test values (2,2,null,2,2,2)");
      statement.execute("INSERT INTO test values (3,3,3,null,3,3)");
      statement.execute("INSERT INTO test values (4,4,4,4,null,4)");
      statement.execute("INSERT INTO test values (5,5,5,5,5,null)");
      statement.execute("INSERT INTO test values (6,6,null,null,6,6)");
      statement.execute("INSERT INTO test values (7,7,7,7,null,null)");
      statement.execute("INSERT INTO test values (8,8,null,8,8,null)");
      statement.execute("INSERT INTO test values (9,9,null,null,null,null)");
      statement.execute("INSERT INTO test values (10,10,10,10,10,10)");

      Set<Row> allRows = new HashSet<>();
      allRows.add(new Row(1, 1, 1, 1, 1, 1));
      allRows.add(new Row(2, 2, null, 2, 2, 2));
      allRows.add(new Row(3, 3, 3, null, 3, 3));
      allRows.add(new Row(4, 4, 4, 4, null, 4));
      allRows.add(new Row(5, 5, 5, 5, 5, null));
      allRows.add(new Row(6, 6, null, null, 6, 6));
      allRows.add(new Row(7, 7, 7, 7, null, null));
      allRows.add(new Row(8, 8, null, 8, 8, null));
      allRows.add(new Row(9, 9, null, null, null, null));
      allRows.add(new Row(10, 10, 10, 10, 10, 10));

      // Test null conditions on both hash columns.
      String query = "SELECT * FROM test WHERE vh1 IS NULL AND vh2 IS NULL";
      Set<Row> expectedRows = allRows.stream()
                                     .filter(r -> r.get(2) == null && r.get(3) == null)
                                     .collect(Collectors.toSet());
      assertRowSet(statement, query, expectedRows);

      String explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL" + explainOutput,
                 explainOutput.contains("Index Cond: ((vh1 IS NULL) AND (vh2 IS NULL))"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // Test null conditions on all hash+range columns.
      query = "SELECT * FROM test WHERE vh1 IS NULL AND vh2 IS NULL" +
              " AND vr1 IS NULL and vr2 IS NULL";
      expectedRows = allRows.stream()
                                       .filter(r -> r.get(2) == null && r.get(3) == null &&
                                               r.get(4) == null && r.get(5) == null)
                                       .collect(Collectors.toSet());
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL" + explainOutput,
                 explainOutput.contains("Index Cond: ((vh1 IS NULL) AND (vh2 IS NULL)" +
                                                " AND (vr1 IS NULL) AND (vr2 IS NULL))"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // Test null/value condition mix columns.
      query = "SELECT * FROM test WHERE vh1 IS NULL AND vh2 = 8" +
              " AND vr1 = 8 and vr2 IS NULL";
      expectedRows = allRows.stream()
                              .filter(r -> r.get(2) == null && Objects.equals(r.get(3), 8) &&
                                      Objects.equals(r.get(3), 8) && r.get(5) == null)
                              .collect(Collectors.toSet());
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL" + explainOutput,
                 explainOutput.contains("Index Cond: ((vh1 IS NULL) AND (vh2 = 8)" +
                                                " AND (vr1 = 8) AND (vr2 IS NULL))"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));

      // Test partly set hash key (should not push down).
      query = "SELECT * FROM test WHERE vh1 IS NULL AND vr1 IS NULL and vr2 IS NULL";
      expectedRows = allRows.stream()
                              .filter(r -> r.get(2) == null &&
                                      r.get(4) == null && r.get(5) == null)
                              .collect(Collectors.toSet());
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL" + explainOutput,
                 explainOutput.contains("Filter: ((vh1 IS NULL) AND (vr1 IS NULL) " +
                                                "AND (vr2 IS NULL))"));
      assertTrue("Expect YSQL-layer filtering",
                  explainOutput.contains("Rows Removed by Filter: 9"));

      // Test hash key + partly set range key (should push down).
      query = "SELECT * FROM test WHERE vh1 IS NULL AND vh2 IS NULL" +
              " AND vr1 IS NULL";
      expectedRows = allRows.stream()
                              .filter(r -> r.get(2) == null && r.get(3) == null &&
                                      r.get(4) == null)
                              .collect(Collectors.toSet());
      assertRowSet(statement, query, expectedRows);

      explainOutput = getExplainAnalyzeOutput(statement, query);
      assertTrue("Expect pushdown for IS NULL" + explainOutput,
                 explainOutput.contains("Index Cond: ((vh1 IS NULL) AND (vh2 IS NULL)" +
                                                " AND (vr1 IS NULL))"));
      assertFalse("Expect DocDB to filter fully",
                  explainOutput.contains("Rows Removed by"));
    }
  }

}
