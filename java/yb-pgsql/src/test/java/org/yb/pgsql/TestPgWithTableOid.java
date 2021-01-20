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

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.stream.Collectors;

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgWithTableOid extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgWithTableOid.class);

  // This test is for checking the behaviour when reusing oids in pg_constraint for new tables.
  @Test
  public void testConflictingOidsWithPgConstraint() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("set yb_enable_create_with_table_oid=1");
      statement.execute("CREATE TABLE test_table(id int, val int," +
          "CONSTRAINT tt_id_pkey PRIMARY KEY(id)," +
          "CONSTRAINT tt_val_unq UNIQUE(val))");
      statement.execute("INSERT INTO test_table(id, val) VALUES (1,1), (2,2), (3,3)");
      List<Row> expectedRows = Arrays.asList(new Row(1,1), new Row(2,2), new Row(3,3));

      // Get oids that were created.
      long table_oid;
      long constraint1_oid;
      long constraint2_oid;
      String query = "SELECT oid FROM pg_class WHERE relname = 'test_table'";
      try (ResultSet rs = statement.executeQuery(query)) {
        table_oid = getSingleRow(rs).getLong(0);
      }
      query = "SELECT oid FROM pg_constraint WHERE conname = 'tt_id_pkey'";
      try (ResultSet rs = statement.executeQuery(query)) {
        constraint1_oid = getSingleRow(rs).getLong(0);
      }
      query = "SELECT oid FROM pg_constraint WHERE conname = 'tt_val_unq'";
      try (ResultSet rs = statement.executeQuery(query)) {
        constraint2_oid = getSingleRow(rs).getLong(0);
      }

      // Check that creating a table with the same table_oid fails.
      runInvalidQuery(statement,
          "CREATE TABLE test_table2(id int, val int," +
            "CONSTRAINT tt_id_pkey2 PRIMARY KEY(id)," +
            "CONSTRAINT tt_val_unq2 UNIQUE(val))" +
            "WITH (table_oid = " + table_oid + ")",
          "Oid " + table_oid + " is in use.");

      // Try creating new tables with the oids of the constraints.
      statement.execute("CREATE TABLE test_table2(id int, val int," +
          "CONSTRAINT tt_id_pkey2 PRIMARY KEY(id)," +
          "CONSTRAINT tt_val_unq2 UNIQUE(val)) " +
          "WITH (table_oid = " + constraint1_oid + ")");
      statement.execute("INSERT INTO test_table2(id, val) VALUES (1,1), (2,2), (3,3)");

      statement.execute("CREATE TABLE test_table3(id int, val int," +
          "CONSTRAINT tt_id_pkey3 PRIMARY KEY(id)," +
          "CONSTRAINT tt_val_unq3 UNIQUE(val)) " +
          "WITH (table_oid = " + constraint2_oid + ")");
      statement.execute("INSERT INTO test_table3(id, val) VALUES (1,1), (2,2), (3,3)");

      // Check rows.
      try (ResultSet rs = statement.executeQuery("SELECT * FROM test_table ORDER BY id")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      try (ResultSet rs = statement.executeQuery("SELECT * FROM test_table2 ORDER BY id")) {
        assertEquals(expectedRows, getRowList(rs));
      }

      try (ResultSet rs = statement.executeQuery("SELECT * FROM test_table3 ORDER BY val")) {
        assertEquals(expectedRows, getRowList(rs));
      }
    }
  }

  // This test is for checking the behaviour when using oids that would be
  // selected next in pg_class for new tables.
  @Test
  public void testConflictingOidsWithPgClass() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("set yb_enable_create_with_table_oid=1");
      // First create a simple table and get its oid.
      statement.execute("CREATE TABLE test_table1(a int)");
      long table1_oid;
      String query = "SELECT oid FROM pg_class WHERE relname = 'test_table1'";
      try (ResultSet rs = statement.executeQuery(query)) {
        table1_oid = getSingleRow(rs).getLong(0);
      }

      // A simple table will generate 3 new oids: table1_oid, table1_oid + 1, table1_oid + 2
      //  (these additional oids are used in pg_type)
      // We will next generate a new table with table_oid = table1_oid + 5
      // This will also generate two new oids: table1_oid + 3, table1_oid + 4
      long table2_oid = table1_oid + 5;
      statement.execute("CREATE TABLE test_table2(a int) WITH (table_oid = " + table2_oid + ")");
      query = "SELECT oid FROM pg_class WHERE relname = 'test_table2'";
      try (ResultSet rs = statement.executeQuery(query)) {
        assertEquals(table2_oid, getSingleRow(rs).getLong(0).longValue());
      }

      // Ensure that pg_type has the 4 expected oids.
      query = "SELECT COUNT(*) FROM pg_type WHERE " +
              "oid = " + (table1_oid + 1) + " or " +
              "oid = " + (table1_oid + 2) + " or " +
              "oid = " + (table1_oid + 3) + " or " +
              "oid = " + (table1_oid + 4);
      try (ResultSet rs = statement.executeQuery(query)) {
        assertEquals(4, getSingleRow(rs).getLong(0).longValue());
      }

      // Now generate a normal table. It should try to use the next generated oid (table1_oid + 5),
      // however since that value is in use by test_table2, it should end up having
      // oid = table1_oid + 6.
      statement.execute("CREATE TABLE test_table3(a int)");
      query = "SELECT oid FROM pg_class WHERE relname = 'test_table3'";
      try (ResultSet rs = statement.executeQuery(query)) {
        assertEquals(table1_oid + 6, getSingleRow(rs).getLong(0).longValue());
      }
    }
  }

  private void createDatabaseObjects(Connection cxn, int idx) throws Exception {
    try (Statement stmt = cxn.createStatement()) {
      stmt.execute("set yb_enable_create_with_table_oid=1");
      // Execute a simplest SQL statement.
      stmt.execute("SELECT 1");
      int tableOid = 33333;
      int indexOid = 44444;

      // Create simple table.
      String tableName = "test_createwithtableoid";
      String sql =
          String.format("CREATE TABLE %s(h bigint, r float, vi int, vs text, PRIMARY KEY (h, r))" +
                        "WITH (table_oid = %d)", tableName, tableOid);
      stmt.execute(sql);

      // Create index.
      sql = String.format("CREATE INDEX %s_rindex on %s(r) WITH (table_oid = %d)",
                          tableName, tableName, indexOid);
      stmt.execute(sql);

      // Insert some data, data will vary depending on idx.
      for (int i = 0; i < 100 * idx; i += 1 * idx) {
        sql = String.format("INSERT INTO %s VALUES (%d, %f, %d, 'value_%d')",
                            tableName, i, 1.5*i, 2*i, i);
        stmt.execute(sql);
      }
    }
  }

  private void validateDatabaseObjects(Connection cxn, int idx) throws Exception {
    try (Statement stmt = cxn.createStatement()) {
      String tableName = "test_createwithtableoid";
      int tableOid = 33333;
      int indexOid = 44444;

      // Check table_oids;
      String query = "SELECT oid FROM pg_class WHERE relname = '" + tableName + "'";
      try (ResultSet rs = stmt.executeQuery(query)) {
        assertEquals(tableOid, getSingleRow(rs).getLong(0).longValue());
      }
      query = "SELECT oid FROM pg_class WHERE relname = '" + tableName + "_rindex'";
      try (ResultSet rs = stmt.executeQuery(query)) {
        assertEquals(indexOid, getSingleRow(rs).getLong(0).longValue());
      }

      // Test table contents.
      final String PRIMARY_KEY = "test_createwithtableoid_pkey";
      List<Row> allRows = new ArrayList<>();
      for (int i = 0; i < 100 * idx; i += 1 * idx) {
        allRows.add(new Row((long) i,
                            1.5*i,
                            2*i,
                            "value_" + i));
      }

      query = "SELECT * FROM " + tableName;
      try (ResultSet rs = stmt.executeQuery(query)) {
        assertEquals(allRows, getSortedRowList(rs));
      }
      assertFalse(isIndexScan(stmt, query, PRIMARY_KEY));

      query = "SELECT * FROM " + tableName + " WHERE h = " + idx;
      try (ResultSet rs = stmt.executeQuery(query)) {
        List<Row> expectedRows = allRows.stream()
            .filter(row -> row.getLong(0).equals((long) idx))
            .collect(Collectors.toList());
        assertEquals(1, expectedRows.size());
        assertEquals(expectedRows, getSortedRowList(rs));
      }
      assertTrue(isIndexScan(stmt, query, PRIMARY_KEY));
    }
  }

  // This test is for checking the behaviour when using the same table_oids for
  // tables in different databases (This is fine since each database has its
  // own pg_class table).
  @Test
  public void testConflictingTableOidsAcrossDatabases() throws Exception {
    String dbnameroot = "testwithtableoids_db";
    final int kNumLoops = 3;

    // Create databases and objects.
    for (int i = 0; i < kNumLoops; i++) {
      String dbname = dbnameroot + i;
      try (Connection connection0 = getConnectionBuilder().withTServer(0).connect();
          Statement statement = connection0.createStatement()) {
        statement.execute(String.format("CREATE DATABASE %s", dbname));

        // Creating a few objects in the database.
        try (Connection connection1 = getConnectionBuilder().withTServer(1)
                                                            .withDatabase(dbname)
                                                            .connect()) {
          createDatabaseObjects(connection1, i + 1);
        }
      }
    }

    // Validate that everything works fine.
    for (int i = 0; i < kNumLoops; i++) {
      String dbname = dbnameroot + i;
      try (Connection connection0 = getConnectionBuilder().withTServer(1)
                                                         .withDatabase(dbname)
                                                         .connect()) {
        validateDatabaseObjects(connection0, i + 1);
      }
    }
  }
}
