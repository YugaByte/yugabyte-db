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

import com.datastax.driver.core.*;
import com.datastax.driver.core.policies.DCAwareRoundRobinPolicy;
import com.datastax.driver.core.querybuilder.QueryBuilder;
import com.google.common.net.HostAndPort;
import com.yugabyte.driver.core.policies.PartitionAwarePolicy;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.*;
import org.yb.client.*;
import org.yb.consensus.Metadata;
import org.yb.minicluster.Metrics;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.MiniYBDaemon;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

import static junit.framework.TestCase.assertEquals;
import static org.yb.AssertionWrappers.assertNotNull;
import static org.yb.AssertionWrappers.assertTrue;

@RunWith(value=YBTestRunner.class)
public class TestConsistencyLevels extends BaseCQLTest {
  protected static final Logger LOG = LoggerFactory.getLogger(TestConsistencyLevels.class);

  private static final String TABLE_NAME = "test_consistency";

  private static final int NUM_ROWS = 1000;

  private static final int NUM_OPS = 100;

  private static final int WAIT_FOR_REPLICATION_TIMEOUT_MS = 30000;

  private YBTable ybTable = null;

  private LocatedTablet tablet = null;

  private static final String REGION_PREFIX = "region";

  private static final String CQLPROXY_SELECT_METRIC =
    "handler_latency_yb_cqlserver_SQLProcessor_SelectStmt";

  @Override
  public void useKeyspace() throws Exception {
    // Use the DEFAULT_TEST_KEYSPACE for this test.
  }

  @Override
  protected void afterBaseCQLTestTearDown() throws Exception {
    // We need to destroy the mini cluster since we don't want metrics from one test to interfere
    // with another.
    destroyMiniCluster();
  }

  protected void createMiniClusterWithSameRegion() throws Exception {
    // Create a cluster with tservers in the same region.
    List<List<String>> tserverArgs = new ArrayList<>();
    for (int i = 0; i < NUM_TABLET_SERVERS; i++) {
      tserverArgs.add(Arrays.asList(String.format("--placement_region=%s%d", REGION_PREFIX, 1)));
    }
    createMiniCluster(1, tserverArgs);

  }

  protected void createMiniClusterWithPlacementRegion() throws Exception {
    // Create a cluster with tservers in different regions.
    List<List<String>> tserverArgs = new ArrayList<>();
    for (int i = 0; i < NUM_TABLET_SERVERS; i++) {
      tserverArgs.add(Arrays.asList(String.format("--placement_region=%s%d", REGION_PREFIX, i)));
    }
    createMiniCluster(1, tserverArgs);
  }

  @Before
  public void setUpTable() throws Exception {
    // Create a table using YBClient to enforce a single tablet.
    YBClient client = miniCluster.getClient();
    ColumnSchema.ColumnSchemaBuilder hash_column =
      new ColumnSchema.ColumnSchemaBuilder("h", Type.INT32);
    hash_column.hashKey(true);
    ColumnSchema.ColumnSchemaBuilder range_column =
      new ColumnSchema.ColumnSchemaBuilder("r", Type.INT32);
    range_column.rangeKey(true, ColumnSchema.SortOrder.ASC);
    ColumnSchema.ColumnSchemaBuilder regular_column =
      new ColumnSchema.ColumnSchemaBuilder("k", Type.INT32);

    CreateTableOptions options = new CreateTableOptions();
    options.setNumTablets(1);
    options.setTableType(Common.TableType.YQL_TABLE_TYPE);
    ybTable = client.createTable(DEFAULT_TEST_KEYSPACE, TABLE_NAME, new Schema(
      Arrays.asList(hash_column.build(), range_column.build(), regular_column.build())), options);

    // Verify number of replicas.
    List<LocatedTablet> tablets = ybTable.getTabletsLocations(0);
    assertEquals(1, tablets.size());
    tablet = tablets.get(0);

    // Insert some rows.
    for (int idx = 0; idx < NUM_ROWS; idx++) {
      // INSERT: Valid statement with column list.
      String insert_stmt = String.format(
        "INSERT INTO %s.%s(h, r, k) VALUES(%d, %d, %d);", DEFAULT_TEST_KEYSPACE, TABLE_NAME,
        idx, idx, idx);
      session.execute(insert_stmt);
    }
  }

