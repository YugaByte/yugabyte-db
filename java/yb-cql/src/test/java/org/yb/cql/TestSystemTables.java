//
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

import java.nio.ByteBuffer;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;

import com.datastax.driver.core.SimpleStatement;
import com.datastax.driver.core.exceptions.ServerError;
import com.google.common.base.Stopwatch;
import com.google.common.net.HostAndPort;
import org.junit.After;
import org.junit.BeforeClass;
import org.junit.Test;
import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import org.yb.client.YBClient;
import org.yb.minicluster.*;
import org.yb.master.Master;

import static java.util.concurrent.TimeUnit.SECONDS;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.assertNull;
import static org.yb.AssertionWrappers.assertNotEquals;
import static org.yb.AssertionWrappers.assertNotNull;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestSystemTables extends BaseCQLTest {

  private static final String DEFAULT_SCHEMA_VERSION = "00000000-0000-0000-0000-000000000000";
  private static final String MURMUR_PARTITIONER = "org.apache.cassandra.dht.Murmur3Partitioner";
  private static final String RELEASE_VERSION = "3.9-SNAPSHOT";
  private static final String PLACEMENT_REGION = "region1";
  private static final String PLACEMENT_ZONE = "zone1";
  private static final int SYSTEM_PARTITIONS_REFRESH_SECS = 10;

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.tserverHeartbeatTimeoutMs(5000);
    builder.yqlSystemPartitionsVtableRefreshSecs(SYSTEM_PARTITIONS_REFRESH_SECS);
  }

  @After
  public void verifyMasterReads() throws Exception {
    // Verify all reads went to the leader master.
    YBClient client = miniCluster.getClient();
    HostAndPort leaderMaster = client.getLeaderMasterHostAndPort();
    Map<HostAndPort, MiniYBDaemon> masters = miniCluster.getMasters();
    for (Map.Entry<HostAndPort, MiniYBDaemon> master : masters.entrySet()) {
      Metrics metrics = new Metrics(master.getKey().getHost(),master.getValue().getWebPort(),
        "server");
      long numOps = metrics.getHistogram(TSERVER_READ_METRIC).totalCount;
      if (leaderMaster.equals(master.getKey())) {
        assertTrue(numOps > 0);
      } else {
        assertEquals(0, numOps);
      }
    }
  }

  private void verifyPeersTable(List<Row> rows, boolean addressesOnly) throws Exception {
    List<InetSocketAddress> contactPoints = miniCluster.getCQLContactPoints();
    // One of the contact points will be missing, since that is the node we connected to.
    assertEquals(contactPoints.size() - 1, rows.size());
    for (Row row : rows) {
      boolean found = false;
      for (InetSocketAddress addr : contactPoints) {
        if (addr.getAddress().equals(row.getInet("peer")) &&
            addr.getAddress().equals(row.getInet("rpc_address"))) {
          found = true;
        }
      }

      if (!addressesOnly) {
        assertNotNull(row.getUUID("host_id"));
        assertNotNull(row.getString("data_center"));
        assertNotNull(row.getString("rack"));
        assertEquals(row.getString("release_version"), RELEASE_VERSION);
      }
      assertTrue(found);
    }
  }

  private boolean verifySystemSchemaTables(List<Row> results,
                                           String namespace_name,
                                           String table_name) {
    for (Row row : results) {
      if (row.getString("keyspace_name").equals(namespace_name)
        && row.getString("table_name").equals(table_name)) {
        return true;
      }
    }
    return false;
  }

  @BeforeClass
  public static void setUpBeforeClass() throws Exception {
    BaseMiniClusterTest.tserverArgs.add(
        String.format("--placement_region=%s", PLACEMENT_REGION));
    BaseMiniClusterTest.tserverArgs.add(
        String.format("--placement_zone=%s", PLACEMENT_ZONE));
    BaseMiniClusterTest.tserverArgs.add("--cql_update_system_query_cache_msecs=0");
  }

  @Test
  public void testSystemSizeEstimatesTable() throws Exception {
    createTable("est_test");

    Iterator<Row> rows = session.execute("SELECT * FROM system.size_estimates WHERE " +
        "keyspace_name = '" + DEFAULT_TEST_KEYSPACE + "' AND table_name = 'est_test';").iterator();

    long rangeStart;
    long rangeEnd = Long.MIN_VALUE;
    int idx = 0;
    while (rows.hasNext()) {
      Row row = rows.next();
      // Range start should be equal to previous range end (starting from min long);
      rangeStart = Long.parseLong(row.getString("range_start"));
      assertEquals(rangeEnd, rangeStart);

      // Range start should be strictly smaller than range end, except for the last range which
      // cycles back to min long (because range end is an exclusive bound).
      rangeEnd = Long.parseLong(row.getString("range_end"));
      if (rows.hasNext()) {
        assertTrue(rangeStart < rangeEnd);
      } else {
        assertEquals(Long.MIN_VALUE, rangeEnd);
      }
      idx++;
    }

    // Expecting one row per tablet.
    assertEquals(NUM_TABLET_SERVERS * overridableNumShardsPerTServer(), idx);
  }

  @Test
  public void testSystemPeersTable() throws Exception {
    // Pick only 1 contact point since all will have same IP.
    List<InetSocketAddress> contactPoints = miniCluster.getCQLContactPoints();
    assertEquals(NUM_TABLET_SERVERS, contactPoints.size());

    ResultSet rs = session.execute("SELECT * FROM system.peers;");
    List <Row> rows = rs.all();
    verifyPeersTable(rows, false);

    rs = session.execute("SELECT * FROM system.peers WHERE peer = '181.123.12.1'");
    assertEquals(0, rs.all().size());

    rows = session.execute("SELECT peer, rpc_address, schema_version FROM system.peers").all();
    verifyPeersTable(rows, true);
    for (Row row : rows) {
      assertEquals(UUID.fromString(DEFAULT_SCHEMA_VERSION), row.getUUID("schema_version"));
    }

    // Now kill a tablet server and verify peers table has one less entry.
    miniCluster.killTabletServerOnHostPort(
        miniCluster.getTabletServers().keySet().iterator().next());

    // Wait for TServer to timeout.
    Thread.sleep(2 * miniCluster.getClusterParameters().getTServerHeartbeatTimeoutMs());

    // Now verify one tserver is missing.
    long start = System.currentTimeMillis();
    while (System.currentTimeMillis() - start < 30000) {
      try {
        rs = session.execute("SELECT * FROM system.peers;");
        break;
      } catch (Exception e) {
        // Ignoring exception since reconnection might be in progress.
        Thread.sleep(1000);
      }
    }

    // -2 since the node we are connected to won't be in the peers table and we killed one node
    // above.
    assertEquals(NUM_TABLET_SERVERS - 2, rs.all().size());

    // Start the tserver back up and wait for NUM_TABLET_SERVERS + 1 (since master never forgets
    // tservers).
    miniCluster.startTServer(BaseCQLTest.tserverArgs);
    assertTrue(miniCluster.waitForTabletServers(NUM_TABLET_SERVERS + 1));
  }

  @Test
  public void testEmptySystemTables() throws Exception {
    // Tables should be empty.
    assertEquals(0, session.execute("SELECT * FROM system_schema.aggregates;").all().size());
    assertEquals(0, session.execute("SELECT * FROM system_schema.functions;").all().size());
    assertEquals(0, session.execute("SELECT * FROM system_schema.indexes;").all().size());
    assertEquals(0, session.execute("SELECT * FROM system_schema.triggers;").all().size());
    assertEquals(0, session.execute("SELECT * FROM system_schema.types;").all().size());
    assertEquals(0, session.execute("SELECT * FROM system_schema.views;").all().size());
  }

  private void checkContactPoints(String column, Row row) {
    List<InetSocketAddress> contactPoints = miniCluster.getCQLContactPoints();
    boolean found = false;
    for (InetSocketAddress addr : contactPoints) {
      if (addr.getAddress().equals(row.getInet(column))) {
        found = true;
      }
    }
    assertTrue(found);
  }

  private Row getSingleRow(String stmt) {
    List <Row> results = session.execute(stmt).all();
    assertEquals(1, results.size());
    return results.get(0);
  }

  @Test
  public void testSystemLocalTables() throws Exception {
    Row row = getSingleRow("SELECT * FROM system.local WHERE key = 'local';");
    assertEquals("local", row.getString("key"));
    assertEquals("COMPLETED", row.getString("bootstrapped"));
    checkContactPoints("broadcast_address", row);
    assertEquals("local cluster", row.getString("cluster_name"));
    assertEquals("3.4.2", row.getString("cql_version"));
    assertEquals(PLACEMENT_REGION, row.getString("data_center"));
    assertEquals(0, row.getInt("gossip_generation"));
    checkContactPoints("listen_address", row);
    assertEquals("4", row.getString("native_protocol_version"));
    assertEquals(MURMUR_PARTITIONER, row.getString("partitioner"));
    assertEquals(PLACEMENT_ZONE, row.getString("rack"));
    assertEquals(RELEASE_VERSION, row.getString("release_version"));
    assertEquals(UUID.fromString(DEFAULT_SCHEMA_VERSION), row.getUUID("schema_version"));
    checkContactPoints("rpc_address", row);
    assertEquals("20.1.0", row.getString("thrift_version"));
    // no expectations on exact token values, but column must be set
    assertFalse(row.isNull("tokens"));

    // Verify where clauses and projections work.
    row = getSingleRow("SELECT tokens, partitioner, key FROM system.local;");
    assertEquals("local", row.getString("key"));
    assertEquals(MURMUR_PARTITIONER, row.getString("partitioner"));
    // no expectations on exact token values, but column must be set
    assertFalse(row.isNull("tokens"));
    assertFalse(row.getColumnDefinitions().contains("cluster_name"));
    assertFalse(row.getColumnDefinitions().contains("bootstrapped"));
    assertFalse(row.getColumnDefinitions().contains("broadcast_address"));
    assertFalse(row.getColumnDefinitions().contains("cql_version"));
    assertFalse(row.getColumnDefinitions().contains("data_center"));
    assertFalse(row.getColumnDefinitions().contains("gossip_generation"));
    assertFalse(row.getColumnDefinitions().contains("host_id"));
    assertFalse(row.getColumnDefinitions().contains("listen_address"));
    assertFalse(row.getColumnDefinitions().contains("native_protocol_version"));
    assertFalse(row.getColumnDefinitions().contains("rack"));
    assertFalse(row.getColumnDefinitions().contains("release_version"));
    assertFalse(row.getColumnDefinitions().contains("rpc_address"));
    assertFalse(row.getColumnDefinitions().contains("schema_version"));
    assertFalse(row.getColumnDefinitions().contains("thrift_version"));
    assertFalse(row.getColumnDefinitions().contains("truncated_at"));

    List <Row> results = session.execute(
      "SELECT * FROM system.local WHERE key = 'randomkey';").all();
    assertEquals(0, results.size());

    row = getSingleRow("SELECT partitioner FROM system.local WHERE key = 'local';");
    assertEquals(MURMUR_PARTITIONER, row.getString("partitioner"));

    row = getSingleRow("SELECT schema_version FROM system.local WHERE key='local'");
    assertEquals(UUID.fromString(DEFAULT_SCHEMA_VERSION), row.getUUID("schema_version"));

    // Where clause is intentionally missing here, since the system.local table should contain
    // only one row.
    row = getSingleRow("SELECT release_version FROM system.local");
    assertEquals(RELEASE_VERSION, row.getString("release_version"));
  }

  @Test
  public void testSystemKeyspacesAndTables() throws Exception {
    List <Row> results = session.execute(
      "SELECT * FROM system_schema.keyspaces;").all();
    Set<String> expectedKeySpaces = new HashSet<>(Arrays.asList(DEFAULT_TEST_KEYSPACE,
      "system_schema", "system", "system_auth"));
    assertEquals(expectedKeySpaces.size(), results.size());

    for (Row row : results) {
      assertTrue(row.getBool("durable_writes"));
      assertTrue(expectedKeySpaces.remove(row.getString("keyspace_name")));
      Map<String, String> repl_map = row.getMap("replication", String.class, String.class);
      // No expectation on exact values, but properties must be set.
      assertTrue(repl_map.containsKey("class"));
      assertTrue(repl_map.containsKey("replication_factor"));
    }
    assertEquals(0, expectedKeySpaces.size());

    results = session.execute(
      "SELECT * FROM system_schema.tables;").all();
    assertEquals(16, results.size());
    assertTrue(verifySystemSchemaTables(results, "system_schema", "aggregates"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "columns"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "functions"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "indexes"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "triggers"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "types"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "views"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "keyspaces"));
    assertTrue(verifySystemSchemaTables(results, "system_schema", "tables"));
    assertTrue(verifySystemSchemaTables(results, "system", "partitions"));
    assertTrue(verifySystemSchemaTables(results, "system", "peers"));
    assertTrue(verifySystemSchemaTables(results, "system", "local"));
    assertTrue(verifySystemSchemaTables(results, "system", "size_estimates"));
    assertTrue(verifySystemSchemaTables(results, "system_auth", "roles"));
    assertTrue(verifySystemSchemaTables(results, "system_auth", "role_permissions"));
    assertTrue(verifySystemSchemaTables(results, "system_auth", "resource_role_permissions_index"));

    // Create keyspace and table and verify it shows up.
    session.execute("CREATE KEYSPACE my_keyspace;");
    session.execute("CREATE TABLE my_table (c1 int PRIMARY KEY) WITH default_time_to_live = 5;");
    session.execute("CREATE TABLE my_keyspace.my_table (c1 int PRIMARY KEY);");

    // Verify results.
    results = session.execute(
      "SELECT keyspace_name, durable_writes FROM system_schema.keyspaces WHERE keyspace_name = " +
        "'my_keyspace';").all();
    assertEquals(1, results.size());
    assertEquals("my_keyspace", results.get(0).getString("keyspace_name"));
    assertTrue(results.get(0).getBool("durable_writes"));

    results = session.execute(
      String.format("SELECT keyspace_name, table_name, flags, default_time_to_live " +
          "FROM system_schema.tables WHERE keyspace_name = '%s' and table_name = 'my_table';",
          DEFAULT_TEST_KEYSPACE)).all();
    assertEquals(1, results.size());
    assertEquals(DEFAULT_TEST_KEYSPACE, results.get(0).getString("keyspace_name"));
    assertEquals("my_table", results.get(0).getString("table_name"));
    assertEquals(5, results.get(0).getInt("default_time_to_live"));
    assertEquals(new HashSet<String>(Arrays.asList("compound")),
      results.get(0).getSet("flags", String.class));

    results = session.execute(
        "SELECT keyspace_name, table_name, default_time_to_live FROM system_schema.tables " +
        "WHERE keyspace_name = 'my_keyspace' and table_name = 'my_table';").all();
    assertEquals(1, results.size());
    assertEquals("my_keyspace", results.get(0).getString("keyspace_name"));
    assertEquals("my_table", results.get(0).getString("table_name"));
    assertEquals(0, results.get(0).getInt("default_time_to_live"));
    assertFalse(results.get(0).getColumnDefinitions().contains("flags")); // flags was not selected.

    // Verify table id.
    results = session.execute("SELECT id FROM system_schema.tables WHERE keyspace_name = " +
      "'my_keyspace' and table_name = 'my_table'").all();
    assertEquals(1, results.size());
    byte[] table_uuid = null;
    for (Master.ListTablesResponsePB.TableInfo tableInfo :
      miniCluster.getClient().getTablesList("my_table").getTableInfoList()) {
      if (tableInfo.getNamespace().getName().equals("my_keyspace") &&
        tableInfo.getName().equals("my_table")) {
        table_uuid = tableInfo.getId().toByteArray();
      }
    }

    // Flip two adjacent chars (so that we reverse bytes correctly below).
    for (int i = 0; i + 1 < table_uuid.length; i+=2) {
      byte tmp = table_uuid[i];
      table_uuid[i] = table_uuid[i + 1];
      table_uuid[i + 1] = tmp;
    }

    assertNotNull(table_uuid);
    assertEquals(32, table_uuid.length);
    // Reverse bytes since we have UUID in host byte order in TableInfo, but network byte order
    // in the system table.
    String uuid = new StringBuilder(new String(table_uuid)).reverse().toString();

    // Insert hyphens.
    uuid = String.format("%s-%s-%s-%s-%s", uuid.substring(0, 8), uuid.substring
      (8, 12), uuid.substring(12, 16), uuid.substring(16, 20), uuid.substring
      (20, 32));
    assertEquals(UUID.fromString(uuid), results.get(0).getUUID("id"));
  }

  private void verifyColumnSchema(Row row, String table_name, String column_name, String kind,
                                  int position, String type, String clustering_order) {
    assertEquals(DEFAULT_TEST_KEYSPACE, row.getString("keyspace_name"));
    assertEquals(table_name, row.getString("table_name"));
    assertEquals(column_name, row.getString("column_name"));
    assertEquals(clustering_order, row.getString("clustering_order"));
    assertEquals(kind, row.getString("kind"));
    assertEquals(position, row.getInt("position"));
    assertEquals(type, row.getString("type"));
  }

  @Test
  public void testSystemColumnsTable() throws Exception {
    session.execute("CREATE TABLE many_columns (c1 int, c2 text, c3 int, c4 text, c5 int, c6 int," +
      " c7 map <text, text>, c8 list<text>, c9 set<int>, PRIMARY KEY((c1, c2, c3), c4, c5, c6)) " +
      "WITH CLUSTERING ORDER BY (c4 DESC);");
    List<Row> results = session.execute(String.format("SELECT * FROM system_schema.columns WHERE " +
      "keyspace_name = '%s' AND table_name = 'many_columns'", DEFAULT_TEST_KEYSPACE)).all();
    assertEquals(9, results.size());
    verifyColumnSchema(results.get(0), "many_columns", "c1", "partition_key", 0, "int", "none");
    verifyColumnSchema(results.get(1), "many_columns", "c2", "partition_key", 1, "text", "none");
    verifyColumnSchema(results.get(2), "many_columns", "c3", "partition_key", 2, "int", "none");
    verifyColumnSchema(results.get(3), "many_columns", "c4", "clustering", 0, "text", "desc");
    verifyColumnSchema(results.get(4), "many_columns", "c5", "clustering", 1, "int", "asc");
    verifyColumnSchema(results.get(5), "many_columns", "c6", "clustering", 2, "int", "asc");
    verifyColumnSchema(results.get(6), "many_columns", "c7", "regular", -1, "map<text, text>",
      "none");
    verifyColumnSchema(results.get(7), "many_columns", "c8", "regular", -1, "list<text>", "none");
    verifyColumnSchema(results.get(8), "many_columns", "c9", "regular", -1, "set<int>", "none");

    // Verify SELECT * works.
    results = session.execute("SELECT * FROM system_schema.columns").all();
    assertTrue(results.size() > 9);

    // Test counter column.
    session.execute("CREATE TABLE counter_column (k int PRIMARY KEY, c counter);");
    results = session.execute(String.format("SELECT * FROM system_schema.columns WHERE " +
      "keyspace_name = '%s' AND table_name = 'counter_column'", DEFAULT_TEST_KEYSPACE)).all();
    assertEquals(2, results.size());
    verifyColumnSchema(results.get(0), "counter_column", "k", "partition_key", 0, "int", "none");
    verifyColumnSchema(results.get(1), "counter_column", "c", "regular", -1, "counter", "none");
  }

  @Test
  public void testSchemaUpdate() throws Exception {
    // Create keyspace and table.
    session.execute("CREATE KEYSPACE test_keyspace;");
    session.execute("USE test_keyspace;");
    session.execute("CREATE TABLE test_table (k int primary key, c int);");

    // Look up and verify the keyspace and table metadata by the CQL statement generated.
    String cql = cluster.getMetadata()
                        .getKeyspace("test_keyspace")
                        .getTable("test_table")
                        .exportAsString();
    assertTrue("CQL of test_table mismatch: " + cql,
               cql.startsWith("CREATE TABLE test_keyspace.test_table (\n"+
                              "    k int,\n" +
                              "    c int,\n" +
                              "    PRIMARY KEY (k)\n"+
                              ")"));

    // Drop the table. Verify its metadata has been removed.
    session.execute("DROP TABLE test_table;");
    assertNull(cluster.getMetadata().getKeyspace("test_keyspace").getTable("test_table"));

    // Drop the keyspace. Verify its metadata has been removed.
    session.execute("DROP KEYSPACE test_keyspace;");
    assertNull(cluster.getMetadata().getKeyspace("test_keyspace"));
  }

  private void testSystemSchemaPartitionsTable(int vtable_refresh_secs) throws Exception {
    // Create test table.
    session.execute("CREATE KEYSPACE test_keyspace;");
    session.execute("CREATE TABLE test_keyspace.test_table (k int PRIMARY KEY);");

    // Select partitions of test table.
    // Due to the cache being refreshed every vtable_refresh_secs, need to wait for that.
    final Stopwatch stopwatch = Stopwatch.createStarted();
    boolean success = false;
    List<Row> partitions = Collections.emptyList();
    // Run at least once (in case vtable_refresh_secs is 0).
    do {
      Thread.sleep(1000);
      partitions = session.execute("SELECT * FROM system.partitions WHERE " +
                                             "keyspace_name = 'test_keyspace' AND " +
                                             "table_name = 'test_table';").all();
      success = miniCluster.getNumShardsPerTserver() * NUM_TABLET_SERVERS == partitions.size();
    } while (!success && stopwatch.elapsed(SECONDS) < vtable_refresh_secs);
    assertEquals(miniCluster.getNumShardsPerTserver() * NUM_TABLET_SERVERS,
                 partitions.size());

    HashSet<InetAddress> contactPoints = new HashSet<>();
    for (InetSocketAddress contactPoint : miniCluster.getCQLContactPoints()) {
      contactPoints.add(contactPoint.getAddress());
    }

    ByteBuffer startKey = null;
    ByteBuffer endKey = null;
    for (Row partition : partitions) {
      // Verify the first start_key is empty and subsequent ones equal the previous end_key
      startKey = partition.getBytes("start_key");
      if (endKey == null) {
        assertFalse(startKey.hasRemaining());
      } else {
        assertEquals(startKey, endKey);
      }

      // Verify start_key and end_key of each partition are not the same.
      endKey = partition.getBytes("end_key");
      assertNotEquals(startKey, endKey);

      int leader = 0;
      int followers = 0;
      Map<InetAddress, String> replicaAddresses = partition.getMap("replica_addresses",
                                                                   InetAddress.class,
                                                                   String.class);
      for (Map.Entry<InetAddress, String> entry : replicaAddresses.entrySet()) {
        // Verify replica_addresses are in the CQL contact points.
        assertTrue(contactPoints.contains(entry.getKey()));
        if (entry.getValue().equals("LEADER")) {
          leader++;
        } else if (entry.getValue().equals("FOLLOWER")) {
          followers++;
        }
      }
      // Verify there are 1 leader and 2 followers in each partition.
      assertEquals(1, leader);
      assertEquals(2, followers);
    }
    // Verify the last end_key is empty.
    assertFalse(endKey.hasRemaining());
  }

  @Test
  public void testSystemSchemaPartitionsTableWithVtableRefresh() throws Exception {
    testSystemSchemaPartitionsTable(SYSTEM_PARTITIONS_REFRESH_SECS /* vtable_refresh_secs */);
  }

  @Test
  public void testSystemSchemaPartitionsTableWithoutVtableRefresh() throws Exception {
    destroyMiniCluster();
    // Testing with partitions_vtable_cache_refresh_secs flag disabled.
    masterArgs.add("--partitions_vtable_cache_refresh_secs=0");
    createMiniCluster();
    setUpCqlClient();

    testSystemSchemaPartitionsTable(0 /* vtable_refresh_secs */);

    masterArgs.remove("--partitions_vtable_cache_refresh_secs=0");
  }

  private List<String> getRowsAsStringList(ResultSet rs) {
    return rs.all().stream().map(Row::toString).collect(Collectors.toList());
  }

  @Test
  public void testLimitOffsetPageSize() throws Exception {
    SimpleStatement select_all = new SimpleStatement("select * from system_schema.columns");
    ResultSet rs = session.execute(select_all);
    List<String> all_rows = getRowsAsStringList(rs);

    rs = session.execute("select * from system_schema.columns limit 15");
    assertEquals(all_rows.subList(0, 15), getRowsAsStringList(rs));

    rs = session.execute("select * from system_schema.columns offset 15 limit 30");
    assertEquals(all_rows.subList(15, 45), getRowsAsStringList(rs));

    rs = session.execute("select * from system_schema.columns offset 45");
    assertEquals(all_rows.subList(45, all_rows.size()), getRowsAsStringList(rs));

    // Paging is not supported for system tables so page size should be ignored (return all rows).
    select_all.setFetchSize(10);
    rs = session.execute(select_all);
    assertEquals(all_rows, getRowsAsStringList(rs));
  }

  @Test
  public void testCorrectErrorForSystemPeersV2() throws Exception {
    try {
      SimpleStatement select = new SimpleStatement("select * from system.peers_v2");
      ResultSet rs = session.execute(select);
    } catch (ServerError se) {
      LOG.info("Sudo length of the string" + se.getCause().toString().length());
      if (!se.getCause().toString().contains("Unknown keyspace/cf pair (system.peers_v2)")) {
        throw se;
      }
    }
  }
}
