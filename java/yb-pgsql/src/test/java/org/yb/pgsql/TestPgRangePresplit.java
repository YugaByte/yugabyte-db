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
import org.yb.client.ListTablesResponse;
import org.yb.client.YBClient;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.master.Master;

import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

import static org.yb.AssertionWrappers.assertEquals;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgRangePresplit extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgRangePresplit.class);

  @Test
  public void testSingleColumnRangePK() throws Exception {
    List<Row> expectedRows = new ArrayList<>();

    try (Statement statement = connection.createStatement()) {
      statement.execute("create table t(id int, primary key(id asc)) split at values((10),(100))");

      for (int id  = 1; id <= 110; id++) {
        statement.execute(String.format("insert into t(id) values (%d)", id));
        expectedRows.add(new Row(id));
      }

      // Validate insertion.
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check equality condition.
      expectedRows.clear();
      expectedRows.add(new Row(10));
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE id = 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check IN clause.
      expectedRows.clear();
      expectedRows.add(new Row(1));
      expectedRows.add(new Row(10));
      expectedRows.add(new Row(11));
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE id IN (1,10,11)")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check range condition (data from single tablet).
      expectedRows.clear();
      for (int id = 1; id < 5; id++) {
        expectedRows.add(new Row(id));
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE id < 5")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check range condition (data from multiple tablets).
      expectedRows.clear();
      for (int id = 90; id < 105; id++) {
        expectedRows.add(new Row(id));
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE id < 105 AND id >= 90")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check no match.
      expectedRows.clear();
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE id = 0")) {
        assertEquals(expectedRows, getRowList(rs));
      }
    }
  }

  @Test
  public void testMultiColumnRangePK() throws Exception {
    List<Row> expectedRows = new ArrayList<>();

    try (Statement statement = connection.createStatement()) {
      statement.execute("create table t2(a int, b text, primary key(b desc, a asc)) " +
        "split at values(('8'),('5'), ('5', 10), ('3'))");

      String[] prefix = {"0","1","2","3","4","5","6","7","8","9"};
      // Rows should be sorted by (b desc, a asc).
      for (int i = 9; i >= 0; i--) {
        for (int j = 0;  j <= 100; j += 10) {
          statement.execute(String.format("insert into t2(a, b) values (%d, '%s_b')",
            j + i, prefix[i]));
          expectedRows.add(new Row(j + i, prefix[i] + "_b"));
        }
      }

      // Validate insertion.
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check equality condition.
      expectedRows.clear();
      expectedRows.add(new Row(32, "2_b"));
      try (ResultSet rs = statement.executeQuery(
          "SELECT * FROM t2 WHERE a = 32 AND b = '2_b'")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check IN clause.
      expectedRows.clear();
      expectedRows.add(new Row(8, "8_b"));
      expectedRows.add(new Row(54, "4_b"));
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2 WHERE " +
          "a IN (54, 8) AND b IN ('4_b', '8_b')")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check IN clause on first column and equality on second column.
      expectedRows.clear();
      expectedRows.add(new Row(8, "8_b"));
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2 WHERE " +
          "a = 8 AND b IN ('4_b', '8_b')")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check equality on first column and range condition on second column.
      expectedRows.clear();
      for (int id = 20; id <= 100; id += 10) {
         expectedRows.add(new Row(id, "0_b"));
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2 WHERE " +
          "b = '0_b' AND a > 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check range conditions on both columns.
      expectedRows.clear();
      for (int id = 9; id < 80; id += 10) {
        expectedRows.add(new Row(id, "9_b"));
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2 WHERE b > '8_b' AND a < 80")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check range condition on first column.
      expectedRows.clear();
      for (int id = 9; id < 110; id += 10) {
        expectedRows.add(new Row(id, "9_b"));
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2 WHERE b > '8_b'")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Check range condition on second column.
      expectedRows.clear();
      for (int i = 9; i >= 0; i--) {
        for (int j = 80;  j < 110; j += 10) {
          expectedRows.add(new Row(j + i, prefix[i] + "_b"));
        }
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t2 WHERE a >= 80")) {
        assertEquals(expectedRows, getRowList(rs));
      }
    }
  }

  @Test
  public void testWrites() throws Exception {
    List<Row> expectedRows = new ArrayList<>();

    try (Statement statement = connection.createStatement()) {
      statement.execute("create table t3(a int, b int, v int, primary key(a asc, b desc)) " +
          "split at values((10), (20), (20, 5), (30))");

      for (int id  = 1; id <= 50; id++) {
        statement.execute(String.format("insert into t3(a, b, v) values (%d, %d, %d)", id, id, id));
        expectedRows.add(new Row(id, id, id));
      }

      // Validate insertion.
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t3")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Single row update.
      expectedRows.clear();
      expectedRows.add(new Row("Update on t3"));
      expectedRows.add(new Row("  ->  Result"));
      try (ResultSet rs = statement.executeQuery(
          "EXPLAIN(costs off) UPDATE t3 SET v = 0 WHERE a = 10 AND b = 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      statement.execute("UPDATE t3 SET v = 0 WHERE a = 10 AND b = 10");
      expectedRows.clear();
      expectedRows.add(new Row(10, 10, 0));
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t3 WHERE a = 10 AND b = 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Multi row update.
      expectedRows.clear();
      expectedRows.add(new Row("Update on t3"));
      expectedRows.add(new Row("  ->  Index Scan using t3_pkey on t3"));
      expectedRows.add(new Row("        Index Cond: (a < 10)"));
      try (ResultSet rs = statement.executeQuery(
          "EXPLAIN(costs off) UPDATE t3 SET v = 0 WHERE a < 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      statement.execute("UPDATE t3 SET v = 0 WHERE a < 10");
      expectedRows.clear();
      for (int id = 1; id < 10; id++) {
        expectedRows.add(new Row(id, id, 0));
      }
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t3 WHERE a < 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Single row delete.
      expectedRows.clear();
      expectedRows.add(new Row("Delete on t3"));
      expectedRows.add(new Row("  ->  Result"));
      try (ResultSet rs = statement.executeQuery(
          "EXPLAIN(costs off) DELETE FROM t3 WHERE a = 10 AND b = 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      statement.execute("DELETE FROM t3 WHERE a = 10 AND b = 10");
      expectedRows.clear();
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t3 WHERE a = 10 AND b = 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      // Multi row delete.
      expectedRows.clear();
      expectedRows.add(new Row("Delete on t3"));
      expectedRows.add(new Row("  ->  Index Scan using t3_pkey on t3"));
      expectedRows.add(new Row("        Index Cond: (a < 10)"));
      try (ResultSet rs = statement.executeQuery(
          "EXPLAIN(costs off) DELETE FROM t3 WHERE a < 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      statement.execute("DELETE FROM t3 WHERE a < 10");
      expectedRows.clear();
      try (ResultSet rs = statement.executeQuery("SELECT * FROM t3 WHERE a < 10")) {
        assertEquals(expectedRows, getRowList(rs));
      }
    }
  }

  @Test
  public void testIndexes() throws Exception {
    List<Row> expectedRows = new ArrayList<>();

    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test(k SERIAL PRIMARY KEY, a INT, b INT, v VARCHAR)");
      statement.execute(
              "CREATE INDEX test_ab_idx ON test(a ASC, b ASC) SPLIT AT VALUES((10), (10, 20))");
      for (int a = 5; a <= 15; ++a) {
        for (int b = 15; b <= 25; ++b) {
          for (int i = 0; i < 2; ++i) {
            statement.execute(String.format(
              "INSERT INTO test(a, b, v) VALUES(%d, %d, 'a_%d_b_%d')", a, b, a, b));
          }
        }
      }
      expectedRows.clear();
      expectedRows.add(new Row("Index Scan using test_ab_idx on test"));
      expectedRows.add(new Row("  Index Cond: ((b < 22) AND (b > 17))"));
      String query = "SELECT a,b,v FROM test WHERE b < 22 AND b > 17";
      try (ResultSet rs = statement.executeQuery("EXPLAIN(COSTS OFF) " + query)) {
        assertEquals(expectedRows, getRowList(rs));
      }

      expectedRows.clear();
      for (int a = 5; a <= 15; ++a) {
        for (int b = 18; b <= 21; ++b) {
          for (int i = 0; i < 2; ++i) {
            expectedRows.add(new Row(a, b, String.format("a_%d_b_%d", a, b)));
          }
        }
      }
      try (ResultSet rs = statement.executeQuery(query)) {
        assertEquals(expectedRows, getSortedRowList(rs));
      }
    }
    // Check index was splitted into 3 tablets
    YBClient client = miniCluster.getClient();
    List<Master.ListTablesResponsePB.TableInfo> tables =
            client.getTablesList("test_ab_idx").getTableInfoList();
    assertEquals(1, tables.size());
    String tableUuid = tables.get(0).getId().toStringUtf8();
    assertEquals(3, client.getTabletUUIDs(client.openTableByUUID(tableUuid)).size());
  }

  @Test
  public void testUniqueIndexes() throws Exception {
    List<Row> expectedRows = new ArrayList<>();

    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test(k SERIAL PRIMARY KEY, a INT, b INT, v VARCHAR)");
      statement.execute(
        "CREATE UNIQUE INDEX test_ab_idx ON test(a ASC, b ASC) SPLIT AT VALUES((10), (10, 20))");
      for (int a = 5; a <= 15; ++a) {
        for (int b = 15; b <= 25; ++b) {
          statement.execute(String.format(
            "INSERT INTO test(a, b, v) VALUES(%d, %d, 'a_%d_b_%d')", a, b, a, b));
        }
      }
      expectedRows.clear();
      expectedRows.add(new Row("Index Scan using test_ab_idx on test"));
      expectedRows.add(new Row("  Index Cond: ((b < 22) AND (b > 17))"));
      String query = "SELECT a,b,v FROM test WHERE b < 22 AND b > 17";
      try (ResultSet rs = statement.executeQuery("EXPLAIN(COSTS OFF) " + query)) {
        assertEquals(expectedRows, getRowList(rs));
      }

      expectedRows.clear();
      for (int a = 5; a <= 15; ++a) {
        for (int b = 18; b <= 21; ++b) {
          expectedRows.add(new Row(a, b, String.format("a_%d_b_%d", a, b)));
        }
      }
      try (ResultSet rs = statement.executeQuery(query)) {
        assertEquals(expectedRows, getSortedRowList(rs));
      }
    }
    // Check index was splitted into 3 tablets
    YBClient client = miniCluster.getClient();
    List<Master.ListTablesResponsePB.TableInfo> tables =
      client.getTablesList("test_ab_idx").getTableInfoList();
    assertEquals(1, tables.size());
    String tableUuid = tables.get(0).getId().toStringUtf8();
    assertEquals(3, client.getTabletUUIDs(client.openTableByUUID(tableUuid)).size());
  }
}