  @Test
  public void testReadFromLeader() throws Exception {
    // Read from the leader.
    for (int i = 0; i < NUM_OPS; i++) {
      ConsistencyLevel consistencyLevel =
        (i % 2 == 0) ? ConsistencyLevel.LOCAL_ONE : ConsistencyLevel.QUORUM;
      assertTrue(verifyNumRows(consistencyLevel));
    }

    // Verify all reads went to the leader.
    Map<HostAndPort, MiniYBDaemon> tservers = miniCluster.getTabletServers();
    assertEquals(tservers.size(), tablet.getReplicas().size());
    for (LocatedTablet.Replica replica : tablet.getReplicas()) {
      String host = replica.getRpcHost();
      int webPort = tservers.get(HostAndPort.fromParts(host, replica.getRpcPort())).getWebPort();
      Metrics metrics = new Metrics(host, webPort, "server");
      long numOps = metrics.getHistogram(TSERVER_READ_METRIC).totalCount;
      if (replica.getRole().equals(Metadata.RaftPeerPB.Role.LEADER.toString())) {
        assertEquals(NUM_OPS, numOps);
      } else {
        assertEquals(0, numOps);
      }
    }
  }

  private boolean verifyNumRows(ConsistencyLevel consistencyLevel) {
    Statement statement = QueryBuilder.select()
      .from(DEFAULT_TEST_KEYSPACE, TABLE_NAME)
      .setConsistencyLevel(consistencyLevel);
    return NUM_ROWS == session.execute(statement).all().size();
  }

  @Test
  public void testReadFromFollowers() throws Exception {
    // Wait for replicas to converge. Assuming 10 ops will end up hitting each tserver.
    for (int i = 0; i < 10; i++) {
      TestUtils.waitFor(() -> {
        // Ensure the condition is consistently true before returning.
        for (int j = 0; j < 10; j++) {
          if (!verifyNumRows(ConsistencyLevel.ONE)) {
            return false;
          }
        }
        return true;
      }, WAIT_FOR_REPLICATION_TIMEOUT_MS);
    }

    // Read from any replica.
    for (int i = 0; i < NUM_OPS; i++) {
      assertTrue(verifyNumRows(ConsistencyLevel.ONE));
    }

    // Verify reads were spread across all replicas.
    Map<HostAndPort, MiniYBDaemon> tservers = miniCluster.getTabletServers();
    long totalOps = 0;
    for (LocatedTablet.Replica replica: tablet.getReplicas()) {
      String host = replica.getRpcHost();
      int webPort = tservers.get(HostAndPort.fromParts(host, replica.getRpcPort())).getWebPort();

      Metrics metrics = new Metrics(host, webPort, "server");
      long numOps = metrics.getHistogram(TSERVER_READ_METRIC).totalCount;
      LOG.info("Num ops for tserver: " + replica.toString() + " : " + numOps);
      totalOps += numOps;
      // At least some ops went to each server.
      assertTrue(numOps > NUM_OPS/10);
    }
    assertTrue(totalOps >= NUM_OPS);
  }

  @Test
  public void testOtherCQLConsistencyLevels() throws Exception {
    // Expecting correct result for CQL compatibility, but no guarantee of how reads are spread.
    assertTrue(verifyNumRows(ConsistencyLevel.ALL));
    assertTrue(verifyNumRows(ConsistencyLevel.ANY));
    assertTrue(verifyNumRows(ConsistencyLevel.EACH_QUORUM));
    assertTrue(verifyNumRows(ConsistencyLevel.LOCAL_QUORUM));
    assertTrue(verifyNumRows(ConsistencyLevel.LOCAL_SERIAL));
    assertTrue(verifyNumRows(ConsistencyLevel.SERIAL));
    assertTrue(verifyNumRows(ConsistencyLevel.THREE));
    assertTrue(verifyNumRows(ConsistencyLevel.TWO));
  }

