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

package org.yb.pgsql;

import static org.yb.AssertionWrappers.*;

import java.sql.SQLException;
import java.sql.Statement;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;
import java.util.stream.Stream;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.yb.util.YBTestRunnerNonTsanOnly;

/**
 * Test cases involving updates on PK column (basically deletes an old row and inserts a new one).
 */
@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgUpdatePrimaryKey extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgUpdatePrimaryKey.class);

  //
  // Basic tests
  // Verify a single aspect of an update
  //

  @Test
  public void basic() throws Exception {
    String tableName = "update_pk_basic";
    String selectOrderedSql = "SELECT * FROM " + tableName + " ORDER BY id";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (id int PRIMARY KEY, i int)");

      // Initial inserts
      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1), (2, 2), (3, 3)");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      // Invalid (fails on different rows)
      runInvalidQuery(stmt, "UPDATE " + tableName + " SET id = id + 1", "duplicate key value");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(1, 1), new Row(2, 2), new Row(3, 3)));
      runInvalidQuery(stmt, "UPDATE " + tableName + " SET id = 5 - id", "duplicate key value");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      // Single-row-txn-like
      stmt.execute("UPDATE " + tableName + " SET id = 4 WHERE id = 1");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(2, 2), new Row(3, 3), new Row(4, 1)));

      // Update id referencing itself
      stmt.execute("UPDATE " + tableName + " SET id = id * 10");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(20, 2), new Row(30, 3), new Row(40, 1)));

      // Update id referencing other columns
      stmt.execute("UPDATE " + tableName + " SET id = id + i");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(22, 2), new Row(33, 3), new Row(41, 1)));

      // Update all columns
      stmt.execute("UPDATE " + tableName + " SET id = id * 10 + i, i = id * 10 + i");
      assertRowList(stmt, selectOrderedSql,
          Arrays.asList(new Row(222, 222), new Row(333, 333), new Row(411, 411)));
    }
  }

  @Test
  public void basicIndex() throws Exception {
    String tableName = "update_pk_basic_index";
    String indexName = tableName + "_idx";
    // We should account for all values we'll be setting for "i"
    String selectIndexSql = "SELECT * FROM " + tableName + " WHERE i IN (1, 2, 3, 11, 21, 31)";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (id int PRIMARY KEY, i int)");
      stmt.execute("CREATE INDEX " + indexName + " ON " + tableName + " (i)");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1), (2, 2), (3, 3)");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      stmt.execute("UPDATE " + tableName + " SET id = i * 10");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(10, 1), new Row(20, 2), new Row(30, 3)));

      runInvalidQuery(stmt, "UPDATE " + tableName + " SET id = 50 - id",
          "duplicate key value");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(10, 1), new Row(20, 2), new Row(30, 3)));

      stmt.execute("UPDATE " + tableName + " SET id = id + 1, i = i * 10 + 1");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(11, 11), new Row(21, 21), new Row(31, 31)));
    }
  }

  @Test
  public void basicUniqueIndex() throws Exception {
    String tableName = "update_pk_basic_index";
    String indexName = tableName + "_idx";
    // We should account for all values we'll be setting for "i"
    String selectIndexSql = "SELECT * FROM " + tableName + " WHERE i IN (1, 2, 3, 11, 21, 31)";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (id int PRIMARY KEY, i int)");
      stmt.execute("CREATE UNIQUE INDEX " + indexName + " ON " + tableName + " (i)");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1), (2, 2), (3, 3)");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      stmt.execute("UPDATE " + tableName + " SET id = i * 10");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(10, 1), new Row(20, 2), new Row(30, 3)));

      runInvalidQuery(stmt, "UPDATE " + tableName + " SET id = 50 - id",
          "duplicate key value");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(10, 1), new Row(20, 2), new Row(30, 3)));

      stmt.execute("UPDATE " + tableName + " SET id = id + 1, i = i * 10 + 1");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(11, 11), new Row(21, 21), new Row(31, 31)));

      // Uniqueness constraint is intact
      runInvalidQuery(stmt, "UPDATE " + tableName + " SET i = 21 WHERE i = 31",
          "duplicate key value");
      stmt.execute("UPDATE " + tableName + " SET i = 3 WHERE id = 11");
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql,
          asSet(new Row(11, 3), new Row(21, 21), new Row(31, 31)));
    }
  }

  @Test
  public void basicTriggers() throws Exception {
    String tableName = "update_pk_basic_triggers";
    String selectSql = "SELECT * FROM " + tableName;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (id int PRIMARY KEY, i int)");
      createTriggers(stmt, tableName, "id", "i");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1), (2, 2), (3, 3)");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));
      assertTriggers(stmt,
          3, 3, 0, 0, 0, 0,
          Arrays.asList());

      resetTriggers(stmt);
      runInvalidQuery(stmt, "UPDATE " + tableName + " SET id = 5 - id", "duplicate key value");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));
      assertTriggers(stmt,
          0, 0, 0, 0, 0, 0,
          Arrays.asList());

      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName + " SET id = id * 10, i = i * 10 WHERE id < 3");
      assertRowSet(stmt, selectSql,
          asSet(new Row(10, 10), new Row(20, 20), new Row(3, 3)));
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(1, 10, 1, 10),
              new Row(2, 20, 2, 20)));
    }
  }

  @Test
  public void basicForeignKey() throws Exception {
    String table1Name = "update_pk_basic_foreign_key_1";
    String table2Name = "update_pk_basic_foreign_key_2";
    String fkName = table1Name + "_fk";
    String selectSql1 = "SELECT * FROM " + table1Name;
    String selectSql2 = "SELECT * FROM " + table2Name;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + table2Name + " (id int PRIMARY KEY, i int)");
      stmt.execute("CREATE TABLE " + table1Name + " (id int PRIMARY KEY, t2_id int,"
          + " CONSTRAINT " + fkName + " FOREIGN KEY (t2_id) REFERENCES " + table2Name + "(id)"
          + " DEFERRABLE INITIALLY IMMEDIATE)");

      stmt.execute("INSERT INTO " + table2Name + " VALUES (1, 1), (2, 2), (3, 3)");
      stmt.execute("INSERT INTO " + table1Name + " VALUES (1, 1), (2, 2), (3, 3)");
      assertRowSet(stmt, selectSql1,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));
      assertRowSet(stmt, selectSql2,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      // Can't update due to non-exiting FK
      runInvalidQuery(stmt, "UPDATE " + table2Name + " SET id = 999 WHERE id = 1",
          "violates foreign key constraint");
      assertRowSet(stmt, selectSql1,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));
      assertRowSet(stmt, selectSql2,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      // Can't update due to non-exiting FK even when deferred
      stmt.execute("BEGIN");
      stmt.execute("SET CONSTRAINTS " + fkName + " DEFERRED");
      stmt.execute("UPDATE " + table2Name + " SET id = 999 WHERE id = 1");
      runInvalidQuery(stmt, "COMMIT",
          "violates foreign key constraint");
      assertRowSet(stmt, selectSql1,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));
      assertRowSet(stmt, selectSql2,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      // Can update when deferred and changed everywhere
      stmt.execute("BEGIN");
      stmt.execute("SET CONSTRAINTS " + fkName + " DEFERRED");
      stmt.execute("UPDATE " + table2Name + " SET id = 999 WHERE id = 1");
      stmt.execute("UPDATE " + table1Name + " SET t2_id = 999 WHERE t2_id = 1");
      stmt.execute("COMMIT");
      assertRowSet(stmt, selectSql1,
          asSet(new Row(1, 999), new Row(2, 2), new Row(3, 3)));
      assertRowSet(stmt, selectSql2,
          asSet(new Row(999, 1), new Row(2, 2), new Row(3, 3)));
    }
  }

  @Test
  public void basicOnConflict() throws Exception {
    String tableName = "update_pk_basic_on_conflict";
    String selectSql = "SELECT * FROM " + tableName;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (id int PRIMARY KEY, i int)");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1), (2, 2)");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2)));

      // PK stops us from inserting a batch containing duplicates
      runInvalidQuery(stmt, "INSERT INTO " + tableName + " VALUES (2, 20), (3, 30)",
          "duplicate key value");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2)));

      // ON CONFLICT DO NOTHING - only the last row can be inserted
      stmt.execute("INSERT INTO " + tableName + " VALUES (2, 20), (3, 30)"
          + " ON CONFLICT DO NOTHING");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 30)));

      // ON CONFLICT DO UPDATE - change the id but don't change i
      stmt.execute("INSERT INTO " + tableName + " VALUES (3, 31), (4, 41)"
          + " ON CONFLICT (id) DO UPDATE SET id = EXCLUDED.i");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(31, 30), new Row(4, 41)));

      // ON CONFLICT DO UPDATE - changing id to another existing id should still cause conflict
      runInvalidQuery(stmt, "INSERT INTO " + tableName + " VALUES (1, 999)"
          + " ON CONFLICT (id) DO UPDATE SET id = 2",
          "duplicate key value");
      assertRowSet(stmt, selectSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(31, 30), new Row(4, 41)));
    }
  }

  /** Update a primary key column on system catalog tables */
  @Test
  public void basicSystemTables() throws Exception {
    String tableName = "update_pk_basic_system_columns_1";
    String indexName = tableName + "_idx";
    String updatePgIndexOidFmt = "UPDATE pg_index SET indexrelid = %d WHERE indexrelid = %d";
    String countPgIndexByOidFmt = "SELECT COUNT(*) FROM pg_index WHERE indexrelid = %d";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (id int PRIMARY KEY, i int)");
      stmt.execute("CREATE INDEX " + indexName + " ON " + tableName + " (i)");

      long indexrelid = getSingleOid(stmt,
          "SELECT oid FROM pg_class WHERE relname = '" + indexName + "'");
      long newrelid = getSingleOid(stmt,
          "SELECT MAX(oid)::bigint + 1 FROM pg_class");

      // indexrelid is a primary key for pg_index, let's update it
      {
        assertOneRow(stmt, String.format(countPgIndexByOidFmt, indexrelid), 1);
        assertOneRow(stmt, String.format(countPgIndexByOidFmt, newrelid), 0);

        try {
          stmt.execute(String.format(updatePgIndexOidFmt, newrelid, indexrelid));

          assertOneRow(stmt, String.format(countPgIndexByOidFmt, indexrelid), 0);
          assertOneRow(stmt, String.format(countPgIndexByOidFmt, newrelid), 1);
        } finally {
          // Restore indexrelid, otherwise our cleanup code will explode
          try {
            stmt.execute(String.format(updatePgIndexOidFmt, indexrelid, newrelid));
          } catch (Exception ex) {
            LOG.error("Could not restore indexrelid!", ex);
          }
        }
      }

      // We still cannot update oid column though
      runInvalidQuery(stmt, "UPDATE pg_class SET oid = " + newrelid,
          "cannot assign to system column \"oid\"");
    }
  }

  //
  // Complex tests
  //

  @Test
  public void complexTwoRangeKeys() throws Exception {
    String tableName = "update_pk_complex_two_range_keys";
    String indexName = tableName + "_idx";
    String selectSql = "SELECT * FROM " + tableName;
    // We should account for all values we'll be setting for "i"
    String selectIndexSql = "SELECT * FROM " + tableName + " WHERE i IN (1, 2, 3, 11, 21, 31)";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (r1 int, r2 int, i int, "
          + "PRIMARY KEY (r1 ASC, r2 DESC))");
      stmt.execute("CREATE INDEX " + indexName + " ON " + tableName + " (i)");
      createTriggers(stmt, tableName, "r1", "r2", "i");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3)");
      Set<Row> expected = asSet(new Row(1, 1, 1), new Row(2, 2, 2), new Row(3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          3, 3, 0, 0, 0, 0,
          Arrays.asList());

      // Update just r1
      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName + " SET r1 = i * 10 WHERE i IN (1, 2)");
      expected = asSet(new Row(10, 1, 1), new Row(20, 2, 2), new Row(3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(1, 10, 1, 1, 1, 1),
              new Row(2, 20, 2, 2, 2, 2)));

      // Update just r2
      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName + " SET r2 = i * 10 WHERE i IN (1, 2)");
      expected = asSet(new Row(10, 10, 1), new Row(20, 20, 2), new Row(3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(10, 10, 1, 10, 1, 1),
              new Row(20, 20, 2, 20, 2, 2)));

      // Update all columns
      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName + " SET r1 = r1 + 1, r2 = r2 + 1, i = i * 10 + 1"
          + " WHERE i IN (1, 2)");
      expected = asSet(new Row(11, 11, 11), new Row(21, 21, 21), new Row(3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(10, 11, 10, 11, 1, 11),
              new Row(20, 21, 20, 21, 2, 21)));

      // ON CONFLICT - change keys
      //
      // (Note that AFTER INSERT does not trigger! This aligns with vanilla PG v11 behaviour)
      resetTriggers(stmt);
      stmt.execute("INSERT INTO " + tableName + " VALUES (3, 3, 31)"
          + " ON CONFLICT (r1, r2) DO UPDATE SET r1 = EXCLUDED.i, r2 = EXCLUDED.i");
      expected = asSet(new Row(11, 11, 11), new Row(21, 21, 21), new Row(31, 31, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          1, 0, 1, 1, 0, 0,
          Arrays.asList(
              new Row(3, 31, 3, 31, 3, 3)));
    }
  }

  @Test
  public void complexTwoHashOneRangeKeys() throws Exception {
    String tableName = "update_pk_complex_two_hash_one_range_keys";
    String indexName = tableName + "_idx";
    String selectSql = "SELECT * FROM " + tableName;
    // We should account for all values we'll be setting for "i"
    String selectIndexSql = "SELECT * FROM " + tableName + " WHERE i IN (1, 2, 3, 11, 21, 31)";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " (h1 int, h2 int, r int, i int, "
          + "PRIMARY KEY ((h1, h2) HASH, r ASC))");
      stmt.execute("CREATE INDEX " + indexName + " ON " + tableName + " (i)");
      createTriggers(stmt, tableName, "h1", "h2", "r", "i");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1, 1, 1), (2, 2, 2, 2), (3, 3, 3, 3)");
      Set<Row> expected = asSet(new Row(1, 1, 1, 1), new Row(2, 2, 2, 2), new Row(3, 3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          3, 3, 0, 0, 0, 0,
          Arrays.asList());

      // Update just h2
      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName + " SET h2 = i * 10 WHERE i IN (1, 2)");
      expected = asSet(new Row(1, 10, 1, 1), new Row(2, 20, 2, 2), new Row(3, 3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(1, 1, 1, 10, 1, 1, 1, 1),
              new Row(2, 2, 2, 20, 2, 2, 2, 2)));

      // Update just r
      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName + " SET r = i * 10 WHERE i IN (1, 2)");
      expected = asSet(new Row(1, 10, 10, 1), new Row(2, 20, 20, 2), new Row(3, 3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(1, 1, 10, 10, 1, 10, 1, 1),
              new Row(2, 2, 20, 20, 2, 20, 2, 2)));

      // Update all columns
      resetTriggers(stmt);
      stmt.execute("UPDATE " + tableName
          + " SET h1 = i * 10 + 1, h2 = h2 + 1, r = r + 1, i = i * 10 + 1"
          + " WHERE i IN (1, 2)");
      expected = asSet(new Row(11, 11, 11, 11), new Row(21, 21, 21, 21), new Row(3, 3, 3, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          0, 0, 2, 2, 0, 0,
          Arrays.asList(
              new Row(1, 11, 10, 11, 10, 11, 1, 11),
              new Row(2, 21, 20, 21, 20, 21, 2, 21)));

      // ON CONFLICT - change keys
      //
      // (Note that AFTER INSERT does not trigger! This aligns with vanilla PG v11 behaviour)
      resetTriggers(stmt);
      stmt.execute("INSERT INTO " + tableName + " VALUES (3, 3, 3, 31)"
          + " ON CONFLICT (h1, h2, r) DO UPDATE"
          + " SET h1 = EXCLUDED.i, h2 = EXCLUDED.i, r = EXCLUDED.i");
      expected = asSet(new Row(11, 11, 11, 11), new Row(21, 21, 21, 21), new Row(31, 31, 31, 3));
      assertRowSet(stmt, selectSql, expected);
      assertTrue(isIndexScan(stmt, selectIndexSql, indexName));
      assertRowSet(stmt, selectIndexSql, expected);
      assertTriggers(stmt,
          1, 0, 1, 1, 0, 0,
          Arrays.asList(
              new Row(3, 31, 3, 31, 3, 31, 3, 3)));
    }
  }

  /**
   * Tests non-trivial indexes together:
   * <ul>
   * <li>Index over one of PK columns
   * <li>Unique index with {@code INCLUDE}
   * </ul>
   */
  @Test
  public void complexIndexes() throws Exception {
    String tableName = "update_pk_complex_indexes";
    String pkColIndexName = tableName + "_pk_col_idx";
    String uniqIncludeIndexName = tableName + "_uniq_including_idx";
    // We should account for all values we'll be setting for columns
    String selectPkColIndexFullSql = //
        "SELECT * FROM " + tableName + " WHERE h2 IN (1, 2, 3, 11, 12, 13)";
    String selectPkColIndexOnlySql = //
        "SELECT h2 FROM " + tableName + " WHERE h2 IN (1, 2, 3, 11, 12, 13)";
    String selectIncludeIndexSql = //
        "SELECT h2, i FROM " + tableName + " WHERE i IN (1, 2, 3, 11, 12, 13)";
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE " + tableName + " ("
          + " h1 int, h2 int,"
          + " i int,"
          + " PRIMARY KEY ((h1, h2) HASH))");
      stmt.execute("CREATE INDEX " + pkColIndexName + " ON " + tableName + " (h2)");
      stmt.execute("CREATE UNIQUE INDEX " + uniqIncludeIndexName + " ON " + tableName + " (i)"
          + " INCLUDE (h2)");

      stmt.execute("INSERT INTO " + tableName + " VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3)");
      assertTrue(isIndexScan(stmt, selectPkColIndexFullSql, pkColIndexName));
      assertRowSet(stmt, selectPkColIndexFullSql,
          asSet(new Row(1, 1, 1), new Row(2, 2, 2), new Row(3, 3, 3)));
      assertTrue(isIndexOnlyScan(stmt, selectPkColIndexOnlySql, pkColIndexName));
      assertRowSet(stmt, selectPkColIndexOnlySql,
          asSet(new Row(1), new Row(2), new Row(3)));
      assertTrue(isIndexOnlyScan(stmt, selectIncludeIndexSql, uniqIncludeIndexName));
      assertRowSet(stmt, selectIncludeIndexSql,
          asSet(new Row(1, 1), new Row(2, 2), new Row(3, 3)));

      stmt.execute("UPDATE " + tableName + " SET h1 = h1 + 10, h2 = h2 + 10, i = i + 10");
      assertTrue(isIndexScan(stmt, selectPkColIndexFullSql, pkColIndexName));
      assertRowSet(stmt, selectPkColIndexFullSql,
          asSet(new Row(11, 11, 11), new Row(12, 12, 12), new Row(13, 13, 13)));
      assertTrue(isIndexOnlyScan(stmt, selectPkColIndexOnlySql, pkColIndexName));
      assertRowSet(stmt, selectPkColIndexOnlySql,
          asSet(new Row(11), new Row(12), new Row(13)));
      assertTrue(isIndexOnlyScan(stmt, selectIncludeIndexSql, uniqIncludeIndexName));
      assertRowSet(stmt, selectIncludeIndexSql,
          asSet(new Row(11, 11), new Row(12, 12), new Row(13, 13)));

      // Uniqueness constraint is intact
      runInvalidQuery(stmt, "UPDATE " + tableName + " SET i = 13 WHERE i = 12",
          "duplicate key value");
      assertTrue(isIndexScan(stmt, selectPkColIndexFullSql, pkColIndexName));
      assertRowSet(stmt, selectPkColIndexFullSql,
          asSet(new Row(11, 11, 11), new Row(12, 12, 12), new Row(13, 13, 13)));
      assertTrue(isIndexOnlyScan(stmt, selectPkColIndexOnlySql, pkColIndexName));
      assertRowSet(stmt, selectPkColIndexOnlySql,
          asSet(new Row(11), new Row(12), new Row(13)));
      assertTrue(isIndexOnlyScan(stmt, selectIncludeIndexSql, uniqIncludeIndexName));
      assertRowSet(stmt, selectIncludeIndexSql,
          asSet(new Row(11, 11), new Row(12, 12), new Row(13, 13)));
    }
  }

  //
  // Helpers
  //

  /**
   * Create a bunch of "FOR EACH ROW" triggers:
   * <ul>
   * <li>Create table "triggers_fired" and log BEFORE/AFTER (ROW) DML trigger event
   * <li>Create table "triggers_update_values" and log old and new column values for every UPDATE
   * </ul>
   */
  private void createTriggers(Statement stmt, String tableName, String... columns)
      throws Exception {
    // Table "triggers_fired" counts each trigger
    stmt.execute("CREATE TABLE triggers_fired (name text PRIMARY KEY, fired int)");
    stmt.execute("CREATE OR REPLACE FUNCTION log_trigger()"
        + "  RETURNS trigger AS $$"
        + "  BEGIN"
        + "    UPDATE triggers_fired SET fired = triggers_fired.fired + 1 WHERE name = TG_NAME;"
        + "  RETURN NEW;"
        + "  END;"
        + "  $$ LANGUAGE plpgsql;");

    for (String cond : Arrays.asList("BEFORE", "AFTER")) {
      for (String action : Arrays.asList("INSERT", "UPDATE", "DELETE")) {
        String name = (cond + "_" + action).toLowerCase();
        stmt.execute(String.format(
            "CREATE TRIGGER %s %s %s ON %s FOR EACH ROW EXECUTE PROCEDURE log_trigger()",
            name, cond, action, tableName));
        stmt.execute("INSERT INTO triggers_fired VALUES ('" + name + "', 0)");
      }
    }

    // Table "triggers_update_values" logs all updates
    StringBuilder sb = new StringBuilder("CREATE TABLE triggers_update_values (");
    boolean firstIter = true;
    for (String c : columns) {
      if (!firstIter) {
        sb.append(",");
      }
      sb.append(c + "_before int, " + c + "_after int");
      firstIter = false;
    }
    sb.append(")");
    stmt.execute(sb.toString());

    sb = new StringBuilder(
        "CREATE OR REPLACE FUNCTION log_update()"
            + "  RETURNS trigger AS $$"
            + "  BEGIN"
            + "    INSERT INTO triggers_update_values VALUES (");
    firstIter = true;
    for (String c : columns) {
      if (!firstIter) {
        sb.append(",");
      }
      sb.append("OLD." + c + ",");
      sb.append("NEW." + c);
      firstIter = false;
    }
    sb.append(");"
        + "  RETURN NEW;"
        + "  END;"
        + "  $$ LANGUAGE plpgsql;");
    stmt.execute(sb.toString());
    stmt.execute(
        "CREATE TRIGGER log_update_values AFTER UPDATE ON " + tableName
            + " FOR EACH ROW EXECUTE PROCEDURE log_update()");
  }

  private void resetTriggers(Statement stmt) throws Exception {
    stmt.execute("UPDATE triggers_fired SET fired = 0");
    stmt.execute("TRUNCATE TABLE triggers_update_values");
  }

  /**
   * Assert that triggers were firing as expected since last call to {@code resetTriggers}.
   *
   * @param beforeAfterColumnValues
   *          Rows logged by update, each row has twice as much columns as the table. Columns order
   *          is the same as was passed to {@code createTriggers}, for each column "before" value is
   *          followed by "after" value.
   */
  private void assertTriggers(Statement stmt,
      int beforeInserts, int afterInserts,
      int beforeUpdates, int afterUpdates,
      int beforeDeletes, int afterDeletes,
      List<Row> beforeAfterColumnValues) throws Exception {
    assertOneRow(stmt,
        "SELECT fired FROM triggers_fired WHERE name = 'before_insert'", beforeInserts);
    assertOneRow(stmt,
        "SELECT fired FROM triggers_fired WHERE name =  'after_insert'", afterInserts);
    assertOneRow(stmt,
        "SELECT fired FROM triggers_fired WHERE name = 'before_update'", beforeUpdates);
    assertOneRow(stmt,
        "SELECT fired FROM triggers_fired WHERE name =  'after_update'", afterUpdates);
    assertOneRow(stmt,
        "SELECT fired FROM triggers_fired WHERE name = 'before_delete'", beforeDeletes);
    assertOneRow(stmt,
        "SELECT fired FROM triggers_fired WHERE name =  'after_delete'", afterDeletes);

    assertOneRow(stmt, "SELECT COUNT(*) FROM triggers_update_values",
        beforeAfterColumnValues.size());
    assertRowSet(stmt, "SELECT * FROM triggers_update_values",
        new HashSet<>(beforeAfterColumnValues));
  }

  private long getSingleOid(Statement stmt, String query) throws SQLException {
    return getSingleRow(stmt.executeQuery(query)).getLong(0);
  }

  @SafeVarargs
  private final <T> Set<T> asSet(T... ts) {
    return Stream.of(ts).collect(Collectors.toSet());
  }
}
