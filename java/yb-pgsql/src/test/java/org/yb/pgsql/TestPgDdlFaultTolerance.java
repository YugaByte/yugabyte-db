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

import com.google.common.net.HostAndPort;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.SanitizerUtil;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.HashSet;
import java.util.Set;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertTrue;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgDdlFaultTolerance extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgDdlFaultTolerance.class);

  /*
   * Test failures caused by check/validation failures in the YSQL layer.
   */
  @Test
  public void testYSQLMetadataValidationErrors() throws Exception {
    try (Connection connection = getConnectionBuilder().connect();
         Statement statement = connection.createStatement()) {

      String insertSql = "INSERT INTO %s VALUES (%d, %d)";

      // Try CREATE with foreign key referencing invalid table -- expect failure.
      runInvalidQuery(statement, "CREATE TABLE t1(a int primary key, b int references t2)",
                      "relation \"t2\" does not exist");

      // Ensure no entry is written/left in pg_class.
      assertNoRows(statement, "SELECT * FROM pg_class where relname='t1'");

      // Setup three valid tables, t1, t2, t3, such that t2 depends on t1, and t3 on t2.
      statement.execute("CREATE TABLE t1(a int primary key, b int)");
      statement.execute("CREATE TABLE t2(a int primary key, b int references t1)");
      statement.execute("CREATE TABLE t3(a int primary key, b int references t2)");

      // Try to drop t1 (should fail with dependency check).
      runInvalidQuery(statement, "DROP TABLE t1",
                      "cannot drop table t1 because other objects depend on it");

      statement.execute(String.format(insertSql, "t1", 1, 0));
      statement.execute(String.format(insertSql, "t2", 2, 1));
      statement.execute(String.format(insertSql, "t3", 3, 2));

      // Try to DROP both t3 and t1 in one statement, t3 should be fine but t1 will fail because
      // t1 still depends on it -- therefore aborting the entire operation.
      runInvalidQuery(statement, "DROP TABLE t1, t3",
                      "cannot drop desired object(s) because other objects depend on them");

      // Try to ALTER t1 to add a FK reference to t3 -- should fail constraints check.
      runInvalidQuery(statement, "ALTER TABLE t1 ADD CONSTRAINT t1fk " +
                        "FOREIGN KEY (b) REFERENCES t3",
                      "violates foreign key constraint \"t1fk\"");


      // Validate all tables are still usable.
      statement.execute(String.format(insertSql, "t1", 2, 1));
      statement.execute(String.format(insertSql, "t2", 3, 2));
      statement.execute(String.format(insertSql, "t3", 4, 3));

      Set<Row> expectedRows = new HashSet<>();
      expectedRows.add(new Row(1, 0));
      expectedRows.add(new Row(2, 1));
      assertRowSet(statement, "SELECT * from t1", expectedRows);

      expectedRows.clear();
      expectedRows.add(new Row(2, 1));
      expectedRows.add(new Row(3, 2));
      assertRowSet(statement, "SELECT * from t2", expectedRows);

      expectedRows.clear();
      expectedRows.add(new Row(3, 2));
      expectedRows.add(new Row(4, 3));
      assertRowSet(statement, "SELECT * from t3", expectedRows);

      // Try to drop all 3 tables at once -- should work fine.
      statement.execute("DROP TABLE t1, t2, t3");

      // Validate no tables still exist.
      assertNoRows(statement, "SELECT * FROM pg_class where relname IN ('t1', 't2', 't3')");
      runInvalidQuery(statement, "SELECT * FROM t1",
                      "relation \"t1\" does not exist");
      runInvalidQuery(statement, "SELECT * FROM t2",
                      "relation \"t2\" does not exist");
      runInvalidQuery(statement, "SELECT * FROM t3",
                      "relation \"t3\" does not exist");
    }
  }

  /*
   * Test failures caused by the DocDB metadata operations (such as Create/Drop/Alter Table
   * requests) that modify the syscatalog protobuf entries and/or create/drop/modify tablets.
   */
  @Test
  public void testDocDBMetadataFailuresForTable() throws Exception {
    try (Connection connection = getConnectionBuilder().connect();
         Statement statement = connection.createStatement()) {

      HostAndPort masterLeaderAddress = getMasterLeaderAddress();
      //--------------------------------------------------------------------------------------------
      // Test table operations.
      {
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 100);
        String createStmt = "CREATE TABLE ddl_ft_table (a int, b text, c float, " +
          "primary key (a HASH, b ASC))";
        // Create table -- should fail with the injected error.
        runInvalidQuery(statement, createStmt, "Injected random failure for testing");

        // CREATE table again -- previous query should roll back cleanly so expect exactly the same
        // error message.
        runInvalidQuery(statement, createStmt, "Injected random failure for testing");

        // Enable DocDB syscatalog writes again.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 0);

        // CREATE table should now succeed.
        statement.execute(createStmt);

        // Table should now be usable.
        String insertSql = "INSERT INTO ddl_ft_table VALUES (%d, '%d', %d.0)";
        statement.execute(String.format(insertSql, 1, 2, 3));
        statement.execute(String.format(insertSql, 4, 5, 6));

        String selectStmt = "SELECT * FROM ddl_ft_table";
        Set<Row> expectedRows = new HashSet<>();
        expectedRows.add(new Row(1, "2", 3.0));
        expectedRows.add(new Row(4, "5", 6.0));
        assertRowSet(statement, selectStmt, expectedRows);

        // Disable syscatalog again.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 100);

        String alterStmt = "ALTER TABLE ddl_ft_table ADD COLUMN d int DEFAULT 99";
        // Alter should fail because DocDB cannot add the column.
        runInvalidQuery(statement, alterStmt, "Injected random failure for testing");

        // Enable syscatalog again.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 0);

        // Alter should now succeed.
        statement.execute(alterStmt);

        // Table should still be usable (and the default set).
        statement.execute("INSERT INTO ddl_ft_table VALUES (7, '8', 9.0, 10)");

        expectedRows.clear();
        expectedRows.add(new Row(1, "2", 3.0, null));
        expectedRows.add(new Row(4, "5", 6.0, null));
        expectedRows.add(new Row(7, "8", 9.0, 10));
        assertRowSet(statement, selectStmt, expectedRows);

        // Disable syscatalog again.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 100);

        String dropStmt = "DROP TABLE ddl_ft_table";
        // Drop the table -- should succeed with warning.
        verifyStatementWarning(statement, dropStmt, "Injected random failure for testing");

        // Table should not be usable after drop.
        runInvalidQuery(statement, String.format(insertSql, 7, 8, 9),
                        "relation \"ddl_ft_table\" does not exist");

        runInvalidQuery(statement, selectStmt, "relation \"ddl_ft_table\" does not exist");

        // We should be able to create a table with the same name without issues.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 0);

        // Create table should now succeed.
        statement.execute(createStmt);

        // New table should be usable.
        statement.execute(String.format(insertSql, 2, 3, 4));
        statement.execute(String.format(insertSql, 5, 6, 7));

        expectedRows = new HashSet<>();
        expectedRows.add(new Row(2, "3", 4.0));
        expectedRows.add(new Row(5, "6", 7.0));
        assertRowSet(statement, selectStmt, expectedRows);

        // Dropping the new table should succeed.
        statement.execute(dropStmt);
      }
    }
  }

  /*
   * Test failures caused by the DocDB metadata operations (such as Create/Drop Index
   * requests) that modify the syscatalog protobuf entries and/or create/drop/modify tablets.
   */
  @Test
  public void testDocDBMetadataFailuresForIndex() throws Exception {
    try (Connection connection = getConnectionBuilder().connect();
         Statement statement = connection.createStatement()) {

      HostAndPort masterLeaderAddress = getMasterLeaderAddress();
      //--------------------------------------------------------------------------------------------
      // Test Index operations.
      {
        String createStmt = "CREATE TABLE ddl_ft_indexed_table(a int, b text, c float, " +
          "PRIMARY KEY (a HASH, b ASC))";
        statement.execute(createStmt);

        // Disable syscatalog writes.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 100);

        String createIndex = "CREATE INDEX ddl_ft_indexed_table_idx " +
          "ON ddl_ft_indexed_table(c ASC, b ASC)";
        // Index create should fail.
        runInvalidQuery(statement, createIndex, "Injected random failure for testing");

        // Table should only have pkey index.
        String pgClassQuery =
          "SELECT indexname from pg_indexes WHERE tablename = 'ddl_ft_indexed_table'";
        HashSet<Row> expectedIdxs = new HashSet<>();
        expectedIdxs.add(new Row("ddl_ft_indexed_table_pkey"));
        assertRowSet(statement, pgClassQuery, expectedIdxs);

        // Enable syscatalog writes.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 0);

        // Can now create new index.
        statement.execute(createIndex);

        // Confirm index exists for the table.
        expectedIdxs.add(new Row("ddl_ft_indexed_table_idx"));
        assertRowSet(statement, pgClassQuery, expectedIdxs);

        // Confirm table and index are usable.
        String insertSql = "INSERT INTO ddl_ft_indexed_table VALUES (%d, '%d', %d.0)";
        statement.execute(String.format(insertSql, 1, 2, 3));
        statement.execute(String.format(insertSql, 4, 5, 6));

        Set<Row> expectedRows = new HashSet<>();
        expectedRows.add(new Row(1, "2", 3.0));
        expectedRows.add(new Row(4, "5", 6.0));
        String selectStmt = "SELECT * FROM ddl_ft_indexed_table";
        String indexSelectStmt = "SELECT * FROM ddl_ft_indexed_table order by c";
        assertRowSet(statement, selectStmt, expectedRows);
        assertRowSet(statement, indexSelectStmt, expectedRows);
        isIndexScan(statement, indexSelectStmt, "ddl_ft_indexed_table_idx");

        // Disable syscatalog writes.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 100);

        // Drop index should succeed but with a warning.
        verifyStatementWarning(statement, "DROP INDEX ddl_ft_indexed_table_idx",
                               "Injected random failure for testing");

        // Table should only have pkey index.
        expectedIdxs.clear();
        expectedIdxs.add(new Row("ddl_ft_indexed_table_pkey"));
        assertRowSet(statement, pgClassQuery, expectedIdxs);

        // Enable syscatalog writes.
        setDocDBSysCatalogWriteRejection(masterLeaderAddress, 0);

        // We should be able to create a new index with the same name.
        statement.execute(createIndex);

        // Confirm index exists for the table.
        expectedIdxs.add(new Row("ddl_ft_indexed_table_idx"));
        assertRowSet(statement, pgClassQuery, expectedIdxs);

        // Confirm table and (new) index are usable.
        insertSql = "INSERT INTO ddl_ft_indexed_table VALUES (%d, '%d', %d.0)";
        statement.execute(String.format(insertSql, 7, 8, 9));
        statement.execute(String.format(insertSql, 10, 11, 12));

        expectedRows.add(new Row(7, "8", 9.0));
        expectedRows.add(new Row(10, "11", 12.0));
        assertRowSet(statement, selectStmt, expectedRows);
        assertRowSet(statement, indexSelectStmt, expectedRows);
        isIndexScan(statement, indexSelectStmt, "ddl_ft_indexed_table_idx");
      }
    }
  }

  /*
   * Test failure injection for YSQL metadata writes (writes to YSQL catalog tables).
   */
  @Test
  public void testYSQLMetadataFailureInjection() throws Exception {
    try (Connection connection = getConnectionBuilder().connect();
         Statement statement = connection.createStatement()) {

      HostAndPort masterLeaderAddress = getMasterLeaderAddress();

      setYSQLCatalogWriteRejection(masterLeaderAddress, 100);

      runInvalidQuery(statement,"CREATE TABLE t4(a int primary key, b int)",
                      "Injected random failure for testing");

      runInvalidQuery(statement,"CREATE TABLE t5(a int primary key, b int)",
                      "Injected random failure for testing");

      setYSQLCatalogWriteRejection(masterLeaderAddress, 0);

      int successCount = 0;
      int totalIterations = 10;
      for (int i = 0; i < totalIterations; i++) {
        boolean success = checkIntermittentYsqlWriteFailure(masterLeaderAddress, statement);
        if (success) {
          successCount += 1;
        }
      }
      assertTrue("Expect at least one fail case", successCount < totalIterations);
      LOG.info("Success count: {}/{}", successCount, totalIterations);
    }
  }

  public boolean checkIntermittentYsqlWriteFailure(HostAndPort masterLeaderAddress,
                                                  Statement statement) throws Exception {

    // There are two cases:
    // 1. If this fails all tables should still be usable.
    // 2. If this succeeds all tables should be removed.
    String insertSql = "INSERT INTO %s VALUES (%d, %d)";

    // Setup three valid tables, t1, t2, t3, such that t2 depends on t1, and t3 on t2.
    statement.execute("CREATE TABLE t1(a int primary key, b int)");
    statement.execute("CREATE TABLE t2(a int primary key, b int references t1)");
    statement.execute("CREATE TABLE t3(a int primary key, b int references t2)");

    statement.execute(String.format(insertSql, "t1", 1, 0));
    statement.execute(String.format(insertSql, "t2", 2, 1));
    statement.execute(String.format(insertSql, "t3", 3, 2));

    // Use just 5% write failure chance because we will end up doing a lot of writes/deletes (to
    // pg_class, pg_attribute, pg_depend, etc).
    // This should translate to a ~90% chance of overall statement failure.
    setYSQLCatalogWriteRejection(masterLeaderAddress, 5);

    boolean success = false;
    try {
      statement.execute("DROP TABLE t1, t2, t3");
      success = true;
    } catch (SQLException e) {
      // Fail case.
      assertTrue(e.getMessage().contains("Injected random failure for testing"));
    }

    setYSQLCatalogWriteRejection(masterLeaderAddress, 0);

    if (!success) {
      // Validate all tables are still usable.
      statement.execute(String.format(insertSql, "t1", 2, 1));
      statement.execute(String.format(insertSql, "t2", 3, 2));
      statement.execute(String.format(insertSql, "t3", 4, 3));

      Set<Row> expectedRows = new HashSet<>();
      expectedRows.add(new Row(1, 0));
      expectedRows.add(new Row(2, 1));
      assertRowSet(statement, "SELECT * from t1", expectedRows);

      expectedRows.clear();
      expectedRows.add(new Row(2, 1));
      expectedRows.add(new Row(3, 2));
      assertRowSet(statement, "SELECT * from t2", expectedRows);

      expectedRows.clear();
      expectedRows.add(new Row(3, 2));
      expectedRows.add(new Row(4, 3));
      assertRowSet(statement, "SELECT * from t3", expectedRows);

      // Now drop it correctly.
      statement.execute("DROP TABLE t1, t2, t3");
    }

    // Check everything got cleaned up.
    assertNoRows(statement,"SELECT * FROM pg_class where relname IN ('t1', 't2', 't3')");
    runInvalidQuery(statement, "SELECT * FROM t1",
                    "relation \"t1\" does not exist");
    runInvalidQuery(statement, "SELECT * FROM t2",
                    "relation \"t2\" does not exist");
    runInvalidQuery(statement, "SELECT * FROM t3",
                    "relation \"t3\" does not exist");

    return success;
  }

  // Utility functions.

  private void setYSQLCatalogWriteRejection(HostAndPort server, int percentage) throws Exception {
    setServerFlag(server,
                  "TEST_ysql_catalog_write_rejection_percentage",
                  Integer.toString(percentage));
    // TODO Adding a 6 second sleep here due to issue #4848.
    // For ASAN sleep needs to be longer to avoid flaky failures due the the same issue.
    if (SanitizerUtil.isASAN()) {
      Thread.sleep(60000);
    } else {
      Thread.sleep(6000);
    }
  }

  private void setDocDBSysCatalogWriteRejection(HostAndPort server,
                                                int percentage) throws Exception {
    setServerFlag(server,
                  "TEST_sys_catalog_write_rejection_percentage",
                  Integer.toString(percentage));
  }

}