  @Test
  public void testRegionLocalReads() throws Exception {
    // Destroy existing cluster and recreate it.
    destroyMiniCluster();

    // Set a high refresh interval, so that this does not mess with our metrics.
    MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS = Integer.MAX_VALUE;
    createMiniClusterWithPlacementRegion();
    setUpCqlClient();
    setUpTable();

    // Now lets create a client in one of the DC's and ensure all reads go to the same
    // proxy/tserver.
    for (int i = 0 ; i < NUM_TABLET_SERVERS; i++) {
      Cluster cluster = Cluster.builder()
        .addContactPointsWithPorts(miniCluster.getCQLContactPoints())
        .withLoadBalancingPolicy(new PartitionAwarePolicy(
          DCAwareRoundRobinPolicy.builder()
          .withLocalDc(REGION_PREFIX + i)
          .withUsedHostsPerRemoteDc(Integer.MAX_VALUE)
          .build())).build();
      Session session = cluster.connect();

      // Find the tserver, cqlproxy for this region.
      HostAndPort tserver_webaddress = null;
      HostAndPort cqlproxy_webaddress = null;
      for (Map.Entry<HostAndPort, MiniYBDaemon> entry : miniCluster.getTabletServers().entrySet()) {
        for (String cmdline : entry.getValue().getCommandLine()) {
          if (cmdline.contains(REGION_PREFIX + i)) {
            tserver_webaddress = entry.getValue().getWebHostAndPort();
            cqlproxy_webaddress = HostAndPort.fromParts(entry.getKey().getHost(), entry
              .getValue().getCqlWebPort());
          }
        }
      }
      assertNotNull(tserver_webaddress);
      assertNotNull(cqlproxy_webaddress);

      // Note the current metrics.
      Metrics tserverMetrics = new Metrics(tserver_webaddress.getHost(),
        tserver_webaddress.getPort(), "server");
      long tserverNumOpsBefore = tserverMetrics.getHistogram(TSERVER_READ_METRIC).totalCount;
      Metrics cqlproxyMetrics = new Metrics(cqlproxy_webaddress.getHost(),
        cqlproxy_webaddress.getPort(), "server");
      long cqlproxyNumOpsBefore = cqlproxyMetrics.getHistogram(CQLPROXY_SELECT_METRIC).totalCount;

      // Perform a number of reads.
      Statement statement = QueryBuilder.select()
        .from(DEFAULT_TEST_KEYSPACE, TABLE_NAME)
        .setConsistencyLevel(ConsistencyLevel.ONE);
      for (int j = 0; j < NUM_OPS; j++) {
        session.execute(statement);
      }

      // Verify all reads went to the same tserver and cql proxy. There could be some background
      // reads for system tables which increase the number of ops beyond NUM_OPS and hence we
      // assert that we received atleast NUM_OPS instead of exactly NUM_OPS.
      tserverMetrics = new Metrics(tserver_webaddress.getHost(), tserver_webaddress.getPort(),
        "server");
      assertTrue(NUM_OPS <=
        tserverMetrics.getHistogram(TSERVER_READ_METRIC).totalCount - tserverNumOpsBefore);

      cqlproxyMetrics = new Metrics(cqlproxy_webaddress.getHost(),
        cqlproxy_webaddress.getPort(), "server");
      assertTrue(NUM_OPS <=
        cqlproxyMetrics.getHistogram(CQLPROXY_SELECT_METRIC).totalCount - cqlproxyNumOpsBefore);
    }
  }

  @Test
  public void testRoundRobinBetweenReplicas() throws Exception {
    // Destroy existing cluster and recreate it.
    destroyMiniCluster();

    // Set a high refresh interval, so that this does not mess with our metrics.
    MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS = Integer.MAX_VALUE;
    createMiniClusterWithSameRegion();
    setUpCqlClient();
    setUpTable();

    String stmt = "select * from " + DEFAULT_TEST_KEYSPACE + "." + TABLE_NAME + ";";
    PreparedStatement prepare_stmt = session.prepare(stmt).
                                     setConsistencyLevel(ConsistencyLevel.ONE);
    for (int j = 0; j < NUM_OPS; j++) {
      session.execute(prepare_stmt.bind());
    }

    Map<HostAndPort, MiniYBDaemon> tservers = miniCluster.getTabletServers();
    long totalOps = 0;
    long maxOps = 0, minOps = NUM_OPS + 1;
    HostAndPort cqlproxy_webaddress = null;
    for (Map.Entry<HostAndPort, MiniYBDaemon> entry: tservers.entrySet()) {
      cqlproxy_webaddress = HostAndPort.fromParts(entry.getKey().getHost(), entry
              .getValue().getCqlWebPort());
      Metrics cqlproxyMetrics = new Metrics(cqlproxy_webaddress.getHost(),
              cqlproxy_webaddress.getPort(), "server");
      long cqlproxyNumOps = cqlproxyMetrics.getHistogram(CQLPROXY_SELECT_METRIC).totalCount;
      assertTrue(cqlproxyNumOps > NUM_OPS/(STANDARD_DEVIATION_FACTOR*NUM_TABLET_SERVERS));
      LOG.info("CQL Proxy Num Ops: " + cqlproxyNumOps);
    }

    for (LocatedTablet.Replica replica : tablet.getReplicas()) {
      String host = replica.getRpcHost();
      int webPort = tservers.get(HostAndPort.fromParts(host, replica.getRpcPort())).getWebPort();

      Metrics metrics = new Metrics(host, webPort, "server");
      long numOps = metrics.getHistogram(TSERVER_READ_METRIC).totalCount;
      LOG.info("Num ops for tserver: " + replica.toString() + " : " + numOps);
      totalOps += numOps;
      if (numOps > maxOps) {
        maxOps = numOps;
      }
      if (numOps < minOps) {
        minOps = numOps;
      }
      assertTrue(numOps > NUM_OPS/(STANDARD_DEVIATION_FACTOR*NUM_TABLET_SERVERS));
    }
    assertTrue(totalOps <= NUM_OPS);
    assertTrue((maxOps - minOps) < NUM_OPS/(STANDARD_DEVIATION_FACTOR*NUM_TABLET_SERVERS));
  }
}
