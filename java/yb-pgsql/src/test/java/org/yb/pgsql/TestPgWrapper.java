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

import org.junit.Ignore;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgWrapper extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgWrapper.class);

  @Test
  public void testSimpleDDL() throws Exception {
    try (Statement statement = connection.createStatement()) {

      // -------------------------------------------------------------------------------------------
      // Test Database

      statement.execute("CREATE DATABASE dbtest");

      // Database already exists.
      runInvalidQuery(statement, "CREATE DATABASE dbtest", "already exists");

      // Drop database.
      statement.execute("DROP DATABASE dbtest");

      // -------------------------------------------------------------------------------------------
      // Test Table

      createSimpleTable("test", "v");

      // Test table with out of order primary key columns.
      statement.execute("CREATE TABLE test2(v text, r float, h bigint, PRIMARY KEY (h, r))");

      // Table already exists.
      runInvalidQuery(
          statement,
          getSimpleTableCreationStatement("test", "v", PartitioningMode.HASH),
          "already exists");

      // Drop tables.
      statement.execute("DROP TABLE test");
      statement.execute("DROP TABLE test2");

      // Table does not exist.
      runInvalidQuery(statement, "DROP TABLE test3", "does not exist");
    }
  }

  @Test
  public void testDatatypes() throws SQLException {
    LOG.info("START testDatatypes");
    String[] supported_types = {"smallint", "int", "bigint", "real", "double precision", "text"};

    try (Statement statement = connection.createStatement()) {

      StringBuilder sb = new StringBuilder();
      sb.append("CREATE TABLE test_types(");
      // Checking every type is allowed for a column.
      for (int i = 0; i < supported_types.length; i++) {
        sb.append("c").append(i).append(" ");
        sb.append(supported_types[i]);
        sb.append(", ");
      }

      sb.append("PRIMARY KEY(");
      // Checking every type is allowed as primary key.
      for (int i = 0; i < supported_types.length; i++) {
        if (i > 0) sb.append(", ");
        sb.append("c").append(i);
      }
      sb.append("))");

      String sql = sb.toString();
      LOG.info("Creating table. SQL statement: " + sql);
      statement.execute(sb.toString());
      LOG.info("END testDatatypes");
    }
  }

  @Test
  public void testSimpleDML() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_dml(h bigint, r float, v text, PRIMARY KEY (h, r))");

      statement.execute("INSERT INTO test_dml(h, r, v) VALUES (1, 2.5, 'abc')");
      statement.execute("INSERT INTO test_dml(h, r, v) VALUES (1, 3.5, 'def')");

      try (ResultSet rs = statement.executeQuery("SELECT h, r, v FROM test_dml WHERE h = 1")) {

        assertTrue(rs.next());
        assertEquals(1, rs.getLong("h"));
        assertEquals(2.5, rs.getDouble("r"));
        assertEquals("abc", rs.getString("v"));

        assertTrue(rs.next());
        assertEquals(1, rs.getLong("h"));
        assertEquals(3.5, rs.getDouble("r"));
        assertEquals("def", rs.getString("v"));

        assertFalse(rs.next());
      }
    }
  }

  /**
   * ENG-4044: check that we handle text keys of arbitrary length correctly.
   */
  @Test
  public void testSelectByTextKey() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE textkeytable (k text primary key, v1 text, v2 int)");
      StringBuilder kBuilder = new StringBuilder();
      Random r = new Random(349871827L);
      List<String> ks = new ArrayList<>();
      List<String> v1s = new ArrayList<>();
      List<Integer> v2s = new ArrayList<>();

      for (int i = 0; i < 100; i++) {
        String k = kBuilder.toString();
        String v1 = "v1_" + i;
        int v2 = i * i;
        ks.add(k);
        v1s.add(v1);
        v2s.add(v2);
        statement.execute(
            String.format(
                "INSERT INTO textkeytable (k, v1, v2) VALUES ('%s', '%s', %d)",
                k, v1, v2));

        int iSelect = r.nextInt(i + 1);
        String kSelect = ks.get(iSelect);
        ResultSet rs = statement.executeQuery(
            String.format("SELECT * FROM textkeytable WHERE k = '%s'", kSelect));
        assertTrue(rs.next());
        assertEquals(kSelect, rs.getString("k"));
        assertEquals(v1s.get(iSelect), rs.getString("v1"));
        assertEquals(v2s.get(iSelect).intValue(), rs.getInt("v2"));

        // Check varying key lengths, starting from zero (that's why we're appending here).
        kBuilder.append("012345789_i=" + i + "_");
      }
    }
  }

  @Test
  public void testDefaultValues() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE testdefaultvaluetable " +
          "(k int primary key, v1 text default 'abc', v2 int default 1, v3 int)");
      for (int i = 0; i < 100; i++) {
        statement.execute(
            String.format("INSERT INTO testdefaultvaluetable(k, v3) VALUES(%d, %d)", i, i));
      }
      ResultSet rs = statement.executeQuery("SELECT * FROM testdefaultvaluetable ORDER BY k ASC");
      for (int i = 0; i < 100; i++) {
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("k"));
        assertEquals("abc", rs.getString("v1"));
        assertEquals(1, rs.getInt("v2"));
        assertEquals(i, rs.getInt("v3"));
      }
    }
    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      statement.execute("INSERT INTO testdefaultvaluetable(k, v3) VALUES(1000, 3)");
      ResultSet rs = statement.executeQuery("SELECT * FROM testdefaultvaluetable WHERE k = 1000");
      assertTrue(rs.next());
      assertEquals(1000, rs.getInt("k"));
      assertEquals(3, rs.getInt("v3"));
      assertEquals("abc", rs.getString("v1"));
      assertEquals(1, rs.getInt("v2"));
    }
  }

  @Test
  public void testPsqlCommands() throws Exception {
    // Check that the internal queries for basic psql commands succeed.
    // Do not check the actual output as it might vary depending on the database state.
    try (Statement statement = connection.createStatement()) {

      // List Databases: '\l'
      statement.executeQuery(
          "SELECT d.datname as \"Name\",\n" +
              "       pg_catalog.pg_get_userbyid(d.datdba) as \"Owner\",\n" +
              "       pg_catalog.pg_encoding_to_char(d.encoding) as \"Encoding\",\n" +
              "       d.datcollate as \"Collate\",\n" +
              "       d.datctype as \"Ctype\",\n" +
              "       pg_catalog.array_to_string(d.datacl, E'\\n') AS \"Access privileges\"\n" +
              "FROM pg_catalog.pg_database d\n" +
              "ORDER BY 1;"
      );

      // List Databases: '\dt'
      statement.executeQuery(
          "SELECT n.nspname as \"Schema\",\n" +
              "  c.relname as \"Name\",\n" +
              "  CASE c.relkind WHEN 'r' THEN 'table' WHEN 'v' THEN 'view' WHEN 'm' THEN " +
              "      'materialized view' WHEN 'i' THEN 'index' WHEN 'S' THEN 'sequence' WHEN 's'" +
              "      THEN 'special' WHEN 'f' THEN 'foreign table' END as \"Type\",\n" +
              "  pg_catalog.pg_get_userbyid(c.relowner) as \"Owner\"\n" +
              "FROM pg_catalog.pg_class c\n" +
              "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n" +
              "WHERE c.relkind IN ('r','')\n" +
              "      AND n.nspname <> 'pg_catalog'\n" +
              "      AND n.nspname <> 'information_schema'\n" +
              "      AND n.nspname !~ '^pg_toast'\n" +
              "  AND pg_catalog.pg_table_is_visible(c.oid)\n" +
              "ORDER BY 1,2;"
      );

      // Describe table: '\d table_name' (example with pg_class, oid 1259).
      statement.executeQuery(
          "SELECT a.attname,\n" +
              "  pg_catalog.format_type(a.atttypid, a.atttypmod),\n" +
              "  (SELECT substring(pg_catalog.pg_get_expr(d.adbin, d.adrelid) for 128)\n" +
              "   FROM pg_catalog.pg_attrdef d\n" +
              "   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef),\n" +
              "  a.attnotnull, a.attnum,\n" +
              "  (SELECT c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type t\n" +
              "   WHERE c.oid = a.attcollation AND t.oid = a.atttypid AND " +
              "         a.attcollation <> t.typcollation) AS attcollation,\n" +
              "  NULL AS indexdef,\n" +
              "  NULL AS attfdwoptions\n" +
              "FROM pg_catalog.pg_attribute a\n" +
              "WHERE a.attrelid = '1259' AND a.attnum > 0 AND NOT a.attisdropped\n" +
              "ORDER BY a.attnum;"
      );
    }
  }

  @Test
  public void testNotNullConstraint() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute(
          "CREATE TABLE testnotnullconstraint (k int primary key, v1 text not null)");
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("null value in column \"v1\" violates not-null constraint");
      statement.execute("INSERT INTO testnotnullconstraint(k, v1) VALUES (1, null)");
    }
  }

  @Test
  public void testCheckConstraint() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute(
          "CREATE TABLE testcheckconstraint (k int primary key, v1 int CHECK (v1 > 9));");

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("new row for relation \"testcheckconstraint\" violates check " +
          "constraint \"testcheckconstraint_v1_check\"");
      statement.execute("INSERT INTO testcheckconstraint(k, v1) VALUES (1, -1)");
    }
  }
}
