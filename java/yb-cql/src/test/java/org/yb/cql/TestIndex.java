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
package org.yb.cql;

import java.util.*;

import org.junit.BeforeClass;
import org.junit.Test;

import com.datastax.driver.core.BatchStatement;
import com.datastax.driver.core.BoundStatement;
import com.datastax.driver.core.ColumnDefinitions;
import com.datastax.driver.core.ColumnDefinitions.Definition;
import com.datastax.driver.core.PreparedStatement;
import com.datastax.driver.core.ResultSetFuture;
import com.datastax.driver.core.Row;
import com.datastax.driver.core.Session;
import com.datastax.driver.core.SimpleStatement;
import com.datastax.driver.core.TableMetadata;
import com.datastax.driver.core.exceptions.InvalidQueryException;

import org.yb.minicluster.BaseMiniClusterTest;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.RocksDBMetrics;
import org.yb.util.SanitizerUtil;
import org.yb.util.TableProperties;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertNotNull;
import static org.yb.AssertionWrappers.assertNull;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestIndex extends BaseCQLTest {

  @Override
  public int getTestMethodTimeoutSec() {
    // Usual time for a test ~90 seconds. But can be much more on Jenkins.
    return super.getTestMethodTimeoutSec()*10;
  }

  @BeforeClass
  public static void SetUpBeforeClass() throws Exception {
    BaseMiniClusterTest.tserverArgs.add("--allow_index_table_read_write");
    BaseMiniClusterTest.tserverArgs.add(
        "--index_backfill_upperbound_for_user_enforced_txn_duration_ms=1000");
    BaseMiniClusterTest.tserverArgs.add(
        "--index_backfill_wait_for_old_txns_ms=100");
    BaseCQLTest.setUpBeforeClass();
  }

  @Test
  public void testReadWriteIndexTable() throws Exception {
    // TODO: remove this test case and the "allow_index_table_read_write" flag above after
    // secondary index feature is complete.
    session.execute("create table test_index (h int, r1 int, r2 int, c int, " +
                    "primary key ((h), r1, r2)) with transactions = { 'enabled' : true};");
    session.execute("create index i on test_index (h, r2, r1) include (c);");

    session.execute("insert into test_index (h, r1, r2, c) values (1, 2, 3, 4);");
    session.execute("insert into i (\"C$_h\", \"C$_r2\", \"C$_r1\", \"C$_c\")" +
                    " values (1, 3, 2, 4);");
    assertQuery("select * from test_index;", "Row[1, 2, 3, 4]");
    assertQuery("select * from i;", "Row[1, 2, 3, 4]");
  }

  @Test
  public void testCreateIndex() throws Exception {

    // Create test table.
    session.execute("create table test_create_index " +
                    "(h1 int, h2 text, r1 int, r2 text, " +
                    "c1 int, c2 text, c3 decimal, c4 timestamp, c5 boolean, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");

    // Create test indexes with range and non-primary-key columns.
    session.execute("create index i1 on test_create_index (r1, r2) include (c1, c4);");
    session.execute("create index i2 on test_create_index (c4) include (c1, c2);");
    session.execute("create index i4 on test_create_index (c5) include (c4);");
    session.execute("create index i5 on test_create_index (c1, c5) include (c2, c3);");

    // Wait to ensure the partitions metadata was updated.
    // Schema change should trigger a refresh but playing it safe in case debouncer will delay it.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    // Verify the indexes.
    TableMetadata table = cluster.getMetadata().getKeyspace(DEFAULT_TEST_KEYSPACE)
                          .getTable("test_create_index");
    assertEquals("CREATE INDEX i1 ON cql_test_keyspace.test_create_index (r1, r2, h1, h2);",
                 table.getIndex("i1").asCQLQuery());
    assertEquals("CREATE INDEX i2 ON cql_test_keyspace.test_create_index (c4, h1, h2, r1, r2);",
                 table.getIndex("i2").asCQLQuery());
    assertEquals("CREATE INDEX i4 ON cql_test_keyspace.test_create_index (c5, h1, h2, r1, r2);",
                 table.getIndex("i4").asCQLQuery());
    assertEquals("CREATE INDEX i5 ON cql_test_keyspace.test_create_index (c1, c5, h1, h2, r1, r2);",
                 table.getIndex("i5").asCQLQuery());

    // Verify the covering columns.
    assertIndexOptions("test_create_index", "i1", "r1, r2, h1, h2", "c1, c4");
    assertIndexOptions("test_create_index", "i2", "c4, h1, h2, r1, r2", "c1, c2");
    assertIndexOptions("test_create_index", "i4", "c5, h1, h2, r1, r2", "c4");
    assertIndexOptions("test_create_index", "i5", "c1, c5, h1, h2, r1, r2", "c2, c3");

    // Test retrieving non-existent index.
    assertNull(table.getIndex("i3"));

    runInvalidStmt("CREATE INDEX i1 ON cql_test_keyspace.test_create_index (r1) where r1 = 5;");

    // Test create index with duplicate name.
    try {
      session.execute("create index i1 on test_create_index (r1) include (c1);");
      fail("Duplicate index created");
    } catch (InvalidQueryException e) {
      LOG.info("Expected exception " + e.getMessage());
    }

    // Test create index on non-existent table.
    try {
      session.execute("create index i3 on non_existent_table (k) include (v);");
      fail("Index on non-existent table created");
    } catch (InvalidQueryException e) {
      LOG.info("Expected exception " + e.getMessage());
    }

    // Test create index if not exists. Verify i1 is still the same.
    session.execute("create index if not exists i1 on test_create_index (r1) include (c1);");
    assertIndexOptions("test_create_index", "i1", "r1, r2, h1, h2", "c1, c4");

    // Create another test table.
    session.execute("create table test_create_index_2 " +
                    "(h1 int, h2 text, r1 int, r2 text, " +
                    "c1 int, c2 text, c3 decimal, c4 timestamp, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");

    // Test create index by the same name on another table.
    try {
      session.execute("create index i1 on test_create_index_2 (r1, r2) include (c1, c4);");
      fail("Index by the same name created on another table");
    } catch (InvalidQueryException e) {
      LOG.info("Expected exception " + e.getMessage());
    }
  }

  @Test
  public void testCreateIndexWithClusteringOrder() throws Exception {

    // Create test table.
    session.execute("create table test_clustering_index " +
                    "(h1 int, h2 text, r1 int, r2 text, " +
                    "c1 int, c2 text, c3 decimal, c4 timestamp, c5 uuid, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");

    // Create test indexes with range and non-primary-key columns.
    session.execute("create index i1 on test_clustering_index (r1, r2, c1, c2) " +
                    "include (c3, c4) with clustering order by (r2 desc, c1 asc, c2 desc);");
    session.execute("create index i2 on test_clustering_index ((c1, c2), c3, c4) " +
                    "include (c5) with clustering order by (c3 asc, c4 desc);");

    // Wait to ensure the partitions metadata was updated.
    // Schema change should trigger a refresh but playing it safe in case debouncer will delay it.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    // Verify the indexes.
    TableMetadata table = cluster.getMetadata().getKeyspace(DEFAULT_TEST_KEYSPACE)
                          .getTable("test_clustering_index");
    assertEquals("CREATE INDEX i1 ON cql_test_keyspace.test_clustering_index " +
                 "(r1, r2, c1, c2, h1, h2);", table.getIndex("i1").asCQLQuery());
    assertEquals("CREATE INDEX i2 ON cql_test_keyspace.test_clustering_index " +
                 "((c1, c2), c3, c4, h1, h2, r1, r2);", table.getIndex("i2").asCQLQuery());

    assertIndexColumns("i1",
                       "Row[r1, none, partition_key, 0, int]" +
                       "Row[r2, desc, clustering, 0, text]" +
                       "Row[c1, asc, clustering, 1, int]" +
                       "Row[c2, desc, clustering, 2, text]" +
                       "Row[h1, asc, clustering, 3, int]" +
                       "Row[h2, asc, clustering, 4, text]" +
                       "Row[c3, none, regular, -1, decimal]" +
                       "Row[c4, none, regular, -1, timestamp]");
    assertIndexColumns("i2",
                       "Row[c1, none, partition_key, 0, int]" +
                       "Row[c2, none, partition_key, 1, text]" +
                       "Row[c3, asc, clustering, 0, decimal]" +
                       "Row[c4, desc, clustering, 1, timestamp]" +
                       "Row[h1, asc, clustering, 2, int]" +
                       "Row[h2, asc, clustering, 3, text]" +
                       "Row[r1, asc, clustering, 4, int]" +
                       "Row[r2, asc, clustering, 5, text]" +
                       "Row[c5, none, regular, -1, uuid]");
  }

  @Test
  public void testDropIndex() throws Exception {

    // Create test table.
    session.execute("create table test_drop_index " +
                    "(h1 int, h2 text, r1 int, r2 text, " +
                    "c1 int, c2 text, c3 decimal, c4 timestamp, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");

    // Create test index.
    session.execute("create index i1 on test_drop_index (r1, r2) include (c1, c2);");

    // Wait to ensure the partitions metadata was updated.
    // Schema change should trigger a refresh but playing it safe in case debouncer will delay it.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    // Verify the index.
    TableMetadata table = cluster.getMetadata().getKeyspace(DEFAULT_TEST_KEYSPACE)
                          .getTable("test_drop_index");
    assertEquals("CREATE INDEX i1 ON cql_test_keyspace.test_drop_index (r1, r2, h1, h2);",
                 table.getIndex("i1").asCQLQuery());
    assertIndexOptions("test_drop_index", "i1", "r1, r2, h1, h2", "c1, c2");

    // Drop test index.
    session.execute("drop index i1;");

    // Wait to ensure the partitions metadata was updated.
    // Schema change should trigger a refresh but playing it safe in case debouncer will delay it.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    table = cluster.getMetadata().getKeyspace(DEFAULT_TEST_KEYSPACE).getTable("test_drop_index");
    assertNull(table.getIndex("i1"));
    assertNull(session.execute("select options from system_schema.indexes " +
                               "where keyspace_name = ? and table_name = ? and index_name = ?",
                               DEFAULT_TEST_KEYSPACE, "test_drop_index", "i1").one());

    // Create another test index by the same name. Verify new index is created.
    session.execute("create index i1 on test_drop_index (c1, c2) include (c3, c4);");

    // Wait to ensure the partitions metadata was updated.
    // Schema change should trigger a refresh but playing it safe in case debouncer will delay it.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    table = cluster.getMetadata().getKeyspace(DEFAULT_TEST_KEYSPACE).getTable("test_drop_index");
    assertEquals("CREATE INDEX i1 ON cql_test_keyspace.test_drop_index (c1, c2, h1, h2, r1, r2);",
                 table.getIndex("i1").asCQLQuery());
    assertIndexOptions("test_drop_index", "i1", "c1, c2, h1, h2, r1, r2", "c3, c4");
  }

  @Test
  public void testDropTableCascade() throws Exception {

    // Create test table.
    session.execute("create table test_drop_cascade " +
                    "(h1 int, h2 text, r1 int, r2 text, " +
                    "c1 int, c2 text, c3 decimal, c4 timestamp, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");

    // Create test index.
    session.execute("create index i1 on test_drop_cascade (r1, r2) include (c1, c2);");
    session.execute("create index i2 on test_drop_cascade (c4) include (c1);");
    assertIndexOptions("test_drop_cascade", "i1", "r1, r2, h1, h2", "c1, c2");
    assertIndexOptions("test_drop_cascade", "i2", "c4, h1, h2, r1, r2", "c1");

    // Drop test table. Verify the index is cascade-deleted.
    session.execute("drop table test_drop_cascade;");
    assertNull(session.execute("select options from system_schema.indexes " +
                               "where keyspace_name = ? and table_name = ? and index_name = ?",
                               DEFAULT_TEST_KEYSPACE, "test_drop_cascade", "i1").one());
    assertNull(session.execute("select options from system_schema.indexes " +
                               "where keyspace_name = ? and table_name = ? and index_name = ?",
                               DEFAULT_TEST_KEYSPACE, "test_drop_cascade", "i2").one());
  }

  @Test
  public void testRecreateTable() throws Exception {
    LOG.info("Start test: " + getCurrentTestMethodName());
    session.execute("create keyspace test_ks;");
    session.execute("use test_ks;");

    // By default the driver will execute commands on different TSes.
    // Each CQL Server in TS has local Table info cache.
    // The following test simulates a 'cache miss', when a TS cache has invalid info about
    // a recreated table - with old-deleted table ID. In such case TS tryes to attach
    // the index 'i3' to the deleted table - it must fail and the cache must be refreshed.

    // Create test table.
    session.execute("create table test_drop (h1 int primary key, " + // TS-1
                    "c1 int, c2 int, c3 int, c4 int, c5 int) " +
                    "with transactions = {'enabled' : true};");
    // Create test indexes.
    session.execute("create index i1 on test_drop (c1);");           // TS-2
    session.execute("create index i2 on test_drop (c2);");           // TS-3
    session.execute("select * from test_drop;");                     // TS-1
    // Drop test table.
    session.execute("drop table test_drop;");                        // TS-2
    // Create test table again.
    session.execute("create table test_drop (h1 int primary key, " + // TS-3
                    "c1 int, a2 int, a3 int, a4 int, a5 int) " +
                    "with transactions = {'enabled' : true};");
    // Create index.
    session.execute("create index i3 on test_drop (c1);");           // TS-1
    session.execute("drop table test_drop;");                        // TS-2
    session.execute("drop keyspace test_ks;");                       // TS-3

    LOG.info("End test: " + getCurrentTestMethodName());
  }

  private void assertIndexOptions(String table, String index, String target, String include)
      throws Exception {
    Row row = session.execute("select options, transactions, is_unique " +
                              "from system_schema.indexes " +
                              "where keyspace_name = ? and table_name = ? and index_name = ?",
                              DEFAULT_TEST_KEYSPACE, table, index).one();
    Map<String, String> options = row.getMap("options", String.class, String.class);
    assertEquals(target, options.get("target"));
    assertEquals(include, options.get("include"));
    Map<String, String> transactions = row.getMap("transactions", String.class, String.class);
    assertEquals("true", transactions.get("enabled"));
    assertFalse(row.getBool("is_unique"));
  }

  private void assertIndexColumns(String index, String columns) throws Exception {
    String actual = "";
    for (Row row : session.execute("select column_name, clustering_order, kind, position, type " +
                                   "from system_schema.columns " +
                                   "where keyspace_name = ? and table_name = ?",
                                   DEFAULT_TEST_KEYSPACE, index)) {
      actual += row.toString();
    }
    assertEquals(columns, actual);
  }

  private void createTable(String statement, boolean strongConsistency) throws Exception {
    session.execute(
        statement + (strongConsistency ? " with transactions = {'enabled' : true};" : ";"));
  }

  private void createIndex(String statement, boolean strongConsistency) throws Exception {
    session.execute(
        statement + (strongConsistency ? ";" :
                     " with transactions = {'enabled' : false, " +
                     "'consistency_level' : 'user_enforced'};"));
  }

  private Set<String> queryTable(String table, String columns) {
    Set<String> rows = new HashSet<String>();
    for (Row row : session.execute(String.format("select %s from %s;", columns, table))) {
      rows.add(row.toString());
    }
    return rows;
  }

  private void checkIndexColumns(Map<String, String> tableColumnMap,
                                 Map<String, String> indexColumnMap,
                                 String query) throws Exception {
    LOG.info("Check Indexes after query: " + query);
    for (Map.Entry<String, String> entry : tableColumnMap.entrySet()) {
      String iValue = indexColumnMap.get(entry.getKey());
      Set<String> table_result = queryTable("test_update", entry.getValue());
      LOG.debug("In table test_update [" + entry.getValue() + "]: " + table_result);
      Set<String> index_result = queryTable(entry.getKey(), iValue);
      LOG.debug("In index " + entry.getKey() + " [" + iValue + "]: " + index_result);
      assertEquals("Index " + entry.getKey() + " after " + query, table_result, index_result);
    }
  }

  private void assertIndexUpdate(Map<String, String> tableColumnMap,
                                 Map<String, String> indexColumnMap,
                                 String query) throws Exception {
    session.execute(query);
    checkIndexColumns(tableColumnMap, indexColumnMap, query);
  }

  private void testIndexUpdate(boolean strongConsistency) throws Exception {
    // Create test table and indexes.
    createTable("create table test_update " +
                "(h1 int, h2 text, r1 int, r2 text, c1 int, c2 text, " +
                "primary key ((h1, h2), r1, r2))", strongConsistency);
    createIndex("create index i1 on test_update (h1)", strongConsistency);
    createIndex("create index i2 on test_update ((r1, r2)) include (c2)", strongConsistency);
    createIndex("create index i3 on test_update (r2, r1) include (c1, c2)", strongConsistency);
    createIndex("create index i4 on test_update (c1)", strongConsistency);
    createIndex("create index i5 on test_update (c2) include (c1)", strongConsistency);

    Map<String, String> tableColumnMap = new HashMap<String, String>() {{
        put("i1", "h1, h2, r1, r2");
        put("i2", "r1, r2, h1, h2, c2");
        put("i3", "r2, r1, h1, h2, c1, c2");
        put("i4", "c1, h1, h2, r1, r2");
        put("i5", "c2, h1, h2, r1, r2, c1");
      }};

    Map<String, String> indexColumnMap = new HashMap<String, String>() {{
        put("i1", "\"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\"");
        put("i2", "\"C$_r1\", \"C$_r2\", \"C$_h1\", \"C$_h2\", \"C$_c2\"");
        put("i3", "\"C$_r2\", \"C$_r1\", \"C$_h1\", \"C$_h2\", \"C$_c1\", \"C$_c2\"");
        put("i4", "\"C$_c1\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\"");
        put("i5", "\"C$_c2\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\", \"C$_c1\"");
      }};

    // test_update: Row[1, a, 2, b, 3]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "insert into test_update (h1, h2, r1, r2, c1) values (1, 'a', 2, 'b', 3);");

    // test_update: Row[1, a, 2, b, 3, c]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c2 = 'c' " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");

    // test_update: Row[1, a, 2, b, 4, d]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c1 = 4, c2 = 'd' " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");

    // test_update: Row[1, a, 2, b, 4, e]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c2 = 'e' " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");

    // test_update: Row[1, a, 2, b, 4, e]
    //              Row[1, a, 12, bb, 6]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "insert into test_update (h1, h2, r1, r2, c1) values (1, 'a', 12, 'bb', 6);");

    // test_update: Row[1, a, 12, bb, 6]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "delete from test_update " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");

    // test_update: empty
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "delete from test_update where h1 = 1 and h2 = 'a';");

    // test_update: Row[11, aa, 22, bb, 3]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c1 = 3 " +
                      "where h1 = 11 and h2 = 'aa' and r1 = 22 and r2 = 'bb';");

    // test_update: empty
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c1 = null " +
                      "where h1 = 11 and h2 = 'aa' and r1 = 22 and r2 = 'bb';");

    // test_update: Row[11, aa, 222, bbb, 3, c]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c1 = 3, c2 = 'c' " +
                      "where h1 = 11 and h2 = 'aa' and r1 = 222 and r2 = 'bbb';");

    // test_update: empty
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "delete c1, c2 from test_update " +
                      "where h1 = 11 and h2 = 'aa' and r1 = 222 and r2 = 'bbb';");

    // test_update: Row[1, a, 2, b]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "insert into test_update (h1, h2, r1, r2) values (1, 'a', 2, 'b');");
    assertQuery("select h1, h2, r1, r2, c1 from test_update where c1 = null;",
                "Row[1, a, 2, b, NULL]");
    assertQuery("select h1, h2, r1, r2, c1, c2 from test_update where c2 = null;",
                "Row[1, a, 2, b, NULL, NULL]");

    // test_update: Row[1, a, 2, b, 4]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c1 = 4 " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");
    assertQuery("select h1, h2, r1, r2, c1 from test_update where c1 = null;",
                "");
    assertQuery("select h1, h2, r1, r2, c1 from test_update where c1 = 4;",
                "Row[1, a, 2, b, 4]");
    assertQuery("select h1, h2, r1, r2, c1, c2 from test_update where c2 = null;",
                "Row[1, a, 2, b, 4, NULL]");

    // test_update: Row[1, a, 2, b, 4, c]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "update test_update set c2 = 'c' " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");
    assertQuery("select h1, h2, r1, r2, c1 from test_update where c1 = 4;",
                "Row[1, a, 2, b, 4]");
    assertQuery("select h1, h2, r1, r2, c1, c2 from test_update where c2 = null;",
                "");
    assertQuery("select h1, h2, r1, r2, c1, c2 from test_update where c2 = 'c';",
                "Row[1, a, 2, b, 4, c]");

    // test_update: Row[1, a, 2, b]
    assertIndexUpdate(tableColumnMap, indexColumnMap,
                      "delete c1, c2 from test_update " +
                      "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';");
    assertQuery("select h1, h2, r1, r2, c1 from test_update where c1 = null;",
                "Row[1, a, 2, b, NULL]");
    assertQuery("select h1, h2, r1, r2, c1 from test_update where c1 = 4;",
                "");
    assertQuery("select h1, h2, r1, r2, c1, c2 from test_update where c2 = null;",
                "Row[1, a, 2, b, NULL, NULL]");
    assertQuery("select h1, h2, r1, r2, c1, c2 from test_update where c2 = 'c';",
                "");
  }

  @Test
  public void testIndexUpdate() throws Exception {
    testIndexUpdate(true);
  }

  @Test
  public void testWeakIndexUpdate() throws Exception {
    testIndexUpdate(false);
  }

  @Test
  public void testWeakIndexBatchUpdate() throws Exception {
    // Test batch insert into a table with secondary index.
    session.execute("create table test_batch (k int primary key, v text);");
    session.execute("create index test_batch_by_v on test_batch (v) " +
                    "with transactions = {'enabled' : false, " +
                    "'consistency_level' : 'user_enforced'};");

    assertQuery(String.format("select options, transactions from system_schema.indexes where "+
                              "keyspace_name = '%s' and " +
                              "table_name = 'test_batch' and " +
                              "index_name = 'test_batch_by_v';",
                              DEFAULT_TEST_KEYSPACE),
                "Row[{target=v, k}, {enabled=false, consistency_level=user_enforced}]");

    final int BATCH_SIZE = 20;
    final int KEY_COUNT = 1000;

    PreparedStatement statement = session.prepare("insert into test_batch (k, v) values (?, ?);");
    int k = 0;
    while (k < KEY_COUNT) {
      BatchStatement batch = new BatchStatement();
      for (int i = 0; i < BATCH_SIZE; i++) {
        batch.add(statement.bind(Integer.valueOf(k), "v" + k));
        k++;
      }
      session.execute(batch);
    }

    // Verify the rows in the index are identical to the indexed table.
    assertEquals(queryTable("test_batch", "k, v"),
                 queryTable("test_batch_by_v", "\"C$_k\", \"C$_v\""));

    // Verify that all the rows can be read.
    statement = session.prepare("select k from test_batch where v = ?;");
    for (int i = 0; i < KEY_COUNT; i++) {
      assertEquals(session.execute(statement.bind("v" + i)).one().getInt("k"), i);
    }
  }

  private void assertRoutingVariables(String query,
                                      List<String> expectedVars,
                                      Object[] values,
                                      String expectedRow) {
    PreparedStatement stmt = session.prepare(query);
    int hashIndexes[] = stmt.getRoutingKeyIndexes();
    if (expectedVars == null) {
      assertNull(hashIndexes);
    } else {
      List<String> actualVars = new Vector<String>();
      ColumnDefinitions vars = stmt.getVariables();
      for (int hashIndex : hashIndexes) {
        actualVars.add(vars.getTable(hashIndex) + "." + vars.getName(hashIndex));
      }
      assertEquals(expectedVars, actualVars);
    }
    assertEquals(expectedRow, session.execute(stmt.bind(values)).one().toString());
  }

  @Test
  public void testPreparedStatement() throws Exception {
    // Create test table and indexes.
    session.execute("create table test_prepare " +
                    "(h1 int, h2 text, r1 int, r2 text, c1 int, c2 text, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index i1 on test_prepare (h1);");
    session.execute("create index i2 on test_prepare ((r1, r2)) include (c2);");
    session.execute("create index i3 on test_prepare (r2, r1);");
    session.execute("create index i4 on test_prepare (c1);");
    session.execute("create index i5 on test_prepare (c2) include (c1);");

    // Insert a row.
    session.execute("insert into test_prepare (h1, h2, r1, r2, c1, c2) " +
                    "values (1, 'a', 2, 'b', 3, 'c');");

    // Select using index i1.
    assertRoutingVariables("select h1, h2, r1, r2 from test_prepare where h1 = ?;",
                           Arrays.asList("i1.h1"),
                           new Object[] {Integer.valueOf(1)},
                           "Row[1, a, 2, b]");

    // Select using index i1 but as uncovered index.
    assertRoutingVariables("select h1, h2, r1, r2, c1 from test_prepare where h1 = ?;",
                           Arrays.asList("i1.h1"),
                           new Object[] {Integer.valueOf(1)},
                           "Row[1, a, 2, b, 3]");

    // Select using base table because there is no index on h2.
    assertRoutingVariables("select h1, h2, r1, r2 from test_prepare where h2 = ?;",
                           null,
                           new Object[] {"a"},
                           "Row[1, a, 2, b]");

    // Select using index i2 because (r1, r2) is more selective than i1 alone. i3 is equally
    // selective by i2 covers c2 also.
    assertRoutingVariables("select h1, h2, r1, r2, c2 from test_prepare " +
                           "where h1 = ? and r1 = ? and r2 = ?;",
                           Arrays.asList("i2.r1", "i2.r2"),
                           new Object[] {Integer.valueOf(1), Integer.valueOf(2), "b"},
                           "Row[1, a, 2, b, c]");

    // Select using index i3.
    assertRoutingVariables("select h1, h2, r1, r2 from test_prepare where h2 = ? and r2 = ?;",
                           Arrays.asList("i3.r2"),
                           new Object[] {"a", "b"},
                           "Row[1, a, 2, b]");

    // Select using index i4.
    assertRoutingVariables("select h1, h2, r1, r2 from test_prepare where h2 = ? and c1 = ?;",
                           Arrays.asList("i4.c1"),
                           new Object[] {"a", Integer.valueOf(3)},
                           "Row[1, a, 2, b]");

    // Select using index i5 covering c1.
    assertRoutingVariables("select h1, h2, r1, r2, c1, c2 from test_prepare where c2 = ?;",
                           Arrays.asList("i5.c2"),
                           new Object[] {"c"},
                           "Row[1, a, 2, b, 3, c]");
  }

  @Test
  public void testCreateIndexWithWhereClause() throws Exception {
    LOG.info("Start test: " + getCurrentTestMethodName());
    destroyMiniCluster();
    BaseMiniClusterTest.tserverArgs.add("--cql_raise_index_where_clause_error=false");
    createMiniCluster();
    setUpCqlClient();

    // Create test table.
    LOG.info("create test table");
    session.execute("create table test_create_index " +
                    "(h1 int, h2 text, r1 int, r2 text, " +
                    "c1 int, c2 text, c3 decimal, c4 timestamp, c5 boolean, " +
                    "primary key ((h1, h2), r1, r2)) " +
                    "with transactions = {'enabled' : true};");
    LOG.info("create test index");
    session.execute("CREATE INDEX i1 ON test_create_index (r1) where r1 = 5;");
    BaseMiniClusterTest.tserverArgs.remove("--cql_raise_index_where_clause_error=false");
    LOG.info("End test: " + getCurrentTestMethodName());
  }

  @Test
  public void testClusterRestart() throws Exception {
    // Destroy existing cluster and recreate a new one before running this test. If the existing
    // cluster is reused without recreating it, a large number of tables may have been created and
    // dropped by other test cases already. In such a case, when a cluster is restarted below, the
    // tservers will take a long time replaying the DeleteTablet ops from the log before getting
    // fully initialized and starting the CQL service, causing the subsequent setUpCqlClient() to
    // time out.
    destroyMiniCluster();
    createMiniCluster();
    setUpCqlClient();

    // Create test table with index.
    session.execute("create table test_cluster_restart (k int primary key, v text) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index test_cluster_restart_by_v on test_cluster_restart (v);");

    // Insert some rows and query them by the indexed column.
    final int ROW_COUNT = 100;
    PreparedStatement insertStmt = session.prepare(
        "insert into test_cluster_restart (k, v) values (?, ?);");
    for (int i = 0; i < ROW_COUNT; i++) {
      session.execute(insertStmt.bind(Integer.valueOf(i), "v" + i));
    }
    PreparedStatement selectStmt = session.prepare(
        "select k from test_cluster_restart where v = ?;");
    for (int i = 0; i < ROW_COUNT; i++) {
      assertEquals(i, session.execute(selectStmt.bind("v" + i)).one().getInt("k"));
    }

    // Restart the cluster
    miniCluster.restart();
    setUpCqlClient();

    // Update half of the rows. Query them back and verify the unmodified rows still can still
    // be queried while the modified ones have the new value.
    PreparedStatement updateStmt = session.prepare(
        "update test_cluster_restart set v = ? where k = ?;");
    for (int i = ROW_COUNT / 2; i < ROW_COUNT; i++) {
      session.execute(updateStmt.bind("vv" + i, Integer.valueOf(i)));
    }
    selectStmt = session.prepare(
        "select k from test_cluster_restart where v = ?;");
    for (int i = 0; i < ROW_COUNT; i++) {
      assertEquals(i, session.execute(
          selectStmt.bind((i >= ROW_COUNT / 2 ? "vv" : "v") + i)).one().getInt("k"));
    }

    // Also verify that the old index values do not exist any more.
    for (int i = ROW_COUNT / 2; i < ROW_COUNT; i++) {
      assertNull(session.execute(selectStmt.bind("v" + i)).one());
    }
  }

  @Test
  public void testRestarts() throws Exception {

    // Test concurrent inserts into a table with secondary index, which require a read of the
    // table in order to update the index. For consistent update of the index, the read and write
    // operations are executed in a distributed transaction. With multiple writes happening in
    // parallel, some reads may require a restart. Verify that there are restarts and the inserts
    // are retried without error.
    session.execute("create table test_restart (k int primary key, v int) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index test_restart_by_v on test_restart (v);");

    PreparedStatement insertStmt = session.prepare(
        "insert into test_restart (k, v) values (1, 1000);");

    final int PARALLEL_WRITE_COUNT = 5;

    int initialRestarts = getRestartsCount("test_restart");
    int initialRetries = getRetriesCount();
    LOG.info("Initial restarts = {}, retries = {}", initialRestarts, initialRetries);

    while (true) {
      Set<ResultSetFuture> results = new HashSet<ResultSetFuture>();
      for (int i = 0; i < PARALLEL_WRITE_COUNT; i++) {
        results.add(session.executeAsync(insertStmt.bind()));
      }
      for (ResultSetFuture result : results) {
        result.get();
      }
      int currentRestarts = getRestartsCount("test_restart");
      int currentRetries = getRetriesCount();
      LOG.info("Current restarts = {}, retries = {}", currentRestarts, currentRetries);
      if (currentRetries > initialRetries)
        break;
    }

    // Also verify that the rows are inserted indeed.
    assertQuery("select k, v from test_restart", "Row[1, 1000]");
  }

  private void assertInvalidUniqueIndexDML(String query, String indexName) {
    try {
      session.execute(query);
      fail("InvalidQueryException not thrown for " + query);
    } catch (InvalidQueryException e) {
      assertTrue(e.getMessage().startsWith(
          String.format("Execution Error. Duplicate value disallowed by unique " +
                        "index %s", indexName)));
    }
  }

  @Test
  public void testUniqueIndex() throws Exception {
    // Create test table with 2 unique indexes (single and multiple columns).
    session.execute("create table test_unique (k int primary key, v1 int, v2 text, v3 int) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create unique index test_unique_by_v1 on test_unique (v1);");
    session.execute("create unique index test_unique_by_v2_v3 on test_unique (v2, v3);");

    assertQuery(String.format("select is_unique from system_schema.indexes where "+
                              "keyspace_name = '%s' and table_name = 'test_unique';",
                              DEFAULT_TEST_KEYSPACE),
                "Row[true]" +
                "Row[true]");

    // Test unique constraint on NULL values in v2 and v3.
    session.execute(
        "insert into test_unique (k, v1) values (1, 1);");
    assertInvalidUniqueIndexDML(
        "insert into test_unique (k, v1) values (2, 2);", "test_unique_by_v2_v3");
    session.execute(
        "insert into test_unique (k, v1, v2, v3) values (2, 2, 'b', 2);");

    // Test unique constraint on NULL value in v1.
    session.execute(
        "insert into test_unique (k, v2, v3) values (3, 'c', 3);");
    assertInvalidUniqueIndexDML(
        "insert into test_unique (k, v2, v3) values (4, 'd', 4);", "test_unique_by_v1");

    // Test unique constraint on non-NULL values.
    session.execute(
        "insert into test_unique (k, v1, v2, v3) values (5, 5, 'e', 5);");
    assertInvalidUniqueIndexDML(
        "insert into test_unique (k, v1, v2, v3) values (6, 5, 'f', 6);", "test_unique_by_v1");
    assertInvalidUniqueIndexDML(
        "insert into test_unique (k, v1, v2, v3) values (6, 6, 'e', 5);", "test_unique_by_v2_v3");

    // Test unique constraint with value (v1 = 2) removed and reinserted in another row.
    session.execute("delete from test_unique where k = 2;");
    session.execute("insert into test_unique (k, v1, v2, v3) values (3, 2, 'a', 1);");

    // Test unique constraint with value (v3 = 5) changed and the original value reinserted in
    // another row.
    session.execute("update test_unique set v3 = 6 where k = 5;");
    session.execute("insert into test_unique (k, v1, v2, v3) values (7, 7, 'e', 5);");

    // Test unique constraint with updating values with the same original values.
    session.execute("update test_unique set v1 = 7, v2 = 'e', v3 = 5 where k = 7;");
  }

  @Test
  public void testUniquePrimaryKeyIndex() throws Exception {
    // Test unique index on a primary key column.
    session.execute("create table test_unique_pk (h1 int, h2 int, r int, v int, " +
                    "primary key ((h1, h2), r)) with transactions = {'enabled' : true};");
    session.execute("create unique index test_unique_pk_by_h2 on test_unique_pk (h2);");
    session.execute("create unique index test_unique_pk_by_r on test_unique_pk (r);");

    session.execute("insert into test_unique_pk (h1, h2, r, v) values (1, 1, 1, 1);");

    // Test inserting duplicate h2 and r values.
    assertInvalidUniqueIndexDML(
        "insert into test_unique_pk (h1, h2, r, v) values (1, 1, 2, 2);", "test_unique_pk_by_h2");
    assertInvalidUniqueIndexDML(
        "insert into test_unique_pk (h1, h2, r, v) values (1, 2, 1, 2);", "test_unique_pk_by_r");

    // Restart the cluster
    miniCluster.restart();
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
    setUpCqlClient();

    // Test inserting duplicate h2 and r values again.
    assertInvalidUniqueIndexDML(
        "insert into test_unique_pk (h1, h2, r, v) values (1, 1, 2, 2);", "test_unique_pk_by_h2");
    assertInvalidUniqueIndexDML(
        "insert into test_unique_pk (h1, h2, r, v) values (1, 2, 1, 2);", "test_unique_pk_by_r");

    // Test inserting non-duplicate h2 and r value.
    session.execute("insert into test_unique_pk (h1, h2, r, v) values (1, 2, 2, 2);");
  }

  @Test
  public void testConditionalDML() throws Exception {
    // Create test 2 test tables. One with normal secondary index and one with an additional unique
    // index.
    session.execute("create table test_cond (k int primary key, v1 int, v2 text) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index test_cond_by_v1 on test_cond (v1) include (v2);");

    session.execute("create table test_cond_unique (k int primary key, v1 int, v2 text) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index test_cond_unique_by_v1 on test_cond_unique (v1);");
    session.execute("create unique index test_cond_unique_by_v2 on test_cond_unique (v2) "+
                    "include (v1);");

    // Insert into first table with conditional DML.
    session.execute("insert into test_cond (k, v1, v2) values (1, 1, 'a');");
    assertQuery("insert into test_cond (k, v1, v2) values (1, 1, 'a') if not exists;",
                "Columns[[applied](boolean), k(int), v2(varchar), v1(int)]",
                "Row[false, 1, a, 1]");
    assertQuery("insert into test_cond (k, v1, v2) values (2, 1, 'a') if not exists;",
                "Row[true]");

    // Insert into second table with conditional DML.
    session.execute("insert into test_cond_unique (k, v1, v2) values (1, 1, 'a');");
    assertQuery("insert into test_cond_unique (k, v1, v2) values (1, 1, 'a') if not exists;",
                "Columns[[applied](boolean), k(int), v2(varchar), v1(int)]",
                "Row[false, 1, a, 1]");
    assertInvalidUniqueIndexDML("insert into test_cond_unique (k, v1, v2) values (2, 2, 'a') " +
                                "if not exists;", "test_cond_unique_by_v2");
    assertQueryRowsUnordered("select * from test_cond_unique;",
                             "Row[1, 1, a]");

    assertQuery("insert into test_cond_unique (k, v1, v2) values (2, 2, 'b') if not exists;",
                "Row[true]");
    assertQueryRowsUnordered("select * from test_cond_unique;",
                             "Row[1, 1, a]", "Row[2, 2, b]");
  }

  @Test
  public void testDMLInTranaction() throws Exception {
    // Create 2 tables with secondary indexes and verify they can be updated in the one transaction.
    session.execute("create table test_txn1 (k int primary key, v int) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index test_txn1_by_v on test_txn1 (v);");

    session.execute("create table test_txn2 (k text primary key, v text) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index test_txn2_by_v on test_txn2 (v);");

    session.execute("begin transaction" +
                    "  insert into test_txn1 (k, v) values (1, 101);" +
                    "  insert into test_txn2 (k, v) values ('k1', 'v101');" +
                    "end transaction;");

    // Verify the rows.
    assertQuery("select k, v from test_txn1;", "Row[1, 101]");
    assertQuery("select k, v from test_txn2;", "Row[k1, v101]");

    // Verify rows can be selected by the index columns.
    assertQuery("select k, v from test_txn1 where v = 101;", "Row[1, 101]");
    assertQuery("select k, v from test_txn2 where v = 'v101';", "Row[k1, v101]");

    // Verify the writetimes are the same.
    assertEquals(session.execute("select writetime(v) from test_txn1 where k = 1;")
                 .one().getLong("writetime(v)"),
                 session.execute("select writetime(v) from test_txn2 where k = 'k1';")
                 .one().getLong("writetime(v)"));
  }

  @Test
  public void testSelectAll() throws Exception {
    // Create a table with index.
    session.execute("create table test_all (k int primary key, v1 int, v2 int) " +
                    "with transactions = { 'enabled' : true };");
    session.execute("create index test_all_by_v1 on test_all (v1) include (v2);");
    session.execute("insert into test_all (k, v1, v2) values (1, 2, 3);");

    // Select all columns using index and verify the selected columns are returned in the same order
    // as the table columns.
    assertQuery("select * from test_all where v1 = 2;", "Row[1, 2, 3]");
  }

  @Test
  public void testUncoveredIndex() throws Exception {
    // Create test table and uncovered index.
    session.execute("create table test_uncovered (h int, r int, v1 text, v2 int," +
                    "  primary key ((h), r)) with transactions = {'enabled' : true};");
    session.execute("create index test_uncovered_by_v1 on test_uncovered (v1);");

    // Populate the table.
    for (int h = 1; h <= 5; h++) {
      for (int r = 1; r <= 100; r++) {
        int val = (r == 3) ? 333 : h * 10 + r;
        session.execute("insert into test_uncovered (h, r, v1, v2) values (?, ?, ?, ?);",
                        h, r, "v" + val, val);
      }
    }

    // Fetch by the indexed column. Verify that the index is used and no range scan happens as
    // confirmed by the no. of next's.
    RocksDBMetrics tableMetrics = getRocksDBMetric("test_uncovered");
    RocksDBMetrics indexMetrics = getRocksDBMetric("test_uncovered_by_v1");
    LOG.info("Initial: table {}, index {}", tableMetrics, indexMetrics);

    assertQuery("select * from test_uncovered where v1 = 'v333';",
                new HashSet<String>(Arrays.asList("Row[1, 3, v333, 333]",
                                                  "Row[2, 3, v333, 333]",
                                                  "Row[3, 3, v333, 333]",
                                                  "Row[4, 3, v333, 333]",
                                                  "Row[5, 3, v333, 333]")));

    // Also verfiy select with limit and offset.
    assertQuery("select * from test_uncovered where v1 = 'v333' offset 1 limit 3;",
                new HashSet<String>(Arrays.asList("Row[2, 3, v333, 333]",
                                                  "Row[3, 3, v333, 333]",
                                                  "Row[4, 3, v333, 333]")));

    tableMetrics = getRocksDBMetric("test_uncovered").subtract(tableMetrics);
    indexMetrics = getRocksDBMetric("test_uncovered_by_v1").subtract(indexMetrics);
    LOG.info("Difference: table {}, index {}", tableMetrics, indexMetrics);

    // Verify that both the index and the primary table are read.
    assertTrue(indexMetrics.nextCount > 0);
    assertTrue(tableMetrics.nextCount > 0);

    // Verify uncovered index query of non-existent indexed value.
    assertQuery("select * from test_uncovered where v1 = 'nothing';", "");
  }

  @Test
  public void testUncoveredIndexMisc() throws Exception {
    // Create test table and index and populate with rows.
    session.execute("create table test_misc (h int, r int, s int static, v1 int, v2 int," +
                    "  primary key ((h), r)) with transactions = { 'enabled' : true };");
    session.execute("create index test_misc_by_v1 on test_misc (v1);");

    session.execute("insert into test_misc (h, r, s, v1, v2) values (1, 1, 2, 1, 11);");
    session.execute("insert into test_misc (h, r, s, v1, v2) values (1, 2, 3, 2, 22);");
    session.execute("insert into test_misc (h, r,    v1, v2) values (1, 3,    2, 33);");
    session.execute("insert into test_misc (h, r, s, v1, v2) values (2, 1, 1, 1, 111);");
    session.execute("insert into test_misc (h, r,    v1, v2) values (2, 2,    2, 222);");
    session.execute("insert into test_misc (h, r,    v1, v2) values (3, 1,    2, 333);");

    // Test select with static column.
    assertQuery("select * from test_misc where v1 = 2;",
                new HashSet<String>(Arrays.asList("Row[1, 2, 3, 2, 22]",
                                                  "Row[1, 3, 3, 2, 33]",
                                                  "Row[2, 2, 1, 2, 222]",
                                                  "Row[3, 1, NULL, 2, 333]")));

    // Test select with offset and limit.
    assertQuery("select * from test_misc where v1 = 2 offset 2 limit 1;",
                new HashSet<String>(Arrays.asList("Row[2, 2, 1, 2, 222]")));

    // Test select with additional condition on non-indexed column.
    assertQuery("select * from test_misc where v1 = 2 and v2 = 33;",
                new HashSet<String>(Arrays.asList("Row[1, 3, 3, 2, 33]")));

    // Test select with aggregate functions.
    assertQuery("select sum(r), min(v2), max(v2), sum(v2) from test_misc where v1 = 2 and v2 > 30;",
                new HashSet<String>(Arrays.asList("Row[6, 33, 333, 588]")));

    // Create test table for LIST and index and populate with rows.
    session.execute("create table test_list (h int, r int, v int, l list<int>, " +
            "PRIMARY KEY (h, r)) with transactions = { 'enabled' : true };");
    session.execute("create index on test_list (v);");

    session.execute("insert into test_list (h, r, v, l) values (1, 1, 1, [1, 2]);");
    session.execute("insert into test_list (h, r, v, l) values (2, 2, 2, [3, 4]);");

    assertQuery("select * from test_list;",
                new HashSet<String>(Arrays.asList("Row[1, 1, 1, [1, 2]]",
                                                  "Row[2, 2, 2, [3, 4]]")));

    assertQuery("select * from test_list where v = 1 and l[0] = 1;",
                new HashSet<String>(Arrays.asList("Row[1, 1, 1, [1, 2]]")));

    // Create test table for MAP and index and populate with rows.
    session.execute("create table test_map (h int, r int, v int, m map<int, int>, " +
            "PRIMARY KEY (h, r)) with transactions = { 'enabled' : true };");
    session.execute("create index on test_map (v);");

    session.execute("insert into test_map (h, r, v, m) values (1, 1, 1, {1:2});");
    session.execute("insert into test_map (h, r, v, m) values (2, 2, 2, {3:4});");

    assertQuery("select * from test_map;",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, {1=2}]",
                                              "Row[2, 2, 2, {3=4}]")));

    assertQuery("select * from test_map where v = 1 and m[1] = 2;",
                new HashSet<String>(Arrays.asList("Row[1, 1, 1, {1=2}]")));

    // Create test table for SET and index and populate with rows.
    session.execute("create table test_set (h int, r int, v int, s set<int>, " +
            "PRIMARY KEY (h, r)) with transactions = { 'enabled' : true };");
    session.execute("create index on test_set (v);");

    session.execute("insert into test_set (h, r, v, s) values (1, 1, 1, {});");
    session.execute("insert into test_set (h, r, v, s) values (2, 2, 2, {3,4});");

    assertQuery("select * from test_set;",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, NULL]",
                                              "Row[2, 2, 2, [3, 4]]")));

    assertQuery("select * from test_set where v = 1 and s = NULL;",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, NULL]")));

    // Create test table for JSONB and index and populate with rows.
    session.execute("create table test_json (h int, r int, v int, j jsonb, " +
            "PRIMARY KEY (h, r)) with transactions = { 'enabled' : true };");
    session.execute("create index on test_json (v);");

    session.execute("insert into test_json (h, r, v, j) values (1, 1, 1, '{\"a\":1}');");
    session.execute("insert into test_json (h, r, v, j) values (2, 2, 2, '{\"a\":2}');");

    assertQuery("select * from test_json;",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, {\"a\":1}]",
                                              "Row[2, 2, 2, {\"a\":2}]")));

    assertQuery("select * from test_json where v = 1 and j->>'a' = '1';",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, {\"a\":1}]")));

    // Create test table for UDT and index and populate with rows.
    session.execute("create type udt(v1 int, v2 int);");
    session.execute("create table test_udt (h int, r int, v int, u udt, " +
            "PRIMARY KEY (h, r)) with transactions = { 'enabled' : true };");
    session.execute("create index on test_udt (v);");

    session.execute("insert into test_udt (h, r, v, u) values (1, 1, 1, NULL);");
    session.execute("insert into test_udt (h, r, v, u) values (2, 2, 2, {v1:2,v2:2});");

    assertQuery("select * from test_udt;",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, NULL]",
                                              "Row[2, 2, 2, {v1:2,v2:2}]")));

    assertQuery("select * from test_udt where v = 1 and u = NULL;",
            new HashSet<String>(Arrays.asList("Row[1, 1, 1, NULL]")));
  }

  @Test
  public void testPagingSelect() throws Exception {
    // Create test table and index.
    session.execute("create table test_paging (h int, r int, v1 int, v2 varchar, " +
                    "primary key (h, r)) with transactions = { 'enabled' : true };");
    session.execute("create index test_paging_idx on test_paging (v1);");

    // Populate rows.
    session.execute("insert into test_paging (h, r, v1, v2) values (1, 1, 1, 'a');");
    session.execute("insert into test_paging (h, r, v1, v2) values (1, 2, 2, 'b');");
    session.execute("insert into test_paging (h, r, v1, v2) values (2, 1, 3, 'c');");
    session.execute("insert into test_paging (h, r, v1, v2) values (2, 2, 4, 'd');");
    session.execute("insert into test_paging (h, r, v1, v2) values (3, 1, 5, 'e');");
    session.execute("insert into test_paging (h, r, v1, v2) values (3, 2, 6, 'f');");

    // Execute uncovered select by index column with small page size.
    assertQuery(new SimpleStatement("select * from test_paging where v1 in (3, 4, 5);")
                .setFetchSize(1),
                new HashSet<String>(Arrays.asList("Row[2, 1, 3, c]",
                                                  "Row[2, 2, 4, d]",
                                                  "Row[3, 1, 5, e]")));
  }

  @Test
  public void testDropDuringWrite() throws Exception {
    int numTables = SanitizerUtil.nonTsanVsTsan(5, 2);
    int numTablets = SanitizerUtil.nonTsanVsTsan(6, 3);
    int numThreads = SanitizerUtil.nonTsanVsTsan(10, 4);
    for (int i = 0; i != numTables; ++i) {
      String tableName = "index_test_" + i;
      String indexName = "index_" + i;
      session.execute(String.format(
          "create table %s (h int, c int, primary key ((h))) " +
          "with transactions = { 'enabled' : true } and tablets = %d;", tableName, numTablets));
      session.execute(String.format(
            "create index %s on %s (c) with tablets = %d;", indexName, tableName, numTablets));
      final PreparedStatement statement = session.prepare(String.format(
          "insert into %s (h, c) values (?, ?);", tableName));

      List<Thread> threads = new ArrayList<Thread>();
      while (threads.size() != numThreads) {
        Thread thread = new Thread(() -> {
          int key = 0;
          while (!Thread.interrupted()) {
            session.execute(statement.bind(Integer.valueOf(key), Integer.valueOf(-key)));
            ++key;
          }
        });
        thread.start();
        threads.add(thread);
      }
      try {
        Thread.sleep(5000);
        session.execute(String.format("drop table %s;", tableName));
      } finally {
        for (Thread thread : threads) {
          thread.interrupt();
        }
        for (Thread thread : threads) {
          thread.join();
        }
      }
    }
  }

  @Test
  public void testOrderBy() throws Exception {
    session.execute("CREATE TABLE test_order (a text," +
                    "                         b text," +
                    "                         c int," +
                    "                         PRIMARY KEY (a, b))" +
                    "  WITH CLUSTERING ORDER BY (b ASC) AND default_time_to_live = 0;");

    session.execute("CREATE INDEX test_order_index ON test_order (b, c)" +
                    "  INCLUDE (a)" +
                    "  WITH CLUSTERING ORDER BY (c DESC)" +
                    "    AND transactions = { 'enabled' : FALSE, " +
                    "                         'consistency_level' : 'user_enforced' };");

    // rowDesc is the query result in descending order, rowAsc, ascending.
    String rowDesc = "";
    String rowAsc = "";

    int rowCount = 10;
    String a;
    String b = "index_hash";
    int cMax = 100;
    int cDesc;
    int cAsc = cMax - rowCount;

    // INSERT rows to be selected with order by.
    for (int i = 0; i < rowCount; i++) {
      cDesc = cMax - i;
      a = String.format("a_%d", cDesc);
      rowDesc += String.format("Row[%s, %s, %d]", a, b, cDesc);
      session.execute(String.format("INSERT INTO test_order (a, b, c) VALUES('%s', '%s', %d);",
                                    a, b, cDesc));

      cAsc++;
      a = String.format("a_%d", cAsc);
      rowAsc += String.format("Row[%s, %s, %d]", a, b, cAsc);
    }

    // INSERT dummy rows that should be filtered out by the query.
    b = "dummy";
    cDesc = 100;
    for (int i = 0; i < rowCount; i++) {
      cDesc = cMax - i;
      a = String.format("a_%d", cDesc);
      session.execute(String.format("INSERT INTO test_order (a, b, c) VALUES('%s', '%s', %d);",
                                    a, b, cDesc));
    }

    // Asserting query result.
    assertQuery("SELECT * FROM test_order WHERE b = 'index_hash';", rowDesc);
    assertQuery("SELECT * FROM test_order WHERE b = 'index_hash' ORDER BY c DESC;", rowDesc);
    assertQuery("SELECT * FROM test_order WHERE b = 'index_hash' ORDER BY c ASC;", rowAsc);
  }

  @Test
  public void testCreateIndexWithNonTransactionalTable() throws Exception {
    LOG.info("Start test: " + getCurrentTestMethodName());

    // Create transactional test table and index.
    session.execute("create table test_tr_tbl (h1 int primary key, c1 int) " +
                    "with transactions = {'enabled' : true};");
    session.execute("create index on test_tr_tbl(c1);");
    session.execute("create index idx2 on test_tr_tbl(c1) with " +
                    "transactions = {'enabled' : true};");

    runInvalidStmt("create index on test_tr_tbl(c1) with " +
                   "transactions = {'enabled' : false};");
    runInvalidStmt("create index on test_tr_tbl(c1) with " +
                   "transactions = {'enabled' : true, 'consistency_level' : 'user_enforced'};");
    runInvalidStmt("create index on test_tr_tbl(c1) with " +
                   "transactions = {'enabled' : false, 'consistency_level' : 'user_enforced'};");
    runInvalidStmt("create index on test_tr_tbl(c1) with " +
                   "transactions = {'consistency_level' : 'user_enforced'};");

    // Create non-transactional test tables and indexes.
    session.execute("create table test_non_tr_tbl (h1 int primary key, c1 int) " +
                    "with transactions = {'enabled' : false};");

    runInvalidStmt("create index on test_non_tr_tbl(c1);");
    runInvalidStmt("create index on test_non_tr_tbl(c1) with " +
                   "transactions = {'enabled' : true};");
    runInvalidStmt("create index on test_non_tr_tbl(c1) with " +
                   "transactions = {'enabled' : false};");
    runInvalidStmt("create index on test_non_tr_tbl(c1) with " +
                   "transactions = {'enabled' : true, 'consistency_level' : 'user_enforced'};");
    runInvalidStmt("create index on test_non_tr_tbl(c1) with " +
                   "transactions = {'consistency_level' : 'user_enforced'};");

    // Test weak index.
    session.execute("create index test_non_tr_tbl_idx on test_non_tr_tbl(c1) with " +
                    "transactions = {'enabled' : false, 'consistency_level' : 'user_enforced'};");
    assertQuery("select options, transactions from system_schema.indexes where " +
                "index_name = 'test_non_tr_tbl_idx';",
                "Row[{target=c1, h1}, {enabled=false, consistency_level=user_enforced}]");

    session.execute("create table test_reg_tbl (h1 int primary key, c1 int);");

    runInvalidStmt("create index on test_reg_tbl(c1);");
    runInvalidStmt("create index on test_reg_tbl(c1) with " +
                   "transactions = {'enabled' : true};");
    runInvalidStmt("create index on test_reg_tbl(c1) with " +
                   "transactions = {'enabled' : false};");
    runInvalidStmt("create index on test_reg_tbl(c1) with " +
                   "transactions = {'enabled' : true, 'consistency_level' : 'user_enforced'};");
    runInvalidStmt("create index on test_reg_tbl(c1) with " +
                   "transactions = {'consistency_level' : 'user_enforced'};");

    // Test weak index.
    session.execute("create index test_reg_tbl_idx on test_reg_tbl(c1) with " +
                    "transactions = {'enabled' : false, 'consistency_level' : 'user_enforced'};");
    assertQuery("select options, transactions from system_schema.indexes where " +
                "index_name = 'test_reg_tbl_idx';",
                "Row[{target=c1, h1}, {enabled=false, consistency_level=user_enforced}]");

    // Drop test tables.
    session.execute("drop table test_tr_tbl;");
    session.execute("drop table test_non_tr_tbl;");
    session.execute("drop table test_reg_tbl;");

    LOG.info("End test: " + getCurrentTestMethodName());
  }

  @Test
  public void testCreateInvalidOrderBy() throws Exception {
    // This test makes sure that server does not crash for invalid query such as those with
    // invalid ORDER BY expression.
    LOG.info("Start test: " + getCurrentTestMethodName());

    // Test scalar index against ORDER BY non existing column.
    session.execute("CREATE TABLE test_order_by(a INT PRIMARY KEY, b INT, c INT," +
                    "  non_index_column INT, non_index_cluster_column INT)" +
                    "  WITH TRANSACTIONS = {'enabled' : true};");
    session.execute("CREATE INDEX test_order_by_idx ON test_order_by(b, c)" +
                    "  INCLUDE (non_index_cluster_column);");
    // Run one valid query to make sure the setup is correct.
    runValidSelect("SELECT * FROM test_order_by WHERE b = 3 ORDER BY c;");
    // Test invalid ORDER BY for non-existing column.
    runInvalidQuery("SELECT * FROM test_order_by WHERE b = 3 ORDER BY non_existent_column;");
    // Test invalid ORDER BY for column "non_index_column" that exists in TABLE but not in INDEX.
    runInvalidQuery("SELECT * FROM test_order_by WHERE b = 3 ORDER BY non_index_column;");
    // Test invalid ORDER BY for column that is not an INDEX clustering column.
    runInvalidQuery("SELECT * FROM test_order_by WHERE b = 3 ORDER BY non_index_cluster_column;");

    // Test jsonb index against ORDER BY non existing field.
    session.execute("CREATE TABLE test_jsonb_order_by(i INT, j JSONB, k INT, PRIMARY KEY (i, k))" +
                    "  WITH TRANSACTIONS = { 'enabled' : true };");
    session.execute("CREATE INDEX test_jsonb_order_by_idx ON test_jsonb_order_by(k, j->>'x');");
    // Run one valid query to make sure the setup is correct.
    runValidSelect("SELECT * FROM test_jsonb_order_by WHERE k = 1 ORDER BY j->>'x';");
    // Test invalid ORDER BY non existing column "j->>'y'".
    runInvalidQuery("SELECT * FROM test_jsonb_order_by WHERE k = 1 ORDER BY j->>'y';");
    // Test invalid ORDER BY column "j" that exists in the TABLE but not the INDEX.
    runInvalidQuery("SELECT * FROM test_jsonb_order_by WHERE k = 1 ORDER BY j;");
  }

  @Test
  public void testColumnCoverage() throws Exception {
    // Create test table.
    session.execute("CREATE TABLE test_coverage" +
                    "  ( h INT, r INT, v INT, vv INT, PRIMARY KEY (h, r) )" +
                    "  WITH transactions = {'enabled' : true};");

    // Create test index.
    session.execute("CREATE INDEX vidx ON test_coverage (v);");
    assertIndexOptions("test_coverage", "vidx", "v, h, r", null);

    // Use INSERT & SELECT to check for coverage.
    int h = 7;
    int r = h * 2;
    int v = h * 3;
    int vv = h * 4;
    String stmt = String.format("INSERT INTO test_coverage(h, r, v, vv)" +
                                "  VALUES (%d, %d, %d, %d);", h, r, v, vv);
    session.execute(stmt);

    String query = String.format("SELECT vv FROM test_coverage WHERE v = %d;", v);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT * FROM test_coverage WHERE v = %d;", v);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT h FROM test_coverage WHERE v = %d AND vv = %d;", v, vv);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT * FROM test_coverage WHERE v = %d AND vv = %d;", v, vv);
    assertEquals(1, session.execute(query).all().size());
  }

  @Test
  public void testIndexUpdateWithChangeInExpressionResult() throws Exception {
    // Create test table and indexes.
    session.execute("CREATE TABLE test_update (h int PRIMARY KEY, j JSONB) " +
                    "WITH transactions = {'enabled' : true};");
    // Note: indexed expression is "j->>'x'", but it will be NULL if we set j={"a":n}.
    session.execute("CREATE INDEX i1 on test_update (j->>'x');");

    // test_update: Row[1, {"a":1}]
    session.execute("insert into test_update (h, j) values (1, '{\"a\":1}');");
    assertQuery("select * from test_update;", "Row[1, {\"a\":1}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, NULL]");

    // Note: in the following UPDATE the value of column 'j' in the main table is CHANGED,
    // but the EXPR(j) = j->>'x' is still UNCHANGED and equal NULL.
    // test_update: Row[1, {"a":2}]
    session.execute("update test_update set j = '{\"a\":2}' where h = 1;");
    assertQuery("select * from test_update;", "Row[1, {\"a\":2}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, NULL]");

    // test_update: Row[1, {"x":3}]
    session.execute("update test_update set j = '{\"x\":3}' where h = 1;");
    assertQuery("select * from test_update;", "Row[1, {\"x\":3}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, 3]");

    // test_update: Row[1, {"x":3}], Row[2, {"a":1}]
    session.execute("insert into test_update (h, j) values (2, '{\"a\":1}');");
    assertQuery("select * from test_update;", "Row[1, {\"x\":3}]Row[2, {\"a\":1}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, 3]Row[2, NULL]");

    session.execute("update test_update set j = '{\"x\":3}' where h = 1;");
    assertQuery("select * from test_update;", "Row[1, {\"x\":3}]Row[2, {\"a\":1}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, 3]Row[2, NULL]");

    // test_update: Row[1, {"x":3}], Row[2, {"x":3}]
    session.execute("update test_update set j = '{\"x\":3}' where h = 2;");
    assertQuery("select * from test_update;", "Row[1, {\"x\":3}]Row[2, {\"x\":3}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, 3]Row[2, 3]");

    // test_update: Row[1, '{"x":3, "z":7}'], Row[2, {"x":3}]
    session.execute("update test_update set j = '{\"x\":3, \"z\":7}' where h = 1;");
    assertQuery("select * from test_update;", "Row[1, {\"x\":3,\"z\":7}]Row[2, {\"x\":3}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i1;", "Row[1, 3]Row[2, 3]");
  }

  private Map<String, RocksDBMetrics> initRocksDBMetrics(String tableName,
                                                        Map<String, String> indexColumnMap)
                                                        throws Exception {
    Map<String, RocksDBMetrics> metrics = new HashMap<String, RocksDBMetrics>();
    metrics.put(tableName, getRocksDBMetric(tableName));
    for (Map.Entry<String, String> entry : indexColumnMap.entrySet()) {
      String indexName = entry.getKey();
      metrics.put(indexName, getRocksDBMetric(indexName));
    }
    return metrics;
  }

  private void checkRocksDBMetricsChanges(TableProperties tp,
                                          Map<String, RocksDBMetrics> metrics,
                                          String query,
                                          String... notUpdatedTableNames) throws Exception {
    LOG.info("Check table metrics after query: " + query);
    Set<String> notUpdatedTables = new HashSet<String>(Arrays.asList(notUpdatedTableNames));
    for (Map.Entry<String, RocksDBMetrics> entry : metrics.entrySet()) {
      String tableName = entry.getKey();
      RocksDBMetrics metric = entry.getValue();
      RocksDBMetrics updatedMetric = getRocksDBMetric(tableName);

      if (notUpdatedTables.contains(tableName)) {
        // Check that the NOT updated table metric was not changed.
        assertTrue("Unexpected update of table/index " + tableName + " after: " + query,
                   updatedMetric.equals(metric));
      } else {
        // Note: user-enforced index metric is not changed even if the index was updated.
        if (tp.isTransactional() || tableName == "test_update") {
          // Expecting the table metric is updated.
          assertTrue("Expected update of table/index " + tableName + " after: " + query,
                     !updatedMetric.equals(metric));
        } else {
          LOG.info("Skipping metric check for user-enforced index {}: {} - {}",
                   tableName, metric, updatedMetric);
        }
      }
      metrics.put(tableName, updatedMetric);
    }
  }

  private Object strToObject(String value) throws Exception {
    if (value.toLowerCase().equals("null"))
      return null;

    if (value.charAt(0) == '\'')
      if  (value.charAt(1) == '{') // JSONB
        return value.substring(1, value.length() - 1);
      else // Text
        return value;

    return Integer.valueOf(value);
  }

  private void runQuery(Boolean runPrepared, String query) throws Exception {
    if (!runPrepared) {
      LOG.info("Run query: " + query);
      session.execute(query);
      return;
    }

    query = query.replaceAll("=", " = ")
                 .replaceAll(",", " , ");
    List<String> words = new ArrayList<String>(Arrays.asList(query.split("\\s")));
    words.removeAll(Arrays.asList("", null));

    final String command = words.get(0).toLowerCase();
    String statement = "";
    List<Object> bindValues = new ArrayList<Object>();

    if (command.equals("insert")) {
      String[] parts = query.split("[\\(\\)]");
      String[] names = parts[1].replaceAll("\\s+", "").split(",");
      String[] values = parts[3].replaceAll("\\s+", "").split(",");
      assertEquals(names.length, values.length);
      statement = parts[0] + "(" + parts[1] + ")" + parts[2] + "(";
      for (int i = 0; i < values.length; ++i) {
        bindValues.add(strToObject(values[i]));
        statement += (i == 0 ? "?" : ", ?");
      }
      statement += ")";
    } else if (command.equals("update") || command.equals("delete")) {
      for (int i = 0; i <  words.size(); ++i) {
        if (words.get(i).equals("=")) {
          bindValues.add(strToObject(words.get(i+1)));
          words.set(i+1, "?");
        }
        statement += " " + words.get(i);
      }
    } else {
      fail("Unknown statement type: " + command);
    }

    LOG.info("Run prepared query: " + statement);
    assertFalse(statement.isEmpty());
    PreparedStatement prepared = session.prepare(statement);
    session.execute(prepared.bind(bindValues.toArray()));
  }

  private void assertIndexDataAndMetrics(TableProperties tp,
                                         Map<String, String> tableColumnMap,
                                         Map<String, String> indexColumnMap,
                                         String query,
                                         String... notUpdatedTableNames) throws Exception {
    Map<String, RocksDBMetrics> metrics = initRocksDBMetrics("test_update", indexColumnMap);
    runQuery(tp.usePreparedQueries(), query);
    checkRocksDBMetricsChanges(tp, metrics, query, notUpdatedTableNames);
    checkIndexColumns(tableColumnMap, indexColumnMap, query);
  }

  public void doTestOptimizedIndexUpdate(TableProperties tp) throws Exception {
    final String tableProp = (tp.isTransactional() ?
        " with transactions = { 'enabled' : true }" : "");
    final String indexTrans =
        " transactions = {'enabled' : false, 'consistency_level' : 'user_enforced'}";
    final String withIndexProp = (tp.isTransactional() ? "" : " with" + indexTrans);
    final String andIndexProp = (tp.isTransactional() ? "" : " and" + indexTrans);
    // Create test table and indexes.
    session.execute("CREATE TABLE test_update " +
                    "(h1 int, h2 text, r1 int, r2 text, c1 int, c2 text, " +
                    "PRIMARY KEY ((h1, h2), r1, r2))" + tableProp);
    // PK-only indexes.
    session.execute("CREATE INDEX i1 on test_update (h1)" + withIndexProp);
    session.execute("CREATE INDEX i2 on test_update (r1)" + withIndexProp);
    session.execute("CREATE INDEX i3 on test_update ((r2))" + withIndexProp);
    session.execute("CREATE INDEX i4 on test_update ((h2, r2))" + withIndexProp);
    session.execute("CREATE INDEX i5 on test_update ((r1, r2)) include (c2)" + withIndexProp);
    session.execute("CREATE INDEX i6 on test_update (r2, r1) include (c1, c2)" + withIndexProp);
    // Non-PK-only indexes.
    session.execute("CREATE INDEX i7 on test_update (c1)" + withIndexProp);
    session.execute("CREATE INDEX i8 on test_update (c2) include (c1)" + withIndexProp);
    session.execute("CREATE INDEX i9 on test_update (c2, c1)" + withIndexProp);
    session.execute("CREATE INDEX i10 on test_update (h1, c1) include (c2, r1)" + withIndexProp);
    session.execute("CREATE INDEX i11 on test_update (c2, r2) include (h1, c1)" + withIndexProp);
    session.execute("CREATE INDEX i12 on test_update (c2)" + withIndexProp);

    Map<String, String> tableColumnMap = new HashMap<String, String>() {{
        put("i1", "h1, h2, r1, r2");
        put("i2", "r1, h1, h2, r2");
        put("i3", "r2, h1, h2, r1");
        put("i4", "h2, r2, h1, r1");
        put("i5", "r1, r2, h1, h2, c2");
        put("i6", "r2, r1, h1, h2, c1, c2");
        put("i7", "c1, h1, h2, r1, r2");
        put("i8", "c2, h1, h2, r1, r2, c1");
        put("i9", "c2, c1, h1, h2, r1, r2");
        put("i10", "h1, c1, h2, r1, r2, c2");
        put("i11", "c2, r2, h1, h2, r1, c1");
        put("i12", "c2, h1, h2, r1, r2");
      }};

    Map<String, String> indexColumnMap = new HashMap<String, String>() {{
        put("i1", "\"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\"");
        put("i2", "\"C$_r1\", \"C$_h1\", \"C$_h2\", \"C$_r2\"");
        put("i3", "\"C$_r2\", \"C$_h1\", \"C$_h2\", \"C$_r1\"");
        put("i4", "\"C$_h2\", \"C$_r2\", \"C$_h1\", \"C$_r1\"");
        put("i5", "\"C$_r1\", \"C$_r2\", \"C$_h1\", \"C$_h2\", \"C$_c2\"");
        put("i6", "\"C$_r2\", \"C$_r1\", \"C$_h1\", \"C$_h2\", \"C$_c1\", \"C$_c2\"");
        put("i7", "\"C$_c1\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\"");
        put("i8", "\"C$_c2\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\", \"C$_c1\"");
        put("i9", "\"C$_c2\", \"C$_c1\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\"");
        put("i10", "\"C$_h1\", \"C$_c1\", \"C$_h2\", \"C$_r1\", \"C$_r2\", \"C$_c2\"");
        put("i11", "\"C$_c2\", \"C$_r2\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_c1\"");
        put("i12", "\"C$_c2\", \"C$_h1\", \"C$_h2\", \"C$_r1\", \"C$_r2\"");
      }};

    // test_update: Row[1, a, 2, b, 3]
    // Added NEW row: PK + set c1, NOT set c2 (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "insert into test_update (h1, h2, r1, r2, c1) values (1, 'a', 2, 'b', 3)");

    // test_update: Row[1, a, 2, b, 3, c]
    // Update: not changed c1, set c2
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c2 = 'c' " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'",
        // not updated indexes:
        "i7");
    // Update: not changed c1, touch c2
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c2 = 'c' " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'",
        // not updated indexes:
        "i7", "i8", "i9", "i10", "i11", "i12");

    // test_update: Row[1, a, 2, b, 4, c]
    // Update: modify c1, not changed c2
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = 4 " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'",
        // not updated indexes:
        "i12");
    // Update: touch c1, not changed c2
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = 4 " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'",
        // not updated indexes:
        "i7", "i8", "i9", "i10", "i11", "i12");

    // test_update: Row[1, a, 2, b, 5, d]
    // Update: modify c1, modify c2 (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = 5, c2 = 'd' " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'");
    // Update: touch c1, touch c2
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = 5, c2 = 'd' " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'",
        // not updated indexes:
        "i7", "i8", "i9", "i10", "i11", "i12");

    // test_update: Row[1, a, 2, b, 5, e]
    // Update: not changed c1, modify c2
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c2 = 'e' " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'",
        // not updated indexes:
        "i7");

    // test_update: Row[1, a,  2,  b, 5, e]
    //              Row[1, a, 12, bb, 6   ]
    // Added NEW row: PK + set c1, NOT set c2 (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "insert into test_update (h1, h2, r1, r2, c1) values (1, 'a', 12, 'bb', 6)");

    // test_update: Row[1, a,  2,  b, 5, e]
    //              Row[1, a, 12, bb, null]
    // In existing row: delete c1, not changed null c2.
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete c1 from test_update " +
        "where h1 = 1 and h2 = 'a' and r1 = 12 and r2 = 'bb'",
        // not updated indexes:
        "i1", "i2", "i3", "i4", "i5", "i12");
    // In existing row: not changed null c1, delete NULL c2.
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete c2 from test_update " +
        "where h1 = 1 and h2 = 'a' and r1 = 12 and r2 = 'bb'",
        // not updated indexes:
        "i1", "i2", "i3", "i4", "i5", "i6", "i7", "i8", "i9", "i10", "i11", "i12");

    // test_update: Row[1, a, 12, bb, null, null]
    // Delete first row by PK. (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete from test_update " +
        "where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b'");

    // test_update: empty
    // Delete all existing rows. (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete from test_update where h1 = 1 and h2 = 'a'");

    // test_update: Row[11, aa, 22, bb, 3]
    // UPsert: PK + set c1, not set c2. (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = 3 " +
        "where h1 = 11 and h2 = 'aa' and r1 = 22 and r2 = 'bb'");

    // test_update: Row[11, aa, 22, bb, null]
    // Update: set c1 = NULL, not set c2. (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = null " +
        "where h1 = 11 and h2 = 'aa' and r1 = 22 and r2 = 'bb'");

    // test_update: Row[11, aa, 222, bbb, 3, c]
    // UPsert: PK + set c1, set c2. (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c1 = 3, c2 = 'c' " +
        "where h1 = 11 and h2 = 'aa' and r1 = 222 and r2 = 'bbb'");

    // test_update: Row[11, aa, 222, bbb, null, null]
    // In existing row: delete c1, delete c2. (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete c1, c2 from test_update " +
        "where h1 = 11 and h2 = 'aa' and r1 = 222 and r2 = 'bbb'");
  }

  @Test
  public void testOptimizedIndexUpdate() throws Exception {
    doTestOptimizedIndexUpdate(new TableProperties(TableProperties.TP_NON_TRANSACTIONAL));
  }

  @Test
  public void testOptimizedIndexUpdate_Transactional() throws Exception {
    doTestOptimizedIndexUpdate(new TableProperties(TableProperties.TP_TRANSACTIONAL));
  }

  @Test
  public void testPreparedOptimizedIndexUpdate() throws Exception {
    doTestOptimizedIndexUpdate(new TableProperties(
        TableProperties.TP_NON_TRANSACTIONAL | TableProperties.TP_PREPARED_QUERY));
  }

  @Test
  public void testPreparedOptimizedIndexUpdate_Transactional() throws Exception {
    doTestOptimizedIndexUpdate(new TableProperties(
        TableProperties.TP_TRANSACTIONAL | TableProperties.TP_PREPARED_QUERY));
  }

  public void doTestOptimizedJsonIndexUpdate(TableProperties tp) throws Exception {
    final String tableProp = (tp.isTransactional() ?
        " with transactions = { 'enabled' : true }" : "");
    final String indexTrans =
        " transactions = {'enabled' : false, 'consistency_level' : 'user_enforced'}";
    final String withIndexProp = (tp.isTransactional() ? "" : " with" + indexTrans);
    final String andIndexProp = (tp.isTransactional() ? "" : " and" + indexTrans);
    // Create test table and indexes.
    session.execute("CREATE TABLE test_update " +
                    "(h int, r int, c int, j JSONB, PRIMARY KEY ((h), r)) " + tableProp);
    session.execute("CREATE INDEX i1 on test_update (j->>'a')" + withIndexProp);
    session.execute("CREATE INDEX i2 on test_update ((j->'a'->>'b'))" + withIndexProp);
    session.execute("CREATE INDEX i3 on test_update (j->'a'->>'b') include (c)" + withIndexProp);
    session.execute("CREATE INDEX i4 on test_update ((j->>'a')) include (c)" + withIndexProp);
    session.execute("CREATE INDEX i5 on test_update (c)" + withIndexProp);
    session.execute("CREATE INDEX i6 on test_update ((c))" + withIndexProp);

    Map<String, String> tableColumnMap = new HashMap<String, String>() {{
        put("i1", "j->>'a', h, r");
        put("i2", "j->'a'->>'b', h, r");
        put("i3", "j->'a'->>'b', h, r, c");
        put("i4", "j->>'a', h, r, c");
        put("i5", "c, h, r");
        put("i6", "c, h, r");
      }};

    Map<String, String> indexColumnMap = new HashMap<String, String>() {{
        put("i1", "\"C$_j->>\'J$_a\'\", \"C$_h\", \"C$_r\"");
        put("i2", "\"C$_j->\'J$_a\'->>\'J$_b\'\", \"C$_h\", \"C$_r\"");
        put("i3", "\"C$_j->\'J$_a\'->>\'J$_b\'\", \"C$_h\", \"C$_r\", \"C$_c\"");
        put("i4", "\"C$_j->>\'J$_a\'\", \"C$_h\", \"C$_r\", \"C$_c\"");
        put("i5", "\"C$_c\", \"C$_h\", \"C$_r\"");
        put("i6", "\"C$_c\", \"C$_h\", \"C$_r\"");
      }};

    // test_update: Row[1, 2, 3, {"a":1}] (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "insert into test_update (h, r, c, j) values (1, 2, 3, '{\"a\":1}')");

    // test_update: Row[1, 2, 9, {"a":1}]
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c = 9 where h = 1 and r = 2",
        // not updated indexes:
        "i1", "i2");
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c = 9 where h = 1 and r = 2",
        // not updated indexes:
        "i1", "i2", "i3", "i4", "i5", "i6");
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set j = '{\"a\":1}' where h = 1 and r = 2",
        // not updated indexes:
        "i1", "i2", "i3", "i4", "i5", "i6");

    // test_update: Row[1, 2, 9, {"a":2}]
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set j = '{\"a\":2}' where h = 1 and r = 2",
        // not updated indexes:
        "i2", "i3", "i5", "i6");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".i2;", "Row[1, 2, NULL]");

    // test_update: empty (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete from test_update where h = 1 and r = 2");

    // test_update: Row[4, 5, 6, {"a":{"b":2}}] (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "insert into test_update (h, r, c, j) values " +
        "(4, 5, 6, '{\"a\":{\"b\":2}}')");

    // test_update: Row[4, 5, 6, {"a":{"b":3}}]
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set j = '{\"a\":{\"b\":3}}' where h = 4 and r = 5",
        // not updated indexes:
        "i5", "i6");

    // test_update: Row[4, 5, null, {"a":{"b":3}}]
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete c from test_update where h = 4 and r = 5",
        // not updated indexes:
        "i1", "i2");

    // test_update: Row[4, 5, null, null] (updating all indexes)
    //TODO: If JSONB value == NULL: Error - Execution Error. Not enough data to process
    //      https://github.com/yugabyte/yugabyte-db/issues/5899
    //      [YCQL] Incorrect 'null' value handling with secondary indexes based on JSONB column.
