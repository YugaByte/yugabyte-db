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

import static org.yb.AssertionWrappers.assertEquals;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgInsert extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgInsert.class);

  @Test
  public void testBasicInsert() throws SQLException {
    createSimpleTable("test");
    try (Statement statement = connection.createStatement()) {
      List<Row> expectedRows = new ArrayList<>();

      // Test simple insert.
      statement.execute("INSERT INTO test(h, r, vi, vs) VALUES (1, 1.5, 2, 'abc')");
      expectedRows.add(new Row(1L, 1.5D, 2, "abc"));

      // Test different column order.
      statement.execute("INSERT INTO test(vs, vi, h, r) VALUES ('def', 3, 2, 2.5)");
      expectedRows.add(new Row(2L, 2.5D, 3, "def"));

      // Test null values (for non-primary-key columns).
      statement.execute("INSERT INTO test(h, r, vi, vs) VALUES (3, 3.5, null, null)");
      // 0 and "" are the default values JDBC will report for integer and text types, respectively.
      expectedRows.add(new Row(3L, 3.5D, null, null));

      // Test missing values (for non-primary-key columns).
      statement.execute("INSERT INTO test(h, r) VALUES (4, 4.5)");
      // 0 and "" are the default values JDBC will report for integer and text types, respectively.
      expectedRows.add(new Row(4L, 4.5D, null, null));

      // Test inserting multiple value sets.
      statement.execute("INSERT INTO test(h, r, vi, vs) VALUES (5, 5.5, 6, 'ghi'), " +
                            "(6, 6.5, 7, 'jkl'), (7, 7.5, 8, 'mno')");
      expectedRows.add(new Row(5L, 5.5D, 6, "ghi"));
      expectedRows.add(new Row(6L, 6.5D, 7, "jkl"));
      expectedRows.add(new Row(7L, 7.5D, 8, "mno"));

      // Test null values (for primary-key columns) -- expecting error.
      runInvalidQuery(statement,
          "INSERT INTO test(h, r, vi, vs) VALUES (8, null, 2, 'abc')",
          "violates not-null constraint");
      runInvalidQuery(statement,
          "INSERT INTO test(h, r, vi, vs) VALUES (null, 8.5, 2, 'abc')",
          "violates not-null constraint");

      // Test missing values (for primary-key columns) -- expecting error.
      runInvalidQuery(statement,
          "INSERT INTO test(r, vi, vs) VALUES (9, 2, 'abc')",
          "violates not-null constraint");
      runInvalidQuery(statement,
          "INSERT INTO test(h, vi, vs) VALUES (9.5, 2, 'abc')",
          "violates not-null constraint");

      // Test duplicate primary key -- expecting error;
      runInvalidQuery(statement,
          "INSERT INTO test(h, r, vi, vs) VALUES (1, 1.5, 2, 'abc')",
          "violates unique constraint");
      runInvalidQuery(statement,
          "INSERT INTO test(h, r, vi, vs) VALUES (1, 1.5, 9, 'xyz')",
          "violates unique constraint");
      runInvalidQuery(statement,
          "INSERT INTO test(h, r) VALUES (1, 1.5)",
          "violates unique constraint");

      // Sort expected rows to compare with result set.
      Collections.sort(expectedRows);

      // Check rows.
      try (ResultSet rs = statement.executeQuery("SELECT * FROM test")) {
        assertEquals(expectedRows, getSortedRowList(rs));
      }
    }
  }

  @Test
  public void testExpressions() throws Exception {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable("test");

      // Test expressions in INSERT values.
      statement.execute("INSERT INTO test(h, r, vi, vs) " +
                            "VALUES (floor(1 + 1.5), log(3, 27), ceil(pi()), 'ab' || 'c')");
      assertOneRow(statement, "SELECT * FROM test", 2L, 3.0D, 4, "abc");
    }
  }

  @Test
  public void testInsertReturn() throws SQLException {
    String tableName = "test_insert_return";
    createSimpleTable(tableName);

    try (Statement statement = connection.createStatement()) {
      String stmt_format = "INSERT INTO %s(h, r, vi, vs) VALUES(%d, %f, %d, '%s') RETURNING %s";
      for (long h = 0; h < 2; h++) {
        for (int r = 0; r < 2; r++) {
          List<Row> expectedRows = new ArrayList<>();
          String text_stmt = String.format(stmt_format, tableName,
                                           h, r + 0.5, h * 10 + r, "v" + h + r,
                                           "h + 100L, r + 100, vs");
          expectedRows.add(new Row(h + 100, r + 0.5 + 100, "v" + h + r));
          statement.execute(text_stmt);

          // Verify RETURNING clause.
          ResultSet returning = statement.getResultSet();
          assertEquals(expectedRows, getSortedRowList(returning));
        }
      }
    }
  }
}
