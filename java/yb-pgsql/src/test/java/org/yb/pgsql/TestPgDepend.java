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

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgDepend extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgDepend.class);

  private static final int PG_TYPE_OID = 1247;
  private static final int PG_CLASS_OID = 1259;
  private static final int PG_AUTH_ID_OID = 1260;
  private static final int PG_NAMESPACE_OID = 2615;
  private static final int PUBLIC_NAMESPACE_OID = 2200;

  @Test
  public void testPgDependInsertion() throws SQLException {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable(statement, "test");

      // Create an Index for the table.
      statement.execute("CREATE UNIQUE INDEX test_h on test(h)");

      // Get the OID of the table.
      ResultSet rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test'");
      rs.next();
      int oidTable = rs.getInt("oid");

      // Get the OID of the index.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_h'");
      rs.next();
      int oidIndex = rs.getInt("oid");

      // Check that we have inserted the dependency into pg_depend.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidIndex + "AND refobjid=" + oidTable);
      assertTrue(rs.next());
    }
  }

  @Test
  public void testTableWithIndexDeletion() throws SQLException {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable(statement, "test");

      // Create an Index for the table.
      statement.execute("CREATE INDEX test_h on test(h)");

      // Get the OID of the table.
      ResultSet rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test'");
      rs.next();
      int oidTable = rs.getInt("oid");

      // Get the OID of the index.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_h'");
      rs.next();
      int oidIndex = rs.getInt("oid");

      // Check that we have inserted into pg_index.
      rs = statement.executeQuery("SELECT * FROM pg_index "
                                  + "WHERE indexrelid=" + oidIndex + "AND indrelid=" + oidTable);
      assertTrue(rs.next());

      statement.execute("DROP TABLE test");

      // Check that we have deleted the dependency in pg_depend.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidIndex + "and refobjid=" + oidTable);
      assertFalse(rs.next());

      // Check that we have deleted the index's entry in pg_index.
      rs = statement.executeQuery("SELECT * FROM pg_index "
                                  + "WHERE indexrelid=" + oidIndex + "AND indrelid=" + oidTable);
      assertFalse(rs.next());

      // Check that we have deleted the index's entry in pg_class.
      rs = statement.executeQuery("SELECT * FROM pg_class WHERE relname = 'test_h'");
      assertFalse(rs.next());

      // Check that we can create a new index with the same name.
      createSimpleTable(statement, "test");
      statement.execute("CREATE INDEX test_h on test(h)");
    }
  }

  @Test
  public void testTableWithSequenceDeletion() throws SQLException {
    try (Statement statement = connection.createStatement()) {
      // Create a table with serial type
      statement.execute("CREATE TABLE test (d SERIAL)");

      // Get the OID of the table.
      ResultSet rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname='test'");
      rs.next();
      int oidTable = rs.getInt("oid");

      // Get the parent sequence's oid.
      rs = statement.executeQuery("SELECT objid FROM pg_depend "
                                  + "WHERE classid=" + PG_CLASS_OID + "AND refobjid=" + oidTable);

      rs.next();
      int oidSequence = rs.getInt("objid");

      statement.execute("DROP TABLE test");

      // Check that we have deleted the dependency between the sequence and the table.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidSequence + "AND refobjid=" + oidTable);
      assertFalse(rs.next());

      // Check that we have deleted the sequence's entry from pg_class.
      rs = statement.executeQuery("SELECT * FROM pg_class WHERE oid=" + oidSequence);
      assertFalse(rs.next());

      // Check that we have deleted the sequence's entry from pg_sequence.
      rs = statement.executeQuery("SELECT * FROM pg_sequence WHERE seqrelid=" + oidSequence);
      assertFalse(rs.next());
    }
  }

  @Test
  public void testTableWithViewDeletionWithCascade() throws SQLException {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable(statement, "test");

      // Create an index for the table.
      statement.execute("CREATE INDEX test_h on test(h)");

      // Create a view on the table.
      statement.execute("CREATE VIEW test_view AS SELECT * FROM test");

      // Get the OID of the table.
      ResultSet rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test'");
      rs.next();
      int oidTable = rs.getInt("oid");

      // Get the OID of the index.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_h'");
      rs.next();
      int oidIndex = rs.getInt("oid");

      // Get the OID of the view.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_view'");
      rs.next();
      int oidView = rs.getInt("oid");

      // Test dropping the table (without CASCADE). -- expecting an error
      runInvalidQuery(statement,
          "DROP TABLE test",
          "cannot drop table test because other objects depend on it");

      // Test dropping the table (with CASCADE).
      statement.execute("DROP TABLE test CASCADE");

      // Check that we have deleted the view-table dependency in pg_depend.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidView + "and refobjid=" + oidTable);
      assertFalse(rs.next());

      // Check that we have deleted the index-table dependency in pg_depend.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidIndex + "and refobjid=" + oidTable);
      assertFalse(rs.next());

      // Check that we have deleted the views's entry in pg_class.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_view'");
      assertFalse(rs.next());

      // Check that we can create a new view with the same name.
      createSimpleTable(statement, "test");
      statement.execute("CREATE VIEW test_view AS SELECT * FROM test");
    }
  }

  @Test
  public void testViewDeletionWithCascade() throws SQLException {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable(statement, "test");

      // Create a view on the table.
      statement.execute("CREATE VIEW test_view AS SELECT * FROM test");

      // Create a view on the view.
      statement.execute("CREATE VIEW test_view_view AS SELECT * FROM test_view");

      // Get the OID of test_view.
      ResultSet rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_view'");
      rs.next();
      int oidView1 = rs.getInt("oid");

      // Get the OID of test_view_view;
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_view_view'");
      rs.next();
      int oidView2 = rs.getInt("oid");

      // Test dropping test_view (without CASCADE). -- expecting an error
      runInvalidQuery(statement,
          "DROP VIEW test_view",
          "cannot drop view test_view because other objects depend on it");

      // Test dropping test_view (with CASCADE).
      statement.execute("DROP VIEW test_view CASCADE");

      // Check that we have deleted the view-view dependency in pg_depend.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidView2 + " AND refobjid=" + oidView1);
      assertFalse(rs.next());

      // Check that we have deleted the test_view_view's entry in pg_class.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test_view_view'");
      assertFalse(rs.next());
    }
  }

  @Test
  public void testSequenceDeletionWithCascade() throws SQLException {
    try (Statement statement = connection.createStatement()) {

      // Create a sequence.
      statement.execute("CREATE SEQUENCE seq_test START 101");

      // Create a table with a column that depends on the sequence.
      statement.execute("CREATE TABLE test (a int, b int DEFAULT nextval('seq_test'))");

      // Get the OID of the sequence.
      ResultSet rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname='seq_test'");
      rs.next();
      int oidSequence = rs.getInt("oid");

      // Get the OID of the table.
      rs = statement.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test'");
      rs.next();
      int oidTable = rs.getInt("oid");

      // Get the OID of the table column type
      rs = statement.executeQuery("SELECT objid FROM pg_depend WHERE " +
                                      "classid=" + PG_TYPE_OID + " AND refobjid=" + oidSequence);
      rs.next();
      int oidColType = rs.getInt("objid");

      // Test dropping the sequence (without CASCADE). -- expecting an error
      runInvalidQuery(statement, "DROP SEQUENCE seq_test", "depends on sequence seq_test");

      // Test dropping the sequence (with CASCADE).
      statement.execute("DROP SEQUENCE seq_test CASCADE");

      // Check that we have deleted the table column to sequence type dependency in pg_depend.
      rs = statement.executeQuery("SELECT * FROM pg_depend "
                                  + "WHERE objid=" + oidColType + " AND refobjid=" + oidSequence);
      assertFalse(rs.next());
    }
  }

  @Test
  public void testPinnedSystemTables() throws SQLException {
    try (Statement statement = connection.createStatement()) {

      // Check that we cannot drop system tables.
      runInvalidQuery(statement, "DROP TABLE pg_class", "permission denied");
      runInvalidQuery(statement, "DROP TABLE pg_database", "permission denied");

      // Check that there are pinned entries in pg_depend.
      ResultSet rs = statement.executeQuery("SELECT count(*) AS num_pinned FROM pg_depend " +
                                                "WHERE deptype = 'p'");
      assertTrue(rs.next());
      assertTrue(rs.getInt("num_pinned") > 50);


      // Check that there are pinned entries in pg_shdepend.
      rs = statement.executeQuery("SELECT count(*) AS num_pinned FROM pg_shdepend " +
                                                "WHERE deptype = 'p'");
      assertTrue(rs.next());
      assertTrue(rs.getInt("num_pinned") > 3);


      // Create a simple table and get its oid.
      statement.execute("CREATE TABLE pin_test(a int PRIMARY KEY, b int)");
      rs = statement.executeQuery("SELECT oid FROM pg_class where relname = 'pin_test'");
      assertTrue(rs.next());
      int tableOid = rs.getInt("oid");

      // Check that it does not add superfluous entries in pg_depend.
      // - The public namespace is not pinned so that is the only expected entry.
      rs = statement.executeQuery("SELECT * FROM pg_depend where objid = " + tableOid);
      assertTrue(rs.next());
      assertEquals(PG_NAMESPACE_OID, rs.getInt("refclassid"));
      assertEquals(PUBLIC_NAMESPACE_OID, rs.getInt("refobjid"));
      assertFalse(rs.next());

      // Check that it does not add superfluous entries in pg_shdepend.
      // - The test user (TEST_PG_USER) is not pinned so that is the only expected entry.
      rs = statement.executeQuery("SELECT oid FROM pg_authid WHERE " +
                                      "rolname = '" + TEST_PG_USER + "'");
      assertTrue(rs.next());
      int testUserOid = rs.getInt("oid");
      rs = statement.executeQuery("SELECT * FROM pg_shdepend where objid = " + tableOid);
      assertTrue(rs.next());
      assertEquals(PG_AUTH_ID_OID, rs.getInt("refclassid"));
      assertEquals(testUserOid, rs.getInt("refobjid"));
      assertFalse(rs.next());
    }
  }
}
