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
package org.yb.loadtester;

import com.datastax.driver.core.Host;
import com.google.common.net.HostAndPort;
import com.yugabyte.sample.Main;
import com.yugabyte.sample.apps.CassandraStockTicker;
import com.yugabyte.sample.common.CmdLineOpts;
import org.junit.After;
import org.junit.Before;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.Common;
import org.yb.client.*;
import org.yb.cql.BaseCQLTest;
import org.yb.minicluster.Metrics;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.MiniYBDaemon;

import java.util.*;

import static junit.framework.TestCase.*;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertLessThan;
import static org.yb.AssertionWrappers.fail;

public class TestClusterBase extends BaseCQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestClusterBase.class);

  protected LoadTester loadTesterRunnable = null;

  protected Thread loadTesterThread = null;

  protected String cqlContactPoints = null;

  protected YBClient client = null;

  protected static final String WORKLOAD = "CassandraStockTicker";

  // Number of ops to wait for between significant events in the test.
  protected static final int NUM_OPS_INCREMENT = 1000;

  // Timeout to wait for desired number of ops.
  protected static final int WAIT_FOR_OPS_TIMEOUT_MS = 60000; // 60 seconds

  // Timeout to wait for load balancing to complete.
  protected static final int LOADBALANCE_TIMEOUT_MS = 240000; // 4 mins

  // Timeout to wait for new servers to startup and join the cluster.
  protected static final int WAIT_FOR_SERVER_TIMEOUT_MS = 60000; // 60 seconds.

  // Timeout to wait for cluster move to complete.
  protected static final int CLUSTER_MOVE_TIMEOUT_MS = 300000; // 5 mins

  // Timeout for test completion.
  protected static final int TEST_TIMEOUT_SEC = 1200; // 20 mins

  // Timeout to wait for a new master to come up.
  protected static final int NEW_MASTER_TIMEOUT_MS = 10000; // 10 seconds.

  // Timeout to wait for webservers to be up.
  protected static final int WEBSERVER_TIMEOUT_MS = 10000; // 10 seconds.

  // Timeout to wait for a master leader.
  protected static final int MASTER_LEADER_TIMEOUT_MS = 10000; // 10 seconds.

  // Timeout to wait expected number of tservers to be alive.
  protected static final int EXPECTED_TSERVERS_TIMEOUT_MS = 30000; // 30 seconds.

  // Maximum number of exceptions to tolerate. We might see exceptions during a full master move,
  // since the newly elected leader might not have received heartbeats from any tservers. As a
  // result, the system.peers table would be empty and any client reading the table in that state
  // would end up removing all the hosts from its metadata causing exceptions.
  protected static final int MAX_NUM_EXCEPTIONS = 20;

  @Override
  public int getTestMethodTimeoutSec() {
    // No need to adjust for TSAN vs. non-TSAN here, it will be done automatically.
    return TEST_TIMEOUT_SEC;
  }

  void updateMiniClusterClient() throws Exception {
    miniCluster.startSyncClient(true /* waitForMasterLeader */);
    client = miniCluster.getClient();
  }

  @Override
  protected Map<String, String> getMasterFlags() {
    Map<String, String> flags = super.getMasterFlags();
    // Speed up the load balancer.
    flags.put("load_balancer_max_concurrent_adds", "5");
    // TODO(bogdan): commented out until we figure out #4412.
    /*
    flags.put("load_balancer_max_over_replicated_tablets", "5");
    flags.put("load_balancer_max_concurrent_removals", "5");
    flags.put("load_balancer_max_concurrent_moves", "5");
    */
    return flags;
  }

  @Override
  protected void afterStartingMiniCluster() throws Exception {
    cqlContactPoints = miniCluster.getCQLContactPointsAsString();
    client = miniCluster.getClient();
    LOG.info("Retrieved contact points from cluster: " + cqlContactPoints);
    updateConfigReplicationFactor(NUM_MASTERS);
  }

  @Before
  public void startLoadTester() throws Exception {
    // Start the load tester.
    LOG.info("Using contact points for load tester: " + cqlContactPoints);
    loadTesterRunnable = new LoadTester(WORKLOAD, cqlContactPoints, 1, 1);
    loadTesterThread = new Thread(loadTesterRunnable);
    loadTesterThread.start();
    LOG.info("Loadtester start.");
  }

  @After
  public void stopLoadTester() throws Exception {
    // Stop load tester and exit.
    if (loadTesterRunnable != null) {
      loadTesterRunnable.stopLoadTester();
      loadTesterRunnable = null;
      LOG.info("Loadtester stopped.");
    }

    if (loadTesterThread != null) {
      LOG.info("Waiting for load tester thread to exit");
      loadTesterThread.join();
      LOG.info("Load tester thread completed!");
      loadTesterThread = null;
    }
  }

  @Override
  protected void afterBaseCQLTestTearDown() throws Exception {
    // We need to destroy the mini cluster after BaseCQLTest cleans up all the tables and keyspaces.
    destroyMiniCluster();
    LOG.info("Stopped minicluster.");
  }

  protected class LoadTester implements Runnable {
    private final Main testRunner;

    private volatile boolean testRunnerFailed = false;

    public LoadTester(String workload,
                      String cqlContactPoints,
                      int numThreadsRead,
                      int numThreadsWrite) throws Exception {
      String args[] = {"--workload", workload, "--nodes", cqlContactPoints,
        "--print_all_exceptions", "--num_threads_read", Integer.toString(numThreadsRead),
        "--num_threads_write", Integer.toString(numThreadsWrite)};
      CmdLineOpts configuration = CmdLineOpts.createFromArgs(args);
      testRunner = new Main(configuration);
      LOG.info(String.format(
          "Started a LoadTester instance with %d reader threads, and %d writer threads",
          numThreadsRead, numThreadsWrite));
    }

    public void stopLoadTester() {
      testRunner.stopAllThreads();
      testRunner.resetOps();
      testRunner.terminate();
    }

    @Override
    public void run() {
      try {
        testRunner.run();
        LOG.info("Load tester completed!");
      } catch (Exception e) {
        LOG.error("Load tester exited!", e);
        testRunnerFailed = true;
      }
    }

    public boolean hasFailures() {
      return testRunner.hasFailures() || testRunnerFailed;
    }

    public boolean hasThreadFailed() {
      return testRunner.hasThreadFailed();
    }

    private int getNumExceptions() {
      return testRunner.getNumExceptions();
    }

    public void waitNumOpsIncrement(long incrementNumOps) throws Exception {
      long expectedNumOps = testRunner.numOps() + incrementNumOps;
      TestUtils.waitFor(() -> {
        long numOps = testRunner.numOps();
        if (numOps >= expectedNumOps)
          return true;
        LOG.info("Current num ops: " + numOps + ", expected: " + expectedNumOps);
        return false;
      }, WAIT_FOR_OPS_TIMEOUT_MS);
      LOG.info("Num Ops: " + testRunner.numOps() + ", Expected: " + expectedNumOps);
      assertTrue(testRunner.numOps() >= expectedNumOps);
    }

    public void verifyNumExceptions() {
      assertTrue(String.format("Number of exceptions: %d", getNumExceptions()),
        getNumExceptions() < MAX_NUM_EXCEPTIONS && !hasThreadFailed());
    }
  }

  private void verifyMetrics(int minOps) throws Exception {
    StringBuilder failureMessage = new StringBuilder();
    for (MiniYBDaemon ts : miniCluster.getTabletServers().values()) {
      // Wait for the webserver to be up.
      TestUtils.waitForServer(ts.getLocalhostIP(), ts.getCqlWebPort(), WEBSERVER_TIMEOUT_MS);
      Metrics metrics = new Metrics(ts.getLocalhostIP(), ts.getCqlWebPort(), "server");
      long numOps =
          metrics.getHistogram(
              "handler_latency_yb_cqlserver_SQLProcessor_InsertStmt").totalCount +
          metrics.getHistogram(
              "handler_latency_yb_cqlserver_SQLProcessor_SelectStmt").totalCount +
          metrics.getHistogram(
              "handler_latency_yb_cqlserver_SQLProcessor_UpdateStmt").totalCount;

      LOG.info("TSERVER: " + ts.getLocalhostIP() + ", num_ops: " + numOps + ", min_ops: " + minOps);
      if (numOps < minOps) {
        String msg = "Assertion failed: expected numOps >= minOps, found numOps=" + numOps +
            ", minOps=" + minOps + " for " + ts;
        if (failureMessage.length() > 0) {
          failureMessage.append('\n');
        }
        failureMessage.append(msg);
        LOG.error(msg);
      }
    }

    if (failureMessage.length() > 0) {
      throw new AssertionError(failureMessage.toString());
    }
  }

  private void verifyExpectedLiveTServers(int expected_live) throws Exception {
    // Wait for metrics to be submitted.
    Thread.sleep(2 * MiniYBCluster.CATALOG_MANAGER_BG_TASK_WAIT_MS);

    // Wait for expected number of live tservers to be present since leader elections could
    // result in the metric not being correct momentarily.
    TestUtils.waitFor(() -> {
      client.waitForMasterLeader(MASTER_LEADER_TIMEOUT_MS);

      // Retrieve web address for leader master.
      HostAndPort masterHostAndPort = client.getLeaderMasterHostAndPort();
      assertNotNull(masterHostAndPort);
      Map<HostAndPort, MiniYBDaemon> masters = miniCluster.getMasters();
      assertTrue(String.format("Couldn't find master %s in list %s", masterHostAndPort,
        masters.keySet().toString()), masters.containsKey(masterHostAndPort));
      int masterLeaderWebPort = masters.get(masterHostAndPort).getWebPort();
      Metrics metrics = new Metrics(masterHostAndPort.getHost(), masterLeaderWebPort,
        "cluster");
      int live_tservers = metrics.getCounter("num_tablet_servers_live").value;
      LOG.info("Live tservers: " + live_tservers + ", expected: " + expected_live);
      return live_tservers == expected_live;
    }, EXPECTED_TSERVERS_TIMEOUT_MS);
  }

  protected Set<HostAndPort> createAndAddNewMasters(int numMasters) throws Exception {
    Set<HostAndPort> newMasters = new HashSet<>();
    for (int i = 0; i < numMasters; i++) {
      // Add new master.
      HostAndPort masterRpcHostPort = miniCluster.startShellMaster();

      newMasters.add(masterRpcHostPort);

      // Wait for new master to be online.
      assertTrue(client.waitForMaster(masterRpcHostPort, NEW_MASTER_TIMEOUT_MS));

      LOG.info("New master online: " + masterRpcHostPort.toString());

      // Add new master to the config.
      ChangeConfigResponse response = client.changeMasterConfig(masterRpcHostPort.getHost(),
        masterRpcHostPort.getPort(), true);
      assertFalse("ChangeConfig has error: " + response.errorMessage(), response.hasError());

      LOG.info("Added new master to config: " + masterRpcHostPort.toString());

      // Wait for hearbeat interval to ensure tservers pick up the new masters.
      Thread.sleep(2 * MiniYBCluster.TSERVER_HEARTBEAT_INTERVAL_MS);

      LOG.info("Done waiting for new leader");

      // Verify no load tester errors.
      loadTesterRunnable.verifyNumExceptions();
    }
    return newMasters;
  }

  public void performFullMasterMove() throws Exception {
    // Create a copy to store original list.
    Map<HostAndPort, MiniYBDaemon> originalMasters = new HashMap<>(miniCluster.getMasters());
    for (HostAndPort originalMaster : originalMasters.keySet()) {
      // Add new master.
      HostAndPort masterRpcHostPort = miniCluster.startShellMaster();
      addMaster(masterRpcHostPort);
      removeMaster(originalMaster);
    }
  }

  public Set<HostAndPort> startNewMasters(int numMasters) throws Exception {
    Set<HostAndPort> newMasters = new HashSet<>();
    LOG.info("Attempting to start " + numMasters + " new masters");
    for (int i = 0; i < numMasters; i++) {
      // Add new master.
      HostAndPort masterRpcHostPort = miniCluster.startShellMaster();
      LOG.info("Starting a new shell master (#" + i + " in this batch) on host/port " +
          masterRpcHostPort + " with a timeout of " + NEW_MASTER_TIMEOUT_MS + " ms");

      newMasters.add(masterRpcHostPort);
      // Wait for new master to be online.
      boolean waitSuccessful = client.waitForMaster(masterRpcHostPort, NEW_MASTER_TIMEOUT_MS);
      if (!waitSuccessful) {
        LOG.error("Timed out waiting for master on host/port " + masterRpcHostPort +
            " to come up. Waited for " + NEW_MASTER_TIMEOUT_MS + " ms.");
      }
      assertTrue(waitSuccessful);

      LOG.info("New master online: " + masterRpcHostPort.toString());
    }
    return newMasters;
  }

  protected void addMaster(HostAndPort newMaster) throws Exception {
    ChangeConfigResponse response = client.changeMasterConfig(newMaster.getHost(),
        newMaster.getPort(), true);
    assertFalse("ChangeConfig has error: " + response.errorMessage(), response.hasError());

    LOG.info("Added new master to config: " + newMaster.toString());

    updateMiniClusterClient();

    // Wait for hearbeat interval to ensure tservers pick up the new masters.
    Thread.sleep(4 * MiniYBCluster.TSERVER_HEARTBEAT_INTERVAL_MS);
  }

  protected void removeMaster(HostAndPort oldMaster) throws Exception {
    // Remove old master.
    ChangeConfigResponse response =
        client.changeMasterConfig(oldMaster.getHost(), oldMaster.getPort(), false);
    assertFalse("ChangeConfig has error: " + response.errorMessage(), response.hasError());

    LOG.info("Removed old master from config: " + oldMaster.toString());

    // Wait for the new leader to be online.
    client.waitForMasterLeader(NEW_MASTER_TIMEOUT_MS);
    LOG.info("Done waiting for new leader");

    // Kill the old master.
    miniCluster.killMasterOnHostPort(oldMaster);

    updateMiniClusterClient();

    LOG.info("Killed old master: " + oldMaster.toString());

    // Verify no load tester errors.
    loadTesterRunnable.verifyNumExceptions();
  }

  public void performFullMasterMove(
      Iterator<HostAndPort> oldMastersIter, Iterator<HostAndPort> newMastersIter) throws Exception {
    while (newMastersIter.hasNext() && oldMastersIter.hasNext()) {
      addMaster(newMastersIter.next());
      removeMaster(oldMastersIter.next());
    }
    assert(!newMastersIter.hasNext() && !oldMastersIter.hasNext());
  }

  protected void addNewTServers(int numTservers) throws Exception {
    addNewTServers(numTservers, null);
  }

  protected void addNewTServers(int numTservers, List<String> tserverArgs) throws Exception {
    int expectedTServers = miniCluster.getTabletServers().size() + numTservers;
    // Now double the number of tservers to expand the cluster and verify load spreads.
    for (int i = 0; i < numTservers; i++) {
      miniCluster.startTServer(tserverArgs);
    }

    // Wait for the CQL client to discover the new nodes.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    // Wait for tservers to be up.
    miniCluster.waitForTabletServers(expectedTServers);
  }

  protected void verifyStateAfterTServerAddition(int numTabletServers) throws Exception {
    // Wait for some ops across the entire cluster.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);

    // Verify no failures in load tester.
    loadTesterRunnable.verifyNumExceptions();

    // Verify metrics for all tservers.
    verifyMetrics(0);

    // Verify live tservers.
    verifyExpectedLiveTServers(numTabletServers);
  }

  private void removeTServers(Map<HostAndPort, MiniYBDaemon> originalTServers,
      boolean killMaster) throws Exception {
    // Retrieve existing config, set blacklist and reconfigure cluster.
    List<Common.HostPortPB> blacklisted_hosts = new ArrayList<>();
    for (Map.Entry<HostAndPort, MiniYBDaemon> ts : originalTServers.entrySet()) {
      Common.HostPortPB hostPortPB = Common.HostPortPB.newBuilder()
        .setHost(ts.getKey().getHost())
        .setPort(ts.getKey().getPort())
        .build();
      blacklisted_hosts.add(hostPortPB);
    }

    ModifyMasterClusterConfigBlacklist operation =
      new ModifyMasterClusterConfigBlacklist(client, blacklisted_hosts, true);
    try {
      operation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      fail(e.getMessage());
    }

    if (killMaster) {
      long totalBeforeKillMaster = client.getLoadMoveCompletion().getTotal();

      // Wait for some tablets to get moved from blacklisted tservers.
      TestUtils.waitFor(() -> {
        final long moveRemaining = client.getLoadMoveCompletion().getRemaining();
        final long moveTotal = client.getLoadMoveCompletion().getTotal();
        LOG.info("Move remaining: " + moveRemaining + " - out of - total: ", moveTotal);
        return moveRemaining < moveTotal;
      }, CLUSTER_MOVE_TIMEOUT_MS);

      assertLessThan(client.getLoadMoveCompletion().getRemaining(),
          client.getLoadMoveCompletion().getTotal());

      HostAndPort leaderHostPort = client.getLeaderMasterHostAndPort();
      removeMaster(leaderHostPort);

      long totalAfterKillMaster = client.getLoadMoveCompletion().getTotal();

      // Killing master leader should reset the total count to be the same as remaining.
      // Hence the new total should be strictly less than old total.
      assertLessThan(totalAfterKillMaster, totalBeforeKillMaster);
      // And there should be work remaining to do.
      assertLessThan((long)0, client.getLoadMoveCompletion().getRemaining());
    }

    // Wait for the move to complete.
    TestUtils.waitFor(() -> {
      verifyExpectedLiveTServers(2 * NUM_TABLET_SERVERS);
      final double moveCompletion = client.getLoadMoveCompletion().getPercentCompleted();
      LOG.info("Move completion percent: " + moveCompletion);
      final long moveRemaining = client.getLoadMoveCompletion().getRemaining();
      final long moveTotal = client.getLoadMoveCompletion().getTotal();
      LOG.info("Move remaining: " + moveRemaining + " - out of - total: ", moveTotal);
      return moveCompletion >= 100 && moveRemaining == 0 && moveTotal > 0;
    }, CLUSTER_MOVE_TIMEOUT_MS);

    assertEquals(100, (int) client.getLoadMoveCompletion().getPercentCompleted());
    assertEquals(0, client.getLoadMoveCompletion().getRemaining());
    assertLessThan((long)0, client.getLoadMoveCompletion().getTotal());

    // Wait for the partition metadata to refresh.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    // Kill the old tablet servers.
    // TODO: Check that the blacklisted tablet servers have no tablets assigned.
    for (HostAndPort rpcPort : originalTServers.keySet()) {
      miniCluster.killTabletServerOnHostPort(rpcPort);
    }

    // Wait for heartbeats to expire.
    Thread.sleep(MiniYBCluster.TSERVER_HEARTBEAT_TIMEOUT_MS * 2);

    // Verify live tservers.
    verifyExpectedLiveTServers(NUM_TABLET_SERVERS);
  }

  private void leaderBlacklistTServer(HostAndPort hps[], int offset) throws Exception {
    // Retrieve existing config, set leader blacklist and reconfigure cluster.
    List<Common.HostPortPB> leader_blacklist_hosts = new ArrayList<>();
    HostAndPort hp = hps[offset];
    Common.HostPortPB hostPortPB = Common.HostPortPB.newBuilder()
      .setHost(hp.getHost())
      .setPort(hp.getPort())
      .build();
    leader_blacklist_hosts.add(hostPortPB);

    ModifyMasterClusterConfigBlacklist operation =
      new ModifyMasterClusterConfigBlacklist(client, leader_blacklist_hosts, true /* isAdd */,
          true /* isLeaderBlacklist */);
    try {
      operation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      fail(e.getMessage());
    }

    // Wait for the move to complete.
    TestUtils.waitFor(() -> {
      verifyExpectedLiveTServers(NUM_TABLET_SERVERS + 1);
      final double moveCompletion = client.getLeaderBlacklistCompletion().getPercentCompleted();
      LOG.info("Move completion percent: " + moveCompletion);
      final double moveRemaining = client.getLeaderBlacklistCompletion().getRemaining();
      final double moveTotal = client.getLeaderBlacklistCompletion().getTotal();
      LOG.info("Move remaining: " + moveRemaining + " - out of - total: ", moveTotal);
      return moveCompletion >= 100 && moveRemaining == 0 && moveTotal > 0;
    }, CLUSTER_MOVE_TIMEOUT_MS);

    assertEquals(100, (int) client.getLeaderBlacklistCompletion().getPercentCompleted());
    assertEquals(0, client.getLeaderBlacklistCompletion().getRemaining());
    assertLessThan((long)0, client.getLeaderBlacklistCompletion().getTotal());

    // Wait for the partition metadata to refresh.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    // Verify all tservers have expected tablets.
    TestUtils.waitFor(() -> {
      int numLeaderBlacklistTserversStillLeader = 0;

      YBTable ybTable = client.openTable(CassandraStockTicker.keyspace,
          CassandraStockTicker.tickerTableRaw);
      Map <String, Integer> leaderCounts = new HashMap<>();
      for (LocatedTablet tabletLocation : ybTable.getTabletsLocations(10000)) {
        // Record leader counts for each tserver.
        String tsUuid = tabletLocation.getLeaderReplica().getTsUuid();
        Integer currentCount = leaderCounts.getOrDefault(tsUuid, 0);
        leaderCounts.put(tsUuid, currentCount + 1);

        // Verify all replicas are voters.
        for (LocatedTablet.Replica replica : tabletLocation.getReplicas()) {
          if (!replica.getMemberType().equals("VOTER")) {
            return false;
          }
        }

        // Verify that no leader blacklisted tservers are leaders.
        Common.HostPortPB leader_host = tabletLocation.getLeaderReplica().getRpcHostPort();
        for (Common.HostPortPB leader_blacklist_host : leader_blacklist_hosts) {
          if (leader_host.equals(leader_blacklist_host)) {
            LOG.info("Leader blacklisted tserver " + tsUuid + " is still a leader for tablet " +
                tabletLocation);
            numLeaderBlacklistTserversStillLeader++;
          }
        }
      }

      if (numLeaderBlacklistTserversStillLeader != 0) {
        LOG.info("Number of leader blacklisted tservers still leader = " +
            numLeaderBlacklistTserversStillLeader);
        return false;
      }

      // Verify leaders are balanced across all tservers.
      if (leaderCounts.size() != NUM_TABLET_SERVERS) {
        return false;
      }

      int prevCount = -1;
      for (Integer leaderCount : leaderCounts.values()) {
        // The leader counts could be off by one.
        if (prevCount != -1 && Math.abs(leaderCount - prevCount) > 1) {
          return false;
        }
        prevCount = leaderCount;
      }

      return true;
    }, 10 * WAIT_FOR_SERVER_TIMEOUT_MS);
  }

  private void leaderWhitelistTServer(HostAndPort hps[], int offset) throws Exception {
    // Retrieve existing config, set leader blacklist and reconfigure cluster.
    List<Common.HostPortPB> leader_blacklist_hosts = new ArrayList<>();
    HostAndPort hp = hps[offset];
    Common.HostPortPB hostPortPB = Common.HostPortPB.newBuilder()
      .setHost(hp.getHost())
      .setPort(hp.getPort())
      .build();
    leader_blacklist_hosts.add(hostPortPB);

    ModifyMasterClusterConfigBlacklist operation =
      new ModifyMasterClusterConfigBlacklist(client, leader_blacklist_hosts, false /* isAdd */,
          true /* isLeaderBlacklist */);
    try {
      operation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      fail(e.getMessage());
    }

    // Wait for the move to complete.
    TestUtils.waitFor(() -> {
      verifyExpectedLiveTServers(NUM_TABLET_SERVERS + 1);
      final double moveCompletion = client.getLeaderBlacklistCompletion().getPercentCompleted();
      LOG.info("Move completion percent: " + moveCompletion);
      final long moveRemaining = client.getLeaderBlacklistCompletion().getRemaining();
      final long moveTotal = client.getLeaderBlacklistCompletion().getTotal();
      LOG.info("Move remaining: " + moveRemaining + " - out of - total: ", moveTotal);
      return moveCompletion >= 100 && moveRemaining == 0;
    }, CLUSTER_MOVE_TIMEOUT_MS);

    assertEquals(100, (int) client.getLeaderBlacklistCompletion().getPercentCompleted());
    assertEquals(0, client.getLeaderBlacklistCompletion().getRemaining());

    // Wait for the partition metadata to refresh.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
  }

  protected void performTServerRollingRestart() throws Exception {
    // Add a tserver so that no tablet is under-replicated during rolling restart.
    LOG.info("Add tserver");
    addNewTServers(1);

    // Wait for the load to be balanced across the cluster.
    assertTrue(client.waitForLoadBalance(LOADBALANCE_TIMEOUT_MS, NUM_TABLET_SERVERS + 1));

    // Wait for the load balancer to become idle.
    assertTrue(client.waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS));

    // Wait for the partition metadata to refresh.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    verifyExpectedLiveTServers(NUM_TABLET_SERVERS + 1);

    // Create a copy to store new tserver list.
    Map<HostAndPort, MiniYBDaemon> tservers = new HashMap<>(miniCluster.getTabletServers());
    assertEquals(NUM_TABLET_SERVERS + 1, tservers.size());

    // Retrieve existing config.
    HostAndPort hps[] = new HostAndPort[tservers.size()];
    int i = 0;
    for (HostAndPort hp : tservers.keySet()) {
      hps[i] = hp;
      i++;
    }

    for (int j = 0; j < hps.length; j++) {
      LOG.info("Leader blacklist tserver");
      leaderBlacklistTServer(hps, j);

      // Wait for the load balancer to become idle.
      assertTrue(client.waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS));

      LOG.info("Leader whitelist tserver");
      leaderWhitelistTServer(hps, j);

      // Wait for the load balancer to become idle.
      assertTrue(client.waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS));
    }
  }

  protected void verifyClusterHealth() throws Exception {
    verifyClusterHealth(NUM_TABLET_SERVERS);
  }

  protected void verifyClusterHealth(int numTabletServers) throws Exception {
    // Wait for some ops.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);

    // Wait for some more ops and verify no exceptions.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);
    loadTesterRunnable.verifyNumExceptions();

    // Verify metrics for all tservers.
    verifyMetrics(NUM_OPS_INCREMENT / numTabletServers);

    // Wait for heartbeats to expire.
    Thread.sleep(MiniYBCluster.TSERVER_HEARTBEAT_TIMEOUT_MS * 2);

    // Verify live tservers.
    verifyExpectedLiveTServers(numTabletServers);

    // Verify no failures in the load tester.
    loadTesterRunnable.verifyNumExceptions();
  }

  protected void performTServerExpandShrink(boolean fullMove) throws Exception {
    performTServerExpandShrink(fullMove, /* killMaster */ false);
  }

  protected void performTServerExpandShrink(boolean fullMove, boolean killMaster) throws Exception {
    // Create a copy to store original tserver list.
    Map<HostAndPort, MiniYBDaemon> originalTServers = new HashMap<>(miniCluster.getTabletServers());
    assertEquals(NUM_TABLET_SERVERS, originalTServers.size());

    addNewTServers(originalTServers.size());

    // In the full move case, we don't wait for load balancing.
    if (!fullMove) {
      // Wait for the load to be balanced across the cluster.
      assertTrue(client.waitForLoadBalance(LOADBALANCE_TIMEOUT_MS, NUM_TABLET_SERVERS * 2));

      // Wait for the load balancer to become idle.
      assertTrue(client.waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS));

      // Wait for the partition metadata to refresh.
      Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
    }

    verifyStateAfterTServerAddition(NUM_TABLET_SERVERS * 2);

    LOG.info("Cluster Expand Done!");

    removeTServers(originalTServers, killMaster);

    LOG.info("Cluster Shrink Done!");
  }

  protected void performTServerExpandWithLongRBS() throws Exception {
    // Create a copy to store original tserver list.
    Map<HostAndPort, MiniYBDaemon> originalTServers = new HashMap<>(miniCluster.getTabletServers());
    assertEquals(NUM_TABLET_SERVERS, originalTServers.size());

    // Following var is determined based on log.
    int num_tablets_moved_to_new_tserver = 12;

    int rbs_delay_sec = 15;
    addNewTServers(1, Arrays.asList("--TEST_simulate_long_remote_bootstrap_sec=" + rbs_delay_sec));

    // Load balancer should not become idle while long RBS is half-way.
    assertFalse(client.waitForLoadBalancerIdle(
          (num_tablets_moved_to_new_tserver * rbs_delay_sec) / 2));

    // Wait for the load balancer to become idle.
    assertTrue(client.waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS));

    // Wait for the load to be balanced across the cluster.
    assertTrue(client.waitForLoadBalance(LOADBALANCE_TIMEOUT_MS, NUM_TABLET_SERVERS + 1));

    // Wait for the partition metadata to refresh.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);

    verifyStateAfterTServerAddition(NUM_TABLET_SERVERS + 1);

    LOG.info("Cluster Expand Done!");
  }

  protected void updateConfigReplicationFactor(int replFactor) throws Exception {
    ModifyClusterConfigReplicationFactor modifyRF =
        new ModifyClusterConfigReplicationFactor(client, replFactor);
    modifyRF.doCall();
  }

  /**
   * Change the cluster from one RF to another.
   * We spawn delta number of masters and tservers and update the cluster config.
   */
  public void performRFChange(int fromRF, int toRF) throws Exception {
    if (toRF <= fromRF) {
      LOG.info("Skipping RF reduction for now.");
      return;
    }
    int numNewServers = toRF - fromRF;
    // Wait for load tester to generate traffic.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);
    LOG.info("WaitOps Done.");

    // Increase the number of masters
    createAndAddNewMasters(numNewServers);
    LOG.info("Add Masters Done.");

    // Now perform a tserver expand.
    addNewTServers(numNewServers);
    LOG.info("Add Tservers Done.");

    // Update the RF of the cluster so that load tester adds peers to existing tablets.
    updateConfigReplicationFactor(toRF);
    LOG.info("Replication Factor Change Done.");

    // Wait for load to balance across the target number of tservers.
    assertTrue(client.waitForLoadBalance(LOADBALANCE_TIMEOUT_MS, toRF));

    // Wait for load to balancer to become idle.
    assertTrue(client.waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS));

    // Verify all tservers have expected tablets.
    TestUtils.waitFor(() -> {
      YBTable ybTable = client.openTable(CassandraStockTicker.keyspace,
          CassandraStockTicker.tickerTableRaw);
      Map <String, Integer> leaderCounts = new HashMap<>();
      for (LocatedTablet tabletLocation : ybTable.getTabletsLocations(10000)) {
        // Record leader counts for each tserver.
        String tsUuid = tabletLocation.getLeaderReplica().getTsUuid();
        Integer currentCount = leaderCounts.getOrDefault(tsUuid, 0);
        leaderCounts.put(tsUuid, currentCount + 1);

        if (toRF != tabletLocation.getReplicas().size())  {
          return false;
        }

        // Verify all replicas are voters.
        for (LocatedTablet.Replica replica : tabletLocation.getReplicas()) {
          if (!replica.getMemberType().equals("VOTER")) {
            return false;
          }
        }
      }

      // Verify leaders are balanced across all tservers.
      if (leaderCounts.size() != miniCluster.getTabletServers().size()) {
        return false;
      }

      int prevCount = -1;
      for (Integer leaderCount : leaderCounts.values()) {
        // The leader counts could be off by one.
        if (prevCount != -1 && Math.abs(leaderCount - prevCount) > 1) {
          return false;
        }
        prevCount = leaderCount;
      }
      return true;
    }, WAIT_FOR_SERVER_TIMEOUT_MS);

    // Wait for the partition metadata to refresh.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
    LOG.info("Load Balance Done.");

    // Verify CQL driver knows about all servers.
    TestUtils.waitFor(() -> {
      Collection<Host> connectedHosts = session.getState().getConnectedHosts();
      if (connectedHosts.size() != toRF) {
        return false;
      }
      for (Host h : connectedHosts) {
        if (!h.isUp()) {
          return false;
        }
      }
      return true;
    }, WAIT_FOR_SERVER_TIMEOUT_MS);


    verifyClusterHealth(toRF);
    LOG.info("Sanity Check Done.");

    GetMasterClusterConfigResponse resp = client.getMasterClusterConfig();
    assertEquals(toRF, resp.getConfig().getReplicationInfo().getLiveReplicas().getNumReplicas());

    // TODO: Add per tablet replica count check.
  }
}