/*
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete j from test_update where h = 4 and r = 5");

    // test_update: Row[11, 22, 33, null] (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c = 33 where h = 11 and r = 22");

    // test_update: Row[11, 22, null, null] (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c = null where h = 11 and r = 22");
*/
    // test_update: Row[44, 55, null, {"a":1}] (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set j = '{\"a\":1}' where h = 44 and r = 55");

    // test_update: Row[44, 55, null, null] (updating all indexes)
    //TODO: If JSONB value == NULL: Error - Execution Error. Not enough data to process
    //      https://github.com/yugabyte/yugabyte-db/issues/5899
    //      [YCQL] Incorrect 'null' value handling with secondary indexes based on JSONB column.
/*
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set j = null where h = 44 and r = 55");
*/
    // test_update: Row[66, 77, 1, {"a":1}] (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c = 1, j = '{\"a\":1}' where h = 66 and r = 77");

    // test_update: Row[66, 77, null, null] (updating all indexes)
    //TODO: If JSONB value == NULL: Error - Execution Error. Not enough data to process
    //      https://github.com/yugabyte/yugabyte-db/issues/5899
    //      [YCQL] Incorrect 'null' value handling with secondary indexes based on JSONB column.
/*
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "update test_update set c = null, j = null where h = 66 and r = 77");
*/
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete c from test_update where h = 66 and r = 77",
        // not updated indexes:
        "i1", "i2");
    // (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete j from test_update where h = 66 and r = 77");

    //TODO: If JSONB value == NULL: Error - Execution Error. Not enough data to process
    //      https://github.com/yugabyte/yugabyte-db/issues/5899
    //      [YCQL] Incorrect 'null' value handling with secondary indexes based on JSONB column.
/*
    // (updating all indexes)
    assertIndexDataAndMetrics(tp, tableColumnMap, indexColumnMap,
        "delete c, j from test_update where h = 66 and r = 77");
*/
  }

  @Test
  public void testOptimizedJsonIndexUpdate() throws Exception {
    doTestOptimizedIndexUpdate(new TableProperties(TableProperties.TP_NON_TRANSACTIONAL));
  }

  @Test
  public void testOptimizedJsonIndexUpdate_Transactional() throws Exception {
    doTestOptimizedJsonIndexUpdate(new TableProperties(TableProperties.TP_TRANSACTIONAL));
  }

  @Test
  public void testPreparedOptimizedJsonIndexUpdate() throws Exception {
    doTestOptimizedIndexUpdate(new TableProperties(
        TableProperties.TP_NON_TRANSACTIONAL | TableProperties.TP_PREPARED_QUERY));
  }

  @Test
  public void testPreparedOptimizedJsonIndexUpdate_Transactional() throws Exception {
    doTestOptimizedJsonIndexUpdate(new TableProperties(
        TableProperties.TP_TRANSACTIONAL | TableProperties.TP_PREPARED_QUERY));
  }
}
