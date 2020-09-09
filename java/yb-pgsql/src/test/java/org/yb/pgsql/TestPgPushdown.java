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

import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.function.Function;

import org.apache.commons.lang3.StringUtils;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.yb.util.YBTestRunnerNonTsanOnly;

/**
 * Test pushdown behaviour of different expression from PG to YB layer.
 */
@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgPushdown extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgPushdown.class);

  @Override
  protected Integer getYsqlRequestLimit() {
    // This should be less than number of operations in some tests
    return 7;
  }

  @Test
  public void inequality_oneRangeColumn() throws Exception {
    String tableName = "inequality_predicate_pushdown";

    // Schema:
    //
    // Table "public.inequality_predicate_pushdown"
    //  Column |  Type   | Collation | Nullable | Default
    //  -------+---------+-----------+----------+---------
    //  h      | integer |           | not null |
    //  ri     | integer |           | not null |
    //  vs     | text    |           |          |
    //  Indexes:
    //  "inequality_predicate_pushdown_pkey" PRIMARY KEY, lsm (h HASH, ri)

    // Create table.
    try (Statement stmt = connection.createStatement()) {
      String sql = String.format("CREATE TABLE %s(h int, ri int, vs text, " +
          "PRIMARY KEY (h, ri ASC))", tableName);
      LOG.info("Execute: " + sql);
      stmt.execute(sql);
      LOG.info("Created: " + tableName);

      // Insert rows.
      int hMax = 2, riMax = 5000;
      for (int h = 0; h < hMax; h++) {
        for (int ri = 0; ri < riMax; ri++) {
          String vsTmp = String.format("value_%d_%d", h, ri);

          stmt.execute(String.format("INSERT INTO %s VALUES (%d, %d, '%s')",
              tableName, h, ri, vsTmp));
        }
      }

      // Choose the maximum runtimes of the SELECT statements.

      int fullScanReleaseRuntime = 400;
      int fullScanDebugRuntime = 500;
      int fullScanDebugRuntimeWithMargin = 10 * fullScanDebugRuntime;
      int fullScanMaxRuntimeMillis = getPerfMaxRuntime(fullScanReleaseRuntime,
          fullScanDebugRuntime, fullScanDebugRuntimeWithMargin, fullScanDebugRuntimeWithMargin,
          fullScanDebugRuntimeWithMargin);

      int intervalScanReleaseRuntime = 200;
      int intervalScanDebugRuntime = 250;
      int intervalScanDebugRuntimeWithMargin = 10 * intervalScanDebugRuntime;
      int intervalScanMaxRuntimeMillis = getPerfMaxRuntime(intervalScanReleaseRuntime,
          intervalScanDebugRuntime, intervalScanDebugRuntimeWithMargin,
          intervalScanDebugRuntimeWithMargin, intervalScanDebugRuntimeWithMargin);

      // Full scan.
      final int queryRunCount = 5;
      int h = hMax / 2;
      String query = String.format("SELECT * FROM %s WHERE h = %d", tableName, h);
      assertQueryRuntimeWithRowCount(stmt, query, riMax /* expectedRowCount */, queryRunCount,
          fullScanMaxRuntimeMillis * queryRunCount);

      // Interval scans that should return 100 rows each.

      int riDelta = riMax / 50;
      assertEquals(riDelta, 100);

      int A, B;

      // A < ri.
      A = riMax - riDelta - 1;
      query = String.format("SELECT * FROM %s WHERE h = %d AND ri > %d", tableName, h, A);
      assertQueryRuntimeWithRowCount(stmt, query, riDelta /* expectedRowCount */,
          queryRunCount, intervalScanMaxRuntimeMillis * queryRunCount);

      // ri < B.
      B = riDelta;
      query = String.format("SELECT * FROM %s WHERE h = %d AND ri < %d", tableName, h, B);
      assertQueryRuntimeWithRowCount(stmt, query, riDelta /* expectedRowCount */,
          queryRunCount, intervalScanMaxRuntimeMillis * queryRunCount);

      // A < ri < B.
      A = riMax / 2;
      B = A + riDelta + 1;
      query = String.format("SELECT * FROM %s WHERE h = %d AND ri > %d AND ri < %d", tableName, h,
          A, B);
      assertQueryRuntimeWithRowCount(stmt, query, riDelta /* expectedRowCount */,
          queryRunCount, intervalScanMaxRuntimeMillis * queryRunCount);
    }
  }

  @Test
  public void inequalityAndIn_twoRangeColumns() throws Exception {
    String tableName = "inequality_predicate_pushdown";

    // Schema:
    //
    // Table "public.inequality_predicate_pushdown"
    //  Column |  Type   | Collation | Nullable | Default
    //  -------+---------+-----------+----------+---------
    //  h      | integer |           | not null |
    //  ri     | integer |           | not null |
    //  rs     | text    |           | not null |
    //  vs     | text    |           |          |
    //  Indexes:
    //  "inequality_predicate_pushdown_pkey" PRIMARY KEY, lsm (h HASH, ri, rs DESC)

    // Create table.
    try (Statement statement = connection.createStatement()) {
      String sql = String.format("CREATE TABLE %s(h int, ri int, rs text, vs text, " +
          "PRIMARY KEY (h, ri ASC, rs DESC))", tableName);
      LOG.info("Execute: " + sql);
      statement.execute(sql);
      LOG.info("Created: " + tableName);

      // Numeric and lexicographic order of [rsBase, rsBase + rsMax) should be the same.
      int rsBase = 100;

      // Insert rows.
      int hMax = 2, riMax = 50, rsMax = 100;
      for (int h = 0; h < hMax; h++) {
        for (int ri = 0; ri < riMax; ri++) {
          for (int rs = 0; rs < rsMax; rs++) {
            String rsTmp = String.format("range_%d", rsBase + rs);
            String vsTmp = String.format("value_%d_%d_%s", h, ri, rsTmp);

            String stmt = String.format("INSERT INTO %s VALUES (%d, %d, '%s', '%s')",
                tableName, h, ri, rsTmp, vsTmp);
            statement.execute(stmt);
          }
        }
      }

      // Choose the maximum runtimes of the SELECT statements.

      int fullScanReleaseRuntime = 500;
      int fullScanDebugRuntime = 600;
      int fullScanDebugRuntimeWithMargin = 10 * fullScanDebugRuntime;
      int fullScanMaxRuntimeMillis = getPerfMaxRuntime(fullScanReleaseRuntime,
          fullScanDebugRuntime, fullScanDebugRuntimeWithMargin, fullScanDebugRuntimeWithMargin,
          fullScanDebugRuntimeWithMargin);

      int intervalScanReleaseRuntime = 350;
      int intervalScanDebugRuntime = 400;
      int intervalScanDebugRuntimeWithMargin = 10 * intervalScanDebugRuntime;
      int intervalScanMaxRuntimeMillis = getPerfMaxRuntime(intervalScanReleaseRuntime,
          intervalScanDebugRuntime, intervalScanDebugRuntimeWithMargin,
          intervalScanDebugRuntimeWithMargin, intervalScanDebugRuntimeWithMargin);

      // Full scan.

      final int queryRunCount = 5;
      int h = hMax / 2;

      String query = String.format("SELECT * FROM %s WHERE h = %d", tableName, h);
      assertQueryRuntimeWithRowCount(statement, query, riMax * rsMax /* expectedRowCount */,
          queryRunCount, fullScanMaxRuntimeMillis * queryRunCount);

      // Interval scans that should return 100 rows each.

      int riDelta, rsDelta, A, B, C, D, expectedRowCount;
      String CTmp, DTmp;

      // Inequality only ---------------------------------------------------------------------------

      // A < ri < B.
      riDelta = 1;
      A = riMax / 2;
      B = A + riDelta + 1;
      query = String.format("SELECT * FROM %s WHERE h = %d AND ri > %d AND ri < %d", tableName, h,
          A, B);
      expectedRowCount = riDelta * rsMax;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // C < rs < D.
      rsDelta = 2;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      query = String.format("SELECT * FROM %s WHERE h = %d AND rs > '%s' AND rs < '%s'", tableName,
          h, CTmp, DTmp);
      expectedRowCount = riMax * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // A < ri < B    AND    C < rs < D.
      riDelta = 10;
      rsDelta = 10;
      A = riMax / 2;
      B = A + riDelta + 1;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      query = String.format(
          "SELECT * FROM %s WHERE h = %d AND ri > %d AND ri < %d AND rs > '%s' AND rs < '%s'",
          tableName, h, A, B, CTmp, DTmp);
      expectedRowCount = riDelta * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // In only -----------------------------------------------------------------------------------

      // IN scans that should return 100 rows each.

      String AtoB, CtoD;

      // ri IN {A ... B}.
      riDelta = 1;
      A = riMax / 2;
      B = A + riDelta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      query = String.format("SELECT * FROM %s WHERE h = %d AND ri IN (%s)", tableName, h, AtoB);
      expectedRowCount = riDelta * rsMax;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // rs IN {C ... D}.
      rsDelta = 2;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format("SELECT * FROM %s WHERE h = %d AND rs IN (%s)", tableName, h, CtoD);
      expectedRowCount = riMax * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // ri IN {A ... B}    AND    rs IN {C ... D}.
      riDelta = 10;
      rsDelta = 10;
      A = riMax / 2;
      B = A + riDelta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format(
          "SELECT * FROM %s WHERE h = %d AND ri IN (%s) AND rs IN (%s)", tableName, h, AtoB, CtoD);
      expectedRowCount = riDelta * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // Inequality AND IN on different columns ----------------------------------------------------

      // Scans that should return 100 rows each.

      // A < ri < B    AND    rs IN {C ... D}.
      riDelta = 10;
      rsDelta = 10;
      A = riMax / 2;
      B = A + riDelta + 1;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format(
          "SELECT * FROM %s WHERE h = %d AND ri > %d AND ri < %d AND rs IN (%s)", tableName, h, A,
          B, CtoD);
      expectedRowCount = riDelta * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // ri IN {A ... B}    AND    C < rs < D.
      riDelta = 10;
      rsDelta = 10;
      A = riMax / 2;
      B = A + riDelta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      query = String.format(
          "SELECT * FROM %s WHERE h = %d AND ri IN (%s) AND rs > '%s' AND rs < '%s'", tableName, h,
          AtoB, CTmp, DTmp);
      expectedRowCount = riDelta * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // Inequality AND IN on same column ----------------------------------------------------------

      // Scans that should return 100 rows each.

      // A < ri < B    AND    ri IN {A ... B}.
      riDelta = 1;
      A = riMax / 2;
      B = A + riDelta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      query = String.format("SELECT * FROM %s WHERE h = %d AND ri > %d AND ri < %d AND ri IN (%s)",
          tableName, h, A, B, AtoB);
      expectedRowCount = riDelta * rsMax;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // C < rs < D    AND    rs IN {C ... D}.
      rsDelta = 2;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format(
          "SELECT * FROM %s WHERE h = %d AND rs IN (%s) AND rs > '%s' AND rs < '%s'", tableName,
          h, CtoD, CTmp, DTmp);
      expectedRowCount = riMax * rsDelta;
      assertQueryRuntimeWithRowCount(statement, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);
    }
  }

  @Test
  public void inequality_twoRangeColumns() throws Exception {
    String tableName = "inequality_predicate_pushdown";

    // Schema:
    //
    // Table "public.inequality_predicate_pushdown"
    //  Column |  Type   | Collation | Nullable | Default
    //  -------+---------+-----------+----------+---------
    //  r1     | integer |           | not null |
    //  r2     | integer |           | not null |
    //  vs     | text    |           |          |
    //  Indexes:
    //  "inequality_predicate_pushdown_pkey" PRIMARY KEY, lsm (r1, r2)

    // Create table.
    try (Statement stmt = connection.createStatement()) {
      String sql = String.format("CREATE TABLE %s(r1 int, r2 int, vs text, " +
          "PRIMARY KEY (r1 ASC, r2 ASC))", tableName);
      LOG.info("Execute: " + sql);
      stmt.execute(sql);
      LOG.info("Created: " + tableName);

      // Insert rows.
      int r1Max = 2, r2Max = 5000;
      for (int r1 = 0; r1 < r1Max; r1++) {
        for (int r2 = 0; r2 < r2Max; r2++) {
          String vsTmp = String.format("value_%d_%d", r1, r2);

          stmt.execute(String.format("INSERT INTO %s VALUES (%d, %d, '%s')",
              tableName, r1, r2, vsTmp));
        }
      }

      // Choose the maximum runtimes of the SELECT statements.

      int fullScanReleaseRuntime = 400;
      int fullScanDebugRuntime = 500;
      int fullScanDebugRuntimeWithMargin = 10 * fullScanDebugRuntime;
      int fullScanMaxRuntimeMillis = getPerfMaxRuntime(fullScanReleaseRuntime,
          fullScanDebugRuntime, fullScanDebugRuntimeWithMargin, fullScanDebugRuntimeWithMargin,
          fullScanDebugRuntimeWithMargin);

      int intervalScanReleaseRuntime = 200;
      int intervalScanDebugRuntime = 250;
      int intervalScanDebugRuntimeWithMargin = 10 * intervalScanDebugRuntime;
      int intervalScanMaxRuntimeMillis = getPerfMaxRuntime(intervalScanReleaseRuntime,
          intervalScanDebugRuntime, intervalScanDebugRuntimeWithMargin,
          intervalScanDebugRuntimeWithMargin, intervalScanDebugRuntimeWithMargin);

      // Scan range column.

      final int queryRunCount = 5;
      int r1 = r1Max / 2;
      String query = String.format("SELECT * FROM %s WHERE r1 = %d", tableName, r1);
      assertQueryRuntimeWithRowCount(stmt, query, r2Max /* expectedRowCount */, queryRunCount,
          fullScanMaxRuntimeMillis * queryRunCount);

      // Interval scans that should return 100 rows each.

      int r2Delta = r2Max / 50;
      assertEquals(r2Delta, 100);

      int A, B;

      // A < r2.
      A = r2Max - r2Delta - 1;
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND r2 > %d", tableName, r1, A);
      assertQueryRuntimeWithRowCount(stmt, query, r2Delta /* expectedRowCount */, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);

      // r2 < B.
      B = r2Delta;
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND r2 < %d", tableName, r1, B);
      assertQueryRuntimeWithRowCount(stmt, query, r2Delta /* expectedRowCount */, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);

      // A < r2 < B.
      A = r2Max / 2;
      B = A + r2Delta + 1;
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND r2 > %d AND r2 < %d", tableName, r1,
          A, B);
      assertQueryRuntimeWithRowCount(stmt, query, r2Delta /* expectedRowCount */, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
    }
  }

  @Test
  public void inequalityAndIn_allRangeColumns() throws Exception {
    String tableName = "inequality_predicate_pushdown";

    // Schema:
    //
    // Table "public.inequality_predicate_pushdown"
    //  Column |  Type   | Collation | Nullable | Default
    //  -------+---------+-----------+----------+---------
    //  r1     | integer |           | not null |
    //  r2     | integer |           | not null |
    //  rs     | text    |           | not null |
    //  vs     | text    |           |          |
    //  Indexes:
    //  "inequality_predicate_pushdown_pkey" PRIMARY KEY, lsm (r1, r2, rs DESC)

    // Create table.
    try (Statement stmt = connection.createStatement()) {
      String sql = String.format("CREATE TABLE %s(r1 int, r2 int, rs text, vs text, " +
          "PRIMARY KEY (r1, r2 ASC, rs DESC))", tableName);
      LOG.info("Execute: " + sql);
      stmt.execute(sql);
      LOG.info("Created: " + tableName);

      // Numeric and lexicographic order of [rsBase, rsBase + rsMax) should be the same.
      int rsBase = 100;

      // Insert rows.
      int r1Max = 2, r2Max = 50, rsMax = 100;
      for (int r1 = 0; r1 < r1Max; r1++) {
        for (int r2 = 0; r2 < r2Max; r2++) {
          for (int rs = 0; rs < rsMax; rs++) {
            String rsTmp = String.format("range_%d", rsBase + rs);
            String vsTmp = String.format("value_%d_%d_%s", r1, r2, rsTmp);

            stmt.execute(String.format("INSERT INTO %s VALUES (%d, %d, '%s', '%s')",
                tableName, r1, r2, rsTmp, vsTmp));
          }
        }
      }

      // Choose the maximum runtimes of the SELECT statements.

      int fullScanReleaseRuntime = 500;
      int fullScanDebugRuntime = 600;
      int fullScanDebugRuntimeWithMargin = 10 * fullScanDebugRuntime;
      int fullScanMaxRuntimeMillis = getPerfMaxRuntime(fullScanReleaseRuntime,
          fullScanDebugRuntime, fullScanDebugRuntimeWithMargin, fullScanDebugRuntimeWithMargin,
          fullScanDebugRuntimeWithMargin);

      int intervalScanReleaseRuntime = 350;
      int intervalScanDebugRuntime = 400;
      int intervalScanDebugRuntimeWithMargin = 10 * intervalScanDebugRuntime;
      int intervalScanMaxRuntimeMillis = getPerfMaxRuntime(intervalScanReleaseRuntime,
          intervalScanDebugRuntime, intervalScanDebugRuntimeWithMargin,
          intervalScanDebugRuntimeWithMargin, intervalScanDebugRuntimeWithMargin);

      // Scan range column.

      final int queryRunCount = 5;
      int r1 = r1Max / 2;

      String query = String.format("SELECT * FROM %s WHERE r1 = %d", tableName, r1);
      assertQueryRuntimeWithRowCount(stmt, query, r2Max * rsMax /* expectedRowCount */,
          queryRunCount,
          fullScanMaxRuntimeMillis * queryRunCount);

      // Interval scans that should return 100 rows each.

      int r2Delta, rsDelta, A, B, C, D, expectedRowCount;
      String CTmp, DTmp;

      // Inequality only ---------------------------------------------------------------------------

      // A < r2 < B.
      r2Delta = 1;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND r2 > %d AND r2 < %d", tableName, r1,
          A, B);
      expectedRowCount = r2Delta * rsMax;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // C < rs < D.
      rsDelta = 2;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND rs > '%s' AND rs < '%s'", tableName,
          r1, CTmp, DTmp);
      expectedRowCount = r2Max * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // A < ri < B    AND    C < rs < D.
      r2Delta = 10;
      rsDelta = 10;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      query = String.format(
          "SELECT * FROM %s WHERE r1 = %d AND r2 > %d AND r2 < %d AND rs > '%s' AND rs < '%s'",
          tableName, r1, A, B, CTmp, DTmp);
      expectedRowCount = r2Delta * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // In only -----------------------------------------------------------------------------------

      // IN scans that should return 100 rows each.

      String AtoB, CtoD;

      // r2 IN {A ... B}.
      r2Delta = 1;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND r2 IN (%s)", tableName, r1, AtoB);
      expectedRowCount = r2Delta * rsMax;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // rs IN {C ... D}.
      rsDelta = 2;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND rs IN (%s)", tableName, r1, CtoD);
      expectedRowCount = r2Max * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // r2 IN {A ... B}    AND    rs IN {C ... D}.
      r2Delta = 10;
      rsDelta = 10;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format(
          "SELECT * FROM %s WHERE r1 = %d AND r2 IN (%s) AND rs IN (%s)", tableName, r1, AtoB,
          CtoD);
      expectedRowCount = r2Delta * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // Inequality AND IN on different columns ----------------------------------------------------

      // Scans that should return 100 rows each.

      // A < r2 < B    AND    rs IN {C ... D}.
      r2Delta = 10;
      rsDelta = 10;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format(
          "SELECT * FROM %s WHERE r1 = %d AND r2 > %d AND r2 < %d AND rs IN (%s)", tableName, r1, A,
          B, CtoD);
      expectedRowCount = r2Delta * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // r2 IN {A ... B}    AND    C < rs < D.
      r2Delta = 10;
      rsDelta = 10;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      query = String.format(
          "SELECT * FROM %s WHERE r1 = %d AND r2 IN (%s) AND rs > '%s' AND rs < '%s'", tableName,
          r1,
          AtoB, CTmp, DTmp);
      expectedRowCount = r2Delta * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // Inequality AND IN on same column ----------------------------------------------------------

      // Scans that should return 100 rows each.

      // A < r2 < B    AND    r2 IN {A ... B}.
      r2Delta = 1;
      A = r2Max / 2;
      B = A + r2Delta + 1;
      AtoB = String.format("%d", A + 1);
      for (int i = A + 2; i < B; i++) {
        AtoB += String.format(",%d", i);
      }
      query = String.format("SELECT * FROM %s WHERE r1 = %d AND r2 > %d AND r2 < %d AND r2 IN (%s)",
          tableName, r1, A, B, AtoB);
      expectedRowCount = r2Delta * rsMax;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);

      // C < rs < D    AND    rs IN {C ... D}.
      rsDelta = 2;
      C = rsMax / 2;
      D = C + rsDelta + 1;
      CTmp = String.format("range_%d", rsBase + C);
      DTmp = String.format("range_%d", rsBase + D);
      CtoD = String.format("'range_%d'", rsBase + C + 1);
      for (int i = C + 2; i < D; i++) {
        CtoD += String.format(",'range_%d'", rsBase + i);
      }
      query = String.format(
          "SELECT * FROM %s WHERE r1 = %d AND rs IN (%s) AND rs > '%s' AND rs < '%s'", tableName,
          r1, CtoD, CTmp, DTmp);
      expectedRowCount = r2Max * rsDelta;
      assertQueryRuntimeWithRowCount(stmt, query, expectedRowCount, queryRunCount,
          intervalScanMaxRuntimeMillis * queryRunCount);
      assertEquals(expectedRowCount, 100);
    }
  }

  @Test
  public void in_primaryKeySimple() throws Exception {
    //        Table "public.in_pk_pushdown_simple"
    //  Column |  Type   | Collation | Nullable | Default
    // --------+---------+-----------+----------+---------
    //  h      | integer |           | not null |
    //  v      | integer |           |          |
    // Indexes:
    //     "in_pk_pushdown_simple_pkey" PRIMARY KEY, lsm (h HASH)

    String tableName = "in_pk_pushdown_simple";
    String tableSpec = "h int PRIMARY KEY, v int";

    int numRowsToInsert = 5000;

    InClausePushdownTester tester = new InClausePushdownTester(tableName, tableSpec) {
      @Override
      void fillTable(Statement stmt) throws Exception {
        StringBuilder sb = new StringBuilder("INSERT INTO " + tableName + " (h, v) VALUES ");
        for (int i = 0; i < numRowsToInsert; ++i) {
          sb.append(String.format("(%d, %d),", i, i));
        }
        stmt.executeUpdate(sb.substring(0, sb.length() - 1)); // Drop trailing comma
      }

      @Override
      List<Row> getExpectedRows() {
        return Arrays.asList(
            new Row(10, 10),
            new Row(20, 20),
            new Row(30, 30));
      }

      @Override
      String getOptimizedSelectQuery() {
        return String.format(
            "SELECT * FROM %s WHERE h IN (%d, %d, %d)",
            tableName, 10, 20, 30);
      }

      @Override
      String getOptimizedPreparedSelectQueryString() {
        return String.format(
            "SELECT * FROM %s WHERE h IN (?, ?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareSelectOrDeleteQuery(String queryString) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, 10);
        pquery.setInt(2, 20);
        pquery.setInt(3, 30);
        return pquery;
      }

      @Override
      String getOptimizedUpdateQuery(int valueToSet) {
        return String.format(
            "UPDATE %s SET v = %d WHERE h IN (%d, %d, %d)",
            tableName, valueToSet, 10, 20, 30);
      }

      @Override
      String getOptimizedPreparedUpdateQueryString() {
        return String.format(
            "UPDATE %s SET v = ? WHERE h IN (?, ?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareUpdateQuery(String queryString, int valueToSet) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, valueToSet);
        pquery.setInt(2, 10);
        pquery.setInt(3, 20);
        pquery.setInt(4, 30);
        return pquery;
      }

      @Override
      String getResetQuery() {
        return String.format("UPDATE %s SET v = h", tableName);
      }

      @Override
      String getOptimizedDeleteQuery() {
        return String.format(
            "DELETE FROM %s WHERE h IN (%d, %d, %d)",
            tableName, 10, 20, 30);
      }

      @Override
      String getOptimizedPreparedDeleteQueryString() {
        return String.format(
            "DELETE FROM %s WHERE h IN (?, ?, ?)",
            tableName);
      }
    };
    tester.test();
  }

  @Test
  public void in_primaryKeyTwoHashOneRange() throws Exception {
    //   Table "public.in_pk_pushdown_hash1_hash2_range"
    //  Column |  Type   | Collation | Nullable | Default
    // --------+---------+-----------+----------+---------
    //  h1     | integer |           | not null |
    //  h2     | integer |           | not null |
    //  r      | integer |           | not null |
    //  v      | integer |           |          |
    // Indexes:
    //     "in_pk_pushdown_hash1_hash2_range_pkey" PRIMARY KEY, lsm ((h1, h2) HASH, r)

    String tableName = "in_pk_pushdown_hash1_hash2_range";
    String tableSpec = "h1 int, h2 int, r int, v int, PRIMARY KEY ((h1, h2) HASH, r ASC)";

    int maxIntToInsert = 20;

    InClausePushdownTester tester = new InClausePushdownTester(tableName, tableSpec) {
      @Override
      void fillTable(Statement stmt) throws Exception {
        // Set the value to be the concatenation of row keys
        StringBuilder sb = new StringBuilder(
            "INSERT INTO " + tableName + " (h1, h2, r, v) VALUES ");
        for (int h1 = 0; h1 < maxIntToInsert; ++h1) {
          for (int h2 = 0; h2 < maxIntToInsert; ++h2) {
            for (int r = 0; r < maxIntToInsert; ++r) {
              sb.append(String.format("(%d, %d, %d, %d),",
                  h1, h2, r,
                  (h1 * 10000 + h2 * 100 + r)));
            }
          }
        }
        stmt.executeUpdate(sb.substring(0, sb.length() - 1)); // Drop trailing comma
      }

      @Override
      List<Row> getExpectedRows() {
        // h1 in [1, 2], h2 in [3, 4], r in [5, 6]
        // v is (kind of) the concatenation of [h1, h2, r]
        List<Row> expectedResult = new ArrayList<>();
        for (int h1 : Arrays.asList(1, 2)) {
          for (int h2 : Arrays.asList(3, 4)) {
            for (int r : Arrays.asList(5, 6)) {
              expectedResult.add(new Row(h1, h2, r, (h1 * 10000 + h2 * 100 + r)));
            }
          }
        }
        return expectedResult;
      }

      @Override
      String getOptimizedSelectQuery() {
        return String.format(
            "SELECT * FROM %s WHERE"
                + " h1 IN (%d, %d) AND"
                + " h2 IN (%d, %d) AND"
                + " r  IN (%d, %d)",
            tableName, 1, 2, 3, 4, 5, 6);
      }

      @Override
      String getOptimizedPreparedSelectQueryString() {
        return String.format(
            "SELECT * FROM %s WHERE"
                + " h1 IN (?, ?) AND"
                + " h2 IN (?, ?) AND"
                + " r  IN (?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareSelectOrDeleteQuery(String queryString) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        for (int i = 0; i < 3; ++i) {
          pquery.setInt(i * 2 + 1, i * 2 + 1);
          pquery.setInt(i * 2 + 2, i * 2 + 2);
        }
        return pquery;
      }

      @Override
      String getOptimizedUpdateQuery(int valueToSet) {
        return String.format("UPDATE %s SET v = %d WHERE"
            + " h1 IN (%d, %d) AND"
            + " h2 IN (%d, %d) AND"
            + " r  IN (%d, %d)",
            tableName, valueToSet, 1, 2, 3, 4, 5, 6);
      }

      @Override
      String getOptimizedPreparedUpdateQueryString() {
        return String.format("UPDATE %s SET v = ? WHERE"
            + " h1 IN (?, ?) AND"
            + " h2 IN (?, ?) AND"
            + " r  IN (?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareUpdateQuery(String queryString, int valueToSet) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, valueToSet);
        for (int i = 0; i < 3; ++i) {
          pquery.setInt(i * 2 + 2, i * 2 + 1);
          pquery.setInt(i * 2 + 3, i * 2 + 2);
        }
        return pquery;
      }

      @Override
      String getResetQuery() {
        return String.format("UPDATE %s SET v = h1 * 10000 + h2 * 100 + r", tableName);
      }

      @Override
      String getOptimizedDeleteQuery() {
        return String.format(
            "DELETE FROM %s WHERE"
                + " h1 IN (%d, %d) AND"
                + " h2 IN (%d, %d) AND"
                + " r  IN (%d, %d)",
            tableName, 1, 2, 3, 4, 5, 6);
      }

      @Override
      String getOptimizedPreparedDeleteQueryString() {
        return String.format(
            "DELETE FROM %s WHERE"
                + " h1 IN (?, ?) AND"
                + " h2 IN (?, ?) AND"
                + " r  IN (?, ?)",
            tableName);
      }
    };
    tester.test();
  }

  /**
   * Making sure we don't care about column declaration order nor PK order, thus not running into
   * something like https://github.com/yugabyte/yugabyte-db/issues/3302. For this we're using
   * mismatching key orders everywhere:
   * <ol>
   * <li>(h1, h2, h3) - in table schema
   * <li>(h3, h1, h3) - in PK declaration
   * <li>(h2, h3, h1) - in queries
   * </ol>
   */
  @Test
  public void in_primaryKeyReordered() throws Exception {
    //       Table "public.in_pk_pushdown_reordered"
    //  Column |  Type   | Collation | Nullable | Default
    // --------+---------+-----------+----------+---------
    //  h1     | integer |           | not null |
    //  h2     | integer |           | not null |
    //  h3     | integer |           | not null |
    //  v      | integer |           |          |
    // Indexes:
    //     "in_pk_pushdown_reordered_pkey" PRIMARY KEY, lsm ((h3, h1, h2) HASH)

    String tableName = "in_pk_pushdown_reordered";
    String tableSpec = "h1 int, h2 int, h3 int, v int, PRIMARY KEY ((h3, h1, h2) HASH)";

    int maxIntToInsert = 20; // 8000 rows

    InClausePushdownTester tester = new InClausePushdownTester(tableName, tableSpec) {
      @Override
      void fillTable(Statement stmt) throws Exception {
        // Set the value to be the concatenation of row keys
        StringBuilder sb = new StringBuilder(
            "INSERT INTO " + tableName + " (h2, h3, h1, v) VALUES ");
        for (int h2 = 0; h2 < maxIntToInsert; ++h2) {
          for (int h3 = 0; h3 < maxIntToInsert; ++h3) {
            for (int h1 = 0; h1 < maxIntToInsert; ++h1) {
              sb.append(String.format("(%d, %d, %d, %d),",
                  h2, h3, h1,
                  (h1 * 10000 + h2 * 100 + h3)));
            }
          }
        }
        stmt.executeUpdate(sb.substring(0, sb.length() - 1)); // Drop trailing comma
      }

      @Override
      List<Row> getExpectedRows() {
        // h1 in [1, 2], h2 in [3, 4], h3 in [5, 6]
        // v is (kind of) the concatenation of [h1, h2, h3]
        List<Row> expectedResult = new ArrayList<>();
        for (int h1 : Arrays.asList(1, 2)) {
          for (int h2 : Arrays.asList(3, 4)) {
            for (int h3 : Arrays.asList(5, 6)) {
              expectedResult.add(new Row(h1, h2, h3, (h1 * 10000 + h2 * 100 + h3)));
            }
          }
        }
        return expectedResult;
      }

      @Override
      String getOptimizedSelectQuery() {
        return String.format(
            "SELECT * FROM %s WHERE"
                + " h2 IN (%d, %d) AND"
                + " h3 IN (%d, %d) AND"
                + " h1 IN (%d, %d)",
            tableName, 3, 4, 5, 6, 1, 2);
      }

      @Override
      String getOptimizedPreparedSelectQueryString() {
        return String.format(
            "SELECT * FROM %s WHERE"
                + " h2 IN (?, ?) AND"
                + " h3 IN (?, ?) AND"
                + " h1 IN (?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareSelectOrDeleteQuery(String queryString) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, 3);
        pquery.setInt(2, 4);
        pquery.setInt(3, 5);
        pquery.setInt(4, 6);
        pquery.setInt(5, 1);
        pquery.setInt(6, 2);
        return pquery;
      }

      @Override
      String getOptimizedUpdateQuery(int valueToSet) {
        return String.format("UPDATE %s SET v = %d WHERE"
            + " h2 IN (%d, %d) AND"
            + " h3 IN (%d, %d) AND"
            + " h1 IN (%d, %d)",
            tableName, valueToSet, 3, 4, 5, 6, 1, 2);
      }

      @Override
      String getOptimizedPreparedUpdateQueryString() {
        return String.format("UPDATE %s SET v = ? WHERE"
            + " h2 IN (?, ?) AND"
            + " h3 IN (?, ?) AND"
            + " h1 IN (?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareUpdateQuery(String queryString, int valueToSet) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, valueToSet);
        pquery.setInt(2, 3);
        pquery.setInt(3, 4);
        pquery.setInt(4, 5);
        pquery.setInt(5, 6);
        pquery.setInt(6, 1);
        pquery.setInt(7, 2);
        return pquery;
      }

      @Override
      String getResetQuery() {
        return String.format("UPDATE %s SET v = h1 * 10000 + h2 * 100 + h3", tableName);
      }

      @Override
      String getOptimizedDeleteQuery() {
        return String.format(
            "DELETE FROM %s WHERE"
                + " h2 IN (%d, %d) AND"
                + " h3 IN (%d, %d) AND"
                + " h1 IN (%d, %d)",
            tableName, 3, 4, 5, 6, 1, 2);
      }

      @Override
      String getOptimizedPreparedDeleteQueryString() {
        return String.format(
            "DELETE FROM %s WHERE"
                + " h2 IN (?, ?) AND"
                + " h3 IN (?, ?) AND"
                + " h1 IN (?, ?)",
            tableName);
      }
    };
    tester.test();
  }

  @Test
  public void in_secondaryIndex() throws Exception {
    //       Table "public.in_sidx_pushdown_simple"
    //  Column |  Type   | Collation | Nullable | Default
    // --------+---------+-----------+----------+---------
    //  h      | integer |           | not null |
    //  i      | integer |           |          |
    //  v      | integer |           |          |
    // Indexes:
    //     "in_sidx_pushdown_simple_pkey" PRIMARY KEY, lsm (h HASH)
    //     "in_sidx_pushdown_simple_i_idx" lsm (i HASH)

    String tableName = "in_sidx_pushdown_simple";
    String tableSpec = "h int PRIMARY KEY, i int, v int";

    int numRowsToInsert = 5000;

    InClausePushdownTester tester = new InClausePushdownTester(tableName, tableSpec) {
      @Override
      void createTable(Statement stmt) throws Exception {
        super.createTable(stmt);
        stmt.executeUpdate(String.format("CREATE INDEX %s_i_idx ON %s (i)", tableName, tableName));
      }

      @Override
      void fillTable(Statement stmt) throws Exception {
        StringBuilder sb = new StringBuilder("INSERT INTO " + tableName + " (h, i, v) VALUES ");
        for (int i = 0; i < numRowsToInsert; ++i) {
          sb.append(String.format("(%d, %d, %d),", i, i, i));
        }
        stmt.executeUpdate(sb.substring(0, sb.length() - 1)); // Drop trailing comma
      }

      @Override
      List<Row> getExpectedRows() {
        return Arrays.asList(
            new Row(10, 10, 10),
            new Row(20, 20, 20),
            new Row(30, 30, 30));
      }

      @Override
      String getOptimizedSelectQuery() {
        return String.format(
            "SELECT * FROM %s WHERE i IN (%d, %d, %d)",
            tableName, 10, 20, 30);
      }

      @Override
      String getOptimizedPreparedSelectQueryString() {
        return String.format(
            "SELECT * FROM %s WHERE i IN (?, ?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareSelectOrDeleteQuery(String queryString) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, 10);
        pquery.setInt(2, 20);
        pquery.setInt(3, 30);
        return pquery;
      }

      @Override
      String getOptimizedUpdateQuery(int valueToSet) {
        return String.format(
            "UPDATE %s SET v = %d WHERE i IN (%d, %d, %d)",
            tableName, valueToSet, 10, 20, 30);
      }

      @Override
      String getOptimizedPreparedUpdateQueryString() {
        return String.format(
            "UPDATE %s SET v = ? WHERE i IN (?, ?, ?)",
            tableName);
      }

      @Override
      PreparedStatement prepareUpdateQuery(String queryString, int valueToSet) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, valueToSet);
        pquery.setInt(2, 10);
        pquery.setInt(3, 20);
        pquery.setInt(4, 30);
        return pquery;
      }

      @Override
      String getResetQuery() {
        return String.format("UPDATE %s SET v = h", tableName);
      }

      @Override
      String getOptimizedDeleteQuery() {
        return String.format(
            "DELETE FROM %s WHERE i IN (%d, %d, %d)",
            tableName, 10, 20, 30);
      }

      @Override
      String getOptimizedPreparedDeleteQueryString() {
        return String.format(
            "DELETE FROM %s WHERE i IN (?, ?, ?)",
            tableName);
      }
    };
    tester.test();
  }

  /** Mixing IN and equality: h1 IN (1, 2, 3) AND h2 = 4 */
  @Test
  public void inEq_primaryKeyTwoHash() throws Exception {
    //    Table "public.in_eq_pk_pushdown_hash1_hash2"
    //  Column |  Type   | Collation | Nullable | Default
    // --------+---------+-----------+----------+---------
    //  h1     | integer |           | not null |
    //  h2     | integer |           | not null |
    //  v      | integer |           |          |
    // Indexes:
    //     "in_eq_pk_pushdown_hash1_hash2_pkey" PRIMARY KEY, lsm ((h1, h2) HASH)

    String tableName = "in_eq_pk_pushdown_hash1_hash2";
    String tableSpec = "h1 int, h2 int, v int, PRIMARY KEY ((h1, h2) HASH)";

    int maxIntToInsert = 80; // 6400 rows

    InClausePushdownTester tester = new InClausePushdownTester(tableName, tableSpec) {
      @Override
      void fillTable(Statement stmt) throws Exception {
        // Set the value to be the concatenation of row keys
        StringBuilder sb = new StringBuilder(
            "INSERT INTO " + tableName + " (h1, h2, v) VALUES ");
        for (int h1 = 0; h1 < maxIntToInsert; ++h1) {
          for (int h2 = 0; h2 < maxIntToInsert; ++h2) {
            sb.append(String.format("(%d, %d, %d),",
                h1, h2,
                (h1 * 10000 + h2)));
          }
        }
        stmt.executeUpdate(sb.substring(0, sb.length() - 1)); // Drop trailing comma
      }

      @Override
      List<Row> getExpectedRows() {
        List<Row> expectedResult = new ArrayList<>();
        int h2 = 4;
        for (int h1 : Arrays.asList(1, 2, 3)) {
          expectedResult.add(new Row(h1, h2, (h1 * 10000 + h2)));
        }
        return expectedResult;
      }

      @Override
      String getOptimizedSelectQuery() {
        return String.format(
            "SELECT * FROM %s WHERE"
                + " h1 IN (%d, %d, %d) AND"
                + " h2 = %d",
            tableName, 1, 2, 3, 4);
      }

      @Override
      String getOptimizedPreparedSelectQueryString() {
        return String.format(
            "SELECT * FROM %s WHERE"
                + " h1 IN (?, ?, ?) AND"
                + " h2 = ?",
            tableName);
      }

      @Override
      PreparedStatement prepareSelectOrDeleteQuery(String queryString) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        for (int i = 1; i <= 4; ++i) {
          pquery.setInt(i, i);
        }
        return pquery;
      }

      @Override
      String getOptimizedUpdateQuery(int valueToSet) {
        return String.format("UPDATE %s SET v = %d WHERE"
            + " h1 IN (%d, %d, %d) AND"
            + " h2 = %d",
            tableName, valueToSet, 1, 2, 3, 4);
      }

      @Override
      String getOptimizedPreparedUpdateQueryString() {
        return String.format("UPDATE %s SET v = ? WHERE"
            + " h1 IN (?, ?, ?) AND"
            + " h2 = ?",
            tableName);
      }

      @Override
      PreparedStatement prepareUpdateQuery(String queryString, int valueToSet) throws Exception {
        PreparedStatement pquery = connection.prepareStatement(queryString);
        pquery.setInt(1, valueToSet);
        for (int i = 1; i <= 4; ++i) {
          pquery.setInt(i + 1, i);
        }
        return pquery;
      }

      @Override
      String getResetQuery() {
        return String.format("UPDATE %s SET v = h1 * 10000 + h2", tableName);
      }

      @Override
      String getOptimizedDeleteQuery() {
        return String.format(
            "DELETE FROM %s WHERE"
                + " h1 IN (%d, %d, %d) AND"
                + " h2 = %d",
            tableName, 1, 2, 3, 4);
      }

      @Override
      String getOptimizedPreparedDeleteQueryString() {
        return String.format(
            "DELETE FROM %s WHERE"
                + " h1 IN (?, ?, ?) AND"
                + " h2 = ?",
            tableName);
      }
    };
    tester.test();
  }

  /** Ensure pushing down aggregate functions with constant argument. */
  @Test
  public void aggregates_const() throws Exception {
    String tableName = "aggregate";
    int numRows = 5000;

    new AggregatePushdownTester(tableName, numRows, "COUNT(*)").test();
    new AggregatePushdownTester(tableName, numRows, "COUNT(0)").test();
    new AggregatePushdownTester(tableName, numRows, "COUNT(NULL)").test();

    new AggregatePushdownTester(tableName, numRows, "SUM(2)").test();
    new AggregatePushdownTester(tableName, numRows, "SUM(NULL::int)").test();

    // Postgres optimizes MAX(<const>) or MIN(<const>) so it isn't a real pushdown.
  }

  //
  // Helpers
  //

  /**
   * Tests execution plan and elapsed time of SELECT/UPDATE/DELETE ... WHERE ... IN (...) type of
   * statements by comparing it with the execution time of similar non-optimized queries.
   * <p>
   * Expects last column to be "v" of type INT.
   * <p>
   * Note that actual performance numbers are only verified for SELECT - performance of pushed down
   * UPDATE/DELETE is close enough to the original that it might on rare occasions take even LONGER
   * than non-optimized queries, leading to flaky tests. For them, we only check an execution plan.
   */
  private abstract class InClausePushdownTester {
    /** How many times would each query be iterated to get a total running time? */
    public final int queryRunCount = 20;

    /** Minimum speedup multiplier expected for pushed down SELECT-type queries */
    public final double minSelectSpeedup = 2;

    public final String tableName;

    /** Passed to CREATE TABLE in parentheses, list columns and primary key */
    public final String tableSpec;

    public InClausePushdownTester(String tableName, String tableSpec) {
      this.tableName = tableName;
      this.tableSpec = tableSpec;
    }

    void createTable(Statement stmt) throws Exception {
      stmt.executeUpdate(String.format("DROP TABLE IF EXISTS %s", tableName));
      stmt.executeUpdate(String.format("CREATE TABLE %s (%s)", tableName, tableSpec));
    }

    void dropTable(Statement stmt) throws Exception {
      stmt.executeUpdate(String.format("DROP TABLE %s", tableName));
    }

    /** Fill the table with test values */
    abstract void fillTable(Statement stmt) throws Exception;

    /** Expected rows subset operated by subsequent queries */
    abstract List<Row> getExpectedRows();

    //
    // SELECT
    //

    /** Optimized query that should yield expected rows */
    abstract String getOptimizedSelectQuery();

    /** Optimized query string that, once prepared, should yield expected rows */
    abstract String getOptimizedPreparedSelectQueryString();

    /**
     * Prepare a query returned by either {@link #getOptimizedPreparedSelectQueryString()} or
     * {@link #getOptimizedPreparedDeleteQueryString()} (since their parameters match)
     */
    abstract PreparedStatement prepareSelectOrDeleteQuery(String queryString) throws Exception;

    //
    // UPDATE
    //

    /** Optimized query updating column "v" in expected rows to the given value */
    abstract String getOptimizedUpdateQuery(int valueToSet);

    /** Optimized query string that, once prepared, should update column "v" in expected rows */
    abstract String getOptimizedPreparedUpdateQueryString();

    /**
     * Prepare a query returned by {@link #getOptimizedPreparedUpdateQueryString(int)} to set the
     * given value in expected rows
     */
    abstract PreparedStatement prepareUpdateQuery(String queryString, int valueToSet)
        throws Exception;

    /** Query that should reset column "v" to the initial state in the whole table */
    abstract String getResetQuery();

    //
    // DELETE
    //

    /** Optimized query deleting expected rows */
    abstract String getOptimizedDeleteQuery();

    /** Optimized query string that, once prepared, should delete expected rows */
    abstract String getOptimizedPreparedDeleteQueryString();

    //
    // Test methods
    //

    public void test() throws Exception {
      try (Statement stmt = connection.createStatement()) {
        createTable(stmt);
        fillTable(stmt);

        List<Row> expectedRows = getExpectedRows();
        testSelect(stmt, expectedRows);
        testUpdate(stmt, expectedRows);
        testDelete(stmt, expectedRows);

        dropTable(stmt);
      }
    }

    private void testSelect(Statement stmt, List<Row> expectedRows) throws Exception {
      int lastColIdx = expectedRows.get(0).elems.size() - 1;
      int numRows = expectedRows.size();

      long nonOptimizedTime;
      {
        // Full scan query used as an execution time reference
        String inClause = StringUtils
            .join(expectedRows.stream().map(r -> r.getInt(lastColIdx)).iterator(), ",");
        String nonOptimizedQuery = String.format(
            "SELECT * FROM %s WHERE v IN (%s)", tableName, inClause);
        assertPushdownPlan(stmt, nonOptimizedQuery, false);
        assertEquals(expectedRows, getSortedRowList(stmt.executeQuery(nonOptimizedQuery)));
        nonOptimizedTime = timeQueryWithRowCount(stmt, nonOptimizedQuery, numRows, queryRunCount);
        LOG.info("Non-optimized SELECT total time (ms): " + nonOptimizedTime);
      }

      long maxOptimizedTime = (long) (nonOptimizedTime / minSelectSpeedup);

      // Plain optimized query
      {
        String query = getOptimizedSelectQuery();
        assertPushdownPlan(stmt, query, true);
        assertEquals(expectedRows, getSortedRowList(stmt.executeQuery(query)));
        long optimizedTime = assertQueryRuntimeWithRowCount(stmt, query, numRows, queryRunCount,
            maxOptimizedTime);
        LOG.info("Optimized plain SELECT total time (ms): " + optimizedTime);
      }

      // Prepared optimized query
      {
        String queryString = getOptimizedPreparedSelectQueryString();
        assertPushdownPlan(stmt, queryString, (q) -> {
          try {
            return prepareSelectOrDeleteQuery(q);
          } catch (Exception ex) {
            throw new RuntimeException(ex); // Functional Java, yeah, right
          }
        }, true);
        try (PreparedStatement pquery = prepareSelectOrDeleteQuery(queryString)) {
          assertEquals(expectedRows, getSortedRowList(pquery.executeQuery()));
          long optimizedTime = assertQueryRuntimeWithRowCount(pquery, numRows, queryRunCount,
              maxOptimizedTime);
          LOG.info("Optimized prepared SELECT total time (ms): " + optimizedTime);
        }
      }
    }

    private void testUpdate(Statement stmt, List<Row> expectedRows) throws Exception {
      int lastColIdx = expectedRows.get(0).elems.size() - 1;
      int valueToSet = 123456789;

      List<Row> updatedResult = new ArrayList<>(expectedRows.size());
      for (Row row : expectedRows) {
        Row urow = row.clone();
        urow.elems.set(lastColIdx, valueToSet);
        updatedResult.add(urow);
      }

      String selectQuery1 = getOptimizedSelectQuery();
      String selectQuery2 = String.format("SELECT * FROM %s WHERE v = %d", tableName, valueToSet);

      long nonOptimizedTime;
      {
        // Full scan update query used as an execution time reference
        String inClause = StringUtils
            .join(expectedRows.stream().map(r -> r.getInt(lastColIdx)).iterator(), ",");
        String nonOptimizedQuery = String.format(
            "UPDATE %s SET v = %d WHERE v IN (%s)", tableName, valueToSet, inClause);
        assertPushdownPlan(stmt, nonOptimizedQuery, false);
        nonOptimizedTime = timeStatement(nonOptimizedQuery, queryRunCount);
        LOG.info("Non-optimized UPDATE total time (ms): " + nonOptimizedTime);
        assertEquals(updatedResult, getSortedRowList(stmt.executeQuery(selectQuery1)));
        assertEquals(updatedResult, getSortedRowList(stmt.executeQuery(selectQuery2)));
      }

      // Plain optimized query
      {
        stmt.executeUpdate(getResetQuery());
        String query = getOptimizedUpdateQuery(valueToSet);
        assertPushdownPlan(stmt, query, true);
        long optimizedTime = timeStatement(query, queryRunCount);
        LOG.info("Optimized plain UPDATE total time (ms): " + optimizedTime);
        assertEquals(updatedResult, getSortedRowList(stmt.executeQuery(selectQuery1)));
        assertEquals(updatedResult, getSortedRowList(stmt.executeQuery(selectQuery2)));
      }

      // Prepared optimized query
      {
        stmt.executeUpdate(getResetQuery());
        String queryString = getOptimizedPreparedUpdateQueryString();
        assertPushdownPlan(stmt, queryString, (q) -> {
          try {
            return prepareUpdateQuery(q, valueToSet);
          } catch (Exception ex) {
            throw new RuntimeException(ex); // Functional Java, yeah, right
          }
        }, true);
        try (PreparedStatement pquery = prepareUpdateQuery(queryString, valueToSet)) {
          long optimizedTime = timeStatement(pquery, queryRunCount);
          LOG.info("Optimized prepared UPDATE total time (ms): " + optimizedTime);
          assertEquals(updatedResult, getSortedRowList(stmt.executeQuery(selectQuery1)));
          assertEquals(updatedResult, getSortedRowList(stmt.executeQuery(selectQuery2)));
        }
      }

      // Cleanup
      stmt.executeUpdate(getResetQuery());
    }

    private void testDelete(Statement stmt, List<Row> rowsToDelete) throws Exception {
      int lastColIdx = rowsToDelete.get(0).elems.size() - 1;

      String truncateQuery = String.format("TRUNCATE TABLE %s", tableName);
      String selectQuery = getOptimizedSelectQuery();

      long tableSizeBefore = getTableSize(stmt);
      long tableSizeAfter = tableSizeBefore - rowsToDelete.size();

      long nonOptimizedTime;
      {
        // Full scan update query used as an execution time reference
        String inClause = StringUtils
            .join(rowsToDelete.stream().map(r -> r.getInt(lastColIdx)).iterator(), ",");
        String nonOptimizedQuery = String.format(
            "DELETE FROM %s WHERE v IN (%s)", tableName, inClause);
        assertPushdownPlan(stmt, nonOptimizedQuery, false);
        nonOptimizedTime = timeStatement(nonOptimizedQuery, queryRunCount);
        LOG.info("Non-optimized DELETE total time (ms): " + nonOptimizedTime);
        assertEquals(Collections.EMPTY_LIST, getSortedRowList(stmt.executeQuery(selectQuery)));
        assertEquals(tableSizeAfter, getTableSize(stmt));
      }

      // Plain optimized query
      {
        stmt.executeUpdate(truncateQuery);
        fillTable(stmt);
        String query = getOptimizedDeleteQuery();
        assertPushdownPlan(stmt, query, true);
        long optimizedTime = timeStatement(query, queryRunCount);
        LOG.info("Optimized plain DELETE total time (ms): " + optimizedTime);
        assertEquals(Collections.EMPTY_LIST, getSortedRowList(stmt.executeQuery(selectQuery)));
        assertEquals(tableSizeAfter, getTableSize(stmt));
      }

      // Prepared optimized query
      {
        stmt.executeUpdate(truncateQuery);
        fillTable(stmt);
        String queryString = getOptimizedPreparedDeleteQueryString();
        assertPushdownPlan(stmt, queryString, (q) -> {
          try {
            return prepareSelectOrDeleteQuery(q);
          } catch (Exception ex) {
            throw new RuntimeException(ex); // Functional Java, yeah, right
          }
        }, true);
        try (PreparedStatement pquery = prepareSelectOrDeleteQuery(queryString)) {
          long optimizedTime = timeStatement(pquery, queryRunCount);
          LOG.info("Optimized prepared DELETE total time (ms): " + optimizedTime);
          assertEquals(Collections.EMPTY_LIST, getSortedRowList(stmt.executeQuery(selectQuery)));
          assertEquals(tableSizeAfter, getTableSize(stmt));
        }
      }
    }

    //
    // Helpers
    //

    private long getTableSize(Statement stmt) throws Exception {
      String selectCountQuery = String.format("SELECT COUNT(*) FROM %s", tableName);
      return getRowList(stmt.executeQuery(selectCountQuery)).get(0).getLong(0);
    }

    /** Assert that the plan for the given query results in index/seq scan */
    private void assertPushdownPlan(
        Statement stmt,
        String queryString,
        Function<String, PreparedStatement> prepare,
        boolean shouldUseIndex) throws Exception {
      assertPushdownPlan(
          prepare.apply("EXPLAIN " + queryString).executeQuery(),
          queryString,
          shouldUseIndex);
    }

    /** Assert that the plan for the given query results in index/seq scan */
    private void assertPushdownPlan(Statement stmt, String query, boolean shouldUseIndex)
        throws Exception {
      assertPushdownPlan(stmt.executeQuery("EXPLAIN " + query), query, shouldUseIndex);
    }

    /** Assert that the plan for the given query results in index/seq scan */
    private void assertPushdownPlan(ResultSet rs, String query, boolean shouldUseIndex)
        throws Exception {
      List<Row> rows = getRowList(rs);
      assertTrue("Invalid EXPLAIN query for " + query, rows.size() > 0);
      assertTrue("Invalid EXPLAIN query for " + query, rows.get(0).elems.size() == 1);
      String plan = StringUtils.join(rows.stream().map(r -> r.getString(0)).iterator(), "\n");
      if (shouldUseIndex) {
        assertTrue(
            String.format("Expected query '%s' to be an index scan, but the plan was:\n%s",
                query, plan),
            plan.contains("Index Scan") || plan.contains("Index Only Scan"));
      } else {
        assertTrue(
            String.format("Expected query '%s' to be a seq scan, but the plan was:\n%s",
                query, plan),
            plan.contains("Seq Scan"));
      }
    }
  }

  /**
   * Tests pushdown of aggregate SELECTs statements by analyzing YSQL {@code AggregatePushdowns}
   * metrics.
   * <p>
   * Uses a {@code (id int PRIMARY KEY, v int)} table
   */
  private class AggregatePushdownTester {
    private final String tableName;
    private final int numRowsToInsert;
    private final String optimizedExpr;

    public AggregatePushdownTester(String tableName, int numRowsToInsert, String optimizedExpr) {
      this.tableName = tableName;
      this.numRowsToInsert = numRowsToInsert;
      this.optimizedExpr = optimizedExpr;
    }

    public void test() throws Exception {
      try (Statement stmt = connection.createStatement()) {
        stmt.executeUpdate(String.format(
            "CREATE TABLE %s (id int PRIMARY KEY, v int)",
            tableName));
        stmt.executeUpdate(String.format(
            "INSERT INTO %s ("
                + "SELECT generate_series, generate_series + 1 FROM generate_series(1, %s)"
                + ");",
            tableName, numRowsToInsert));
        verifyPushdown(stmt);
        stmt.executeUpdate(String.format("DROP TABLE %s", tableName));
      }
    }

    private void verifyPushdown(Statement stmt) throws Exception {
      String query = String.format("SELECT %s FROM %s", optimizedExpr, tableName);
      verifyStatementMetric(
          stmt,
          query,
          AGGREGATE_PUSHDOWNS_METRIC,
          1 /* queryMetricDelta */,
          1 /* txnMetricDelta */,
          true /* validStmt */
      );
    }
  }
}
