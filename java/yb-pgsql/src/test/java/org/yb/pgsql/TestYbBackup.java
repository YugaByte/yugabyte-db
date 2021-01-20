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

import java.sql.Connection;
import java.sql.Statement;
import java.util.List;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.TableProperties;
import org.yb.util.YBBackupException;
import org.yb.util.YBBackupUtil;
import org.yb.util.YBTestRunnerNonSanitizersOrMac;

import static org.yb.AssertionWrappers.assertArrayEquals;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;
@RunWith(value=YBTestRunnerNonSanitizersOrMac.class)
public class TestYbBackup extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestYbBackup.class);

  @Before
  public void initYBBackupUtil() {
    YBBackupUtil.setMasterAddresses(masterAddresses);
    YBBackupUtil.setPostgresContactPoint(miniCluster.getPostgresContactPoints().get(0));
  }

  @Override
  public int getTestMethodTimeoutSec() {
    return 360; // Usual time for a test ~90 seconds. But can be much more on Jenkins.
  }

  @Override
  protected int overridableNumShardsPerTServer() {
    return 2;
  }

  public void doAlteredYSQLTableBackup(String dbName, TableProperties tp) throws Exception {
    String colocString = tp.isColocated() ? "TRUE" : "FALSE";
    String initialDBName = dbName + "1";
    String restoreDBName = dbName + "2";

    try (Statement stmt = connection.createStatement()) {
      stmt.execute(String.format("CREATE DATABASE %s COLOCATED=%s", initialDBName, colocString));
    }
    try (Connection connection2 = getConnectionBuilder().withDatabase(initialDBName).connect();
         Statement stmt = connection2.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   String.format("WITH (colocated = %s)", colocString));

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
          " (" + String.valueOf(i) +                     // h
          ", " + String.valueOf(100 + i) +               // a
          ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      stmt.execute("ALTER TABLE test_tbl DROP a");
      YBBackupUtil.runYbBackupCreate("--keyspace", "ysql." + initialDBName);

      stmt.execute("INSERT INTO test_tbl (h, b) VALUES (9999, 8.9)");

      YBBackupUtil.runYbBackupRestore("--keyspace", "ysql." + restoreDBName);

      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999", new Row(9999, 8.9));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase(restoreDBName).connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2002.14));
      assertQuery(stmt, "SELECT h FROM test_tbl WHERE h=2000", new Row(2000));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999");
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE " + initialDBName);
      stmt.execute("DROP DATABASE " + restoreDBName);
    }
  }

  @Test
  public void testAlteredYSQLTableBackup() throws Exception {
    doAlteredYSQLTableBackup("altered_db", new TableProperties(
        TableProperties.TP_YSQL | TableProperties.TP_NON_COLOCATED));
  }

  @Test
  public void testAlteredYSQLTableBackupInColocatedDB() throws Exception {
    doAlteredYSQLTableBackup("altered_colocated_db", new TableProperties(
        TableProperties.TP_YSQL | TableProperties.TP_COLOCATED));
  }

  @Test
  public void testMixedColocatedYSQLDatabaseBackup() throws Exception {
    String initialDBName = "yb_colocated";
    String restoreDBName = "yb_colocated2";

    try (Statement stmt = connection.createStatement()) {
      stmt.execute(String.format("CREATE DATABASE %s COLOCATED=TRUE", initialDBName));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase(initialDBName).connect();
         Statement stmt = connection2.createStatement()) {
      // Create 3 tables, 2 colocated and 1 non colocated but in the same db.
      stmt.execute("CREATE TABLE test_tbl1 (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (COLOCATED=TRUE)");
      stmt.execute("CREATE TABLE test_tbl2 (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (COLOCATED=TRUE)");
      stmt.execute("CREATE TABLE test_tbl3 (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (COLOCATED=FALSE)");

      // Insert random rows/values for tables to snapshot
      for (int j = 1; j <= 3; ++j) {
        for (int i = 1; i <= 2000; ++i) {
          stmt.execute("INSERT INTO test_tbl" + String.valueOf(j) + " (h, a, b) VALUES" +
            " (" + String.valueOf(i * j) +                       // h
            ", " + String.valueOf((100 + i) * j) +               // a
            ", " + String.valueOf((2.14 + (float)i) * j) + ")"); // b
        }
      }

      YBBackupUtil.runYbBackupCreate("--keyspace", "ysql." + initialDBName);

      // Insert more rows after taking the snapshot.
      for (int j = 1; j <= 3; ++j) {
        stmt.execute("INSERT INTO test_tbl" + String.valueOf(j) + " (h, a, b) VALUES" +
        " (" + String.valueOf(9999 * j) +                        // h
        ", " + String.valueOf((100 + 9999) * j) +                // a
        ", " + String.valueOf((2.14 + 9999f) * j) + ")");        // b
      }

      YBBackupUtil.runYbBackupRestore("--keyspace", "ysql." + restoreDBName);

      // Verify the original database has the rows we previously inserted.
      for (int j = 1; j <= 3; ++j) {
        for (int i : new int[] {1, 2000, 9999}) {
          assertQuery(stmt, String.format("SELECT * FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row(i * j, (100 + i) * j, (2.14 + (float)i) * j));
        }
      }
    }

    // Verify that the new database and tables are properly configured.
    List<String> tbl1Tablets = YBBackupUtil.getTabletsForTable("ysql." + restoreDBName,
                                                                "test_tbl1");
    List<String> tbl2Tablets = YBBackupUtil.getTabletsForTable("ysql." + restoreDBName,
                                                                "test_tbl2");
    List<String> tbl3Tablets = YBBackupUtil.getTabletsForTable("ysql." + restoreDBName,
                                                                "test_tbl3");
    // test_tbl1 and test_tbl2 are colocated and so should share the exact same tablet.
    assertEquals("test_tbl1 is not colocated", 1, tbl1Tablets.size());
    assertEquals("test_tbl2 is not colocated", 1, tbl2Tablets.size());
    assertArrayEquals("test_tbl1 and test_tbl2 do not share the same colocated tablet",
                      tbl1Tablets.toArray(), tbl2Tablets.toArray());
    // test_tbl3 is not colocated so it should have more tablets.
    assertTrue("test_tbl3 should have more than 1 tablet", tbl3Tablets.size() > 1);
    assertFalse("test_tbl3 uses the colocated tablet", tbl3Tablets.contains(tbl1Tablets.get(0)));

    try (Connection connection2 = getConnectionBuilder().withDatabase(restoreDBName).connect();
         Statement stmt = connection2.createStatement()) {
      // Verify the new database contains all the rows we inserted before taking the snapshot.
      for (int j = 1; j <= 3; ++j) {
        for (int i : new int[] {1, 500, 2000}) {
          assertQuery(stmt, String.format("SELECT * FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row(i * j, (100 + i) * j, (2.14 + (float)i) * j));
          assertQuery(stmt, String.format("SELECT h FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row(i * j));
          assertQuery(stmt, String.format("SELECT a FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row((100 + i) * j));
          assertQuery(stmt, String.format("SELECT b FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row((2.14 + (float)i) * j));
        }
        // Assert that this returns no rows.
        assertQuery(stmt, String.format("SELECT * FROM test_tbl%d WHERE h=%d", j, 9999 * j));
      }
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute(String.format("DROP DATABASE %s", initialDBName));
      stmt.execute(String.format("DROP DATABASE %s", restoreDBName));
    }
  }

  @Test
  public void testAlteredYSQLTableBackupInOriginalCluster() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE  test_tbl (h INT PRIMARY KEY, a INT, b FLOAT)");

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
          " (" + String.valueOf(i) +                     // h
          ", " + String.valueOf(100 + i) +               // a
          ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      YBBackupUtil.runYbBackupCreate("--keyspace", "ysql.yugabyte");
      stmt.execute("ALTER TABLE test_tbl DROP a");
      stmt.execute("INSERT INTO test_tbl (h, b) VALUES (9999, 8.9)");

      try {
        YBBackupUtil.runYbBackupRestore();
        fail("Backup restoring did not fail as expected");
      } catch (YBBackupException ex) {
        LOG.info("Expected exception", ex);
      }

      YBBackupUtil.runYbBackupRestore("--keyspace", "ysql.yb2");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999", new Row(9999, 8.9));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2100, 2002.14));
      assertQuery(stmt, "SELECT h FROM test_tbl WHERE h=2000", new Row(2000));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999");
    }
  }

  @Test
  public void testYSQLColocatedBackupWithTableOidAlreadySet() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE DATABASE yb1 COLOCATED=TRUE");
    }
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb1").connect();
         Statement stmt = connection2.createStatement()) {
      // Create a table with a set table_oid.
      stmt.execute("SET yb_enable_create_with_table_oid = true");
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (table_oid = 123456)");
      stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES (1, 101, 3.14)");

      // Check that backup and restore works fine.
      YBBackupUtil.runYbBackupCreate("--keyspace", "ysql.yb1");
      YBBackupUtil.runYbBackupRestore("--keyspace", "ysql.yb2");
    }
    // Verify data is correct.
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));

      // Now try to do a backup/restore of the restored db.
      YBBackupUtil.runYbBackupCreate("--keyspace", "ysql.yb2");
      YBBackupUtil.runYbBackupRestore("--keyspace", "ysql.yb3");
    }
    // Verify data is correct.
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb3").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));
    }
    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb1");
      stmt.execute("DROP DATABASE yb2");
      stmt.execute("DROP DATABASE yb3");
    }
  }

  @Test
  public void testAlteredYSQLTableBackupWithNotNull() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl(id int)");

      stmt.execute("ALTER TABLE test_tbl ADD a int NOT NULL");
      stmt.execute("ALTER TABLE test_tbl ADD b int NULL");

      stmt.execute("INSERT INTO test_tbl(id, a, b) VALUES (1, 2, 3)");
      stmt.execute("INSERT INTO test_tbl(id, a) VALUES (2, 4)");

      runInvalidQuery(
          stmt, "INSERT INTO test_tbl(id, b) VALUES(3, 6)",
          "null value in column \"a\" violates not-null constraint");

      YBBackupUtil.runYbBackupCreate("--keyspace", "ysql.yugabyte");
      stmt.execute("INSERT INTO test_tbl (id, a, b) VALUES (9999, 9, 9)");

      YBBackupUtil.runYbBackupRestore("--keyspace", "ysql.yb2");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=1", new Row(1, 2, 3));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=2", new Row(2, 4, (Integer) null));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=3");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=9999", new Row(9999, 9, 9));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=1", new Row(1, 2, 3));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=2", new Row(2, 4, (Integer) null));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=3");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=9999");
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }
}
