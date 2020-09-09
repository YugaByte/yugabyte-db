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

import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.List;

import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgPrepareExecute extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgPrepareExecute.class);

  @Test
  public void testPgPrepareExecute() throws Exception {
    createSimpleTable("test");

    try (Statement statement = connection.createStatement()) {

      statement.execute("PREPARE ins (bigint, double precision, int, text) AS " +
                            "INSERT INTO test(h, r, vi, vs) VALUES ($1, $2, $3, $4)");

      statement.execute("EXECUTE ins(1, 2.0, 3, 'a')");
      statement.execute("EXECUTE ins(2, 3.0, 4, 'b')");

      try (ResultSet rs = statement.executeQuery("SELECT * FROM test ORDER BY h")) {
        assertNextRow(rs, 1L, 2.0D, 3, "a");
        assertNextRow(rs, 2L, 3.0D, 4, "b");
        assertFalse(rs.next());
      }
    }
  }

  @Test
  public void testJdbcPrepareExecute() throws Exception {
    createSimpleTable("test");

    //----------------------------------------------------------------------------------------------
    // Test prepared insert.
    try (PreparedStatement ins = connection.prepareStatement("INSERT INTO test(h, r, vi, vs)" +
                                                               " VALUES (?, ?, ?, ?)")) {

      ins.setLong(1,1);
      ins.setDouble(2,2.0);
      ins.setInt(3,3);
      ins.setString(4,"a");
      ins.execute();

      ins.setLong(1,2);
      ins.setDouble(2,3.0);
      ins.setInt(3,4);
      ins.setString(4,"b");
      ins.execute();

      // Test invalid statement (wrong type for third param).
      ins.setLong(1,3);
      ins.setDouble(2,4.0);
      ins.setString(3,"abc");
      ins.setString(4, "c");
      try {
        ins.execute();
        fail("Prepared statement did not fail.");
      } catch (SQLException e) {
        LOG.info("Expected exception", e);
      }
    }

    // Check rows.
    try (Statement statement = connection.createStatement()) {
      try (ResultSet rs = statement.executeQuery("SELECT * FROM test ORDER BY h")) {
        assertNextRow(rs, 1L, 2.0D, 3, "a");
        assertNextRow(rs, 2L, 3.0D, 4, "b");
        assertFalse(rs.next());
      }
    }

    //----------------------------------------------------------------------------------------------
    // Test prepared select.
    try (PreparedStatement sel = connection.prepareStatement("SELECT * FROM test WHERE h = ?")) {
      sel.setLong(1,2);
      ResultSet rs = sel.executeQuery();
      assertNextRow(rs, 2L, 3.0D, 4, "b");
      assertFalse(rs.next());
    }

    // Test bind variable pushdown:
    // Equality on hash key -- expect index is used with index condition.
    String query = "EXPLAIN SELECT * FROM test WHERE h = ?";
    try (PreparedStatement sel = connection.prepareStatement(query)) {
      sel.setLong(1, 2);
      ResultSet rs = sel.executeQuery();
      List<Row> rows = getRowList(rs);
      assertTrue(rows.toString().contains("Index Cond: "));
    }

    // Test bind variable pushdown:
    // Inequality on hash key -- until index range scan is supported, seq scan is still the best
    // path.
    query = "EXPLAIN SELECT * FROM test WHERE h > ?";
    try (PreparedStatement sel = connection.prepareStatement(query)) {
      sel.setLong(1, 2);
      ResultSet rs = sel.executeQuery();
      List<Row> rows = getRowList(rs);
      assertTrue(rows.toString().contains("Seq Scan"));
    }
  }

}
