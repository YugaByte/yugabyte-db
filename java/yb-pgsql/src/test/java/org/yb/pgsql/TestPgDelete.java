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
import java.util.List;

import static org.yb.AssertionWrappers.assertEquals;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgDelete extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgDelete.class);

  @Test
  public void testBasicDelete() throws SQLException {
    List<Row> allRows = setupSimpleTable("test_basic_del");

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_basic_del WHERE h = 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(10, rcount);
      }
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute("DELETE FROM test_basic_del WHERE h = 2");
    }

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_basic_del WHERE h = 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(0, rcount);
      }
    }
  }

  @Test
  public void testBasicDelete2() throws SQLException {
    List<Row> allRows = setupSimpleTable("test_basic_del2");

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_basic_del2 WHERE h > 8";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(10, rcount);
      }
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute("DELETE FROM test_basic_del2 WHERE h > 8");
    }

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_basic_del2 WHERE h > 8";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(0, rcount);
      }
    }
  }

  @Test
  public void testDeleteWithSingleColumnKey() throws SQLException {
    List<Row> allRows = new ArrayList<>();
    String tableName = "test_delete_single_column_key";
    try (Statement statement = connection.createStatement()) {
      createSimpleTableWithSingleColumnKey(tableName);
      String insertTemplate = "INSERT INTO %s(h, r, vi, vs) VALUES (%d, %f, %d, '%s')";

      for (int h = 0; h < 10; h++) {
        int r = h + 100;
        statement.execute(String.format(insertTemplate, tableName,
                                        h, r + 0.5, h * 10 + r, "v" + h + r));
        allRows.add(new Row((long) h,
                            r + 0.5,
                            h * 10 + r,
                            "v" + h + r));
      }
    }

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_delete_single_column_key WHERE h = 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(1, rcount);
      }
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute("DELETE FROM test_delete_single_column_key WHERE h = 2");
    }

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_delete_single_column_key WHERE h = 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(0, rcount);
      }
    }
  }

  @Test
  public void testDeleteWithSingleColumnKey2() throws SQLException {
    List<Row> allRows = new ArrayList<>();
    String tableName = "test_delete_single_column_key2";
    try (Statement statement = connection.createStatement()) {
      createSimpleTableWithSingleColumnKey(tableName);
      String insertTemplate = "INSERT INTO %s(h, r, vi, vs) VALUES (%d, %f, %d, '%s')";

      for (int h = 0; h < 10; h++) {
        int r = h + 100;
        statement.execute(String.format(insertTemplate, tableName,
                                        h, r + 0.5, h * 10 + r, "v" + h + r));
        allRows.add(new Row((long) h,
                            r + 0.5,
                            h * 10 + r,
                            "v" + h + r));
      }
    }

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_delete_single_column_key2 WHERE h > 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(7, rcount);
      }
    }

    try (Statement statement = connection.createStatement()) {
      statement.execute("DELETE FROM test_delete_single_column_key2 WHERE h > 2");
    }

    try (Statement statement = connection.createStatement()) {
      String query = "SELECT h FROM test_delete_single_column_key2 WHERE h > 2";
      try (ResultSet rs = statement.executeQuery(query)) {
        int rcount = 0;
        while (rs.next()) rcount++;
        assertEquals(0, rcount);
      }
    }
  }

  @Test
  public void testDeleteReturn() throws SQLException {
    String tableName = "test_delete_return";
    createSimpleTable(tableName);

    List<Row> expectedRows = new ArrayList<>();
    try (Statement insert_stmt = connection.createStatement()) {
      String insert_format = "INSERT INTO %s(h, r, vi, vs) VALUES(%d, %f, %d, '%s')";
      for (long h = 0; h < 5; h++) {
        for (int r = 0; r < 5; r++) {
          String insert_text = String.format(insert_format, tableName,
                                             h, r + 0.5, h * 10 + r, "v" + h + r);
          if (h == 2 || h == 3) {
            // Constructing rows to be returned by DELETE.
            expectedRows.add(new Row(h + 100L, r + 0.5 + 100, "v" + h + r));
          }
          insert_stmt.execute(insert_text);
        }
      }
    }

    try (Statement delete_stmt = connection.createStatement()) {
        // Delete with RETURNING clause.
      String delete_text = String.format("DELETE FROM %s WHERE h = 2 OR h = 3 " +
                                         "RETURNING h + 100, r + 100, vs", tableName);
      delete_stmt.execute(delete_text);

      // Verify RETURNING clause.
      ResultSet returning = delete_stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));
    }
  }

  @Test
  public void testDeleteUsing() throws SQLException {
    String tableName1 = "test_delete_using";
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
          // Constructing rows to be returned by DELETE.
          expectedRows.add(new Row(h, h + 0.5 , h * 10 + h, "v" + h, 1, 0.5, 10, "v"));
        }
        insert_stmt.execute(insert_text);
      }
    }

    try (Statement delete_stmt = connection.createStatement()) {
      // Testing USING in Delete with RETURNING clause.
      delete_stmt.execute("DELETE FROM " + tableName1 + " USING " + tableName2 +
                          " WHERE " + tableName1 + ".h  = " + tableName2 + ".h RETURNING *");

      // Verify RETURNING clause.
      ResultSet returning = delete_stmt.getResultSet();
      assertEquals(expectedRows, getSortedRowList(returning));
    }
  }
}
