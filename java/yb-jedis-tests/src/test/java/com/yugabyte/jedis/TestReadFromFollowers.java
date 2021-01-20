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
package com.yugabyte.jedis;

import com.google.common.net.HostAndPort;
import com.google.protobuf.ByteString;
import org.apache.commons.text.RandomStringGenerator;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.Parameterized;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.Common;
import org.yb.YBParameterizedTestRunner;
import org.yb.client.*;
import org.yb.master.Master;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.MiniYBDaemon;

import redis.clients.jedis.Jedis;
import redis.clients.jedis.JedisCommands;
import redis.clients.jedis.YBJedis;
import redis.clients.util.JedisClusterCRC16;

import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;

import static junit.framework.TestCase.*;

@RunWith(value=YBParameterizedTestRunner.class)
public class TestReadFromFollowers extends BaseJedisTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestReadFromFollowers.class);

  protected static final int WAIT_FOR_FOLLOWER_TO_CATCH_UP_TIMEOUT_MS = 60000; // 60 seconds.
  protected static final int NUMBER_INSERTS_AND_READS = 1000;
  protected static final int DEFAULT_NUM_READS = 500;

  protected final String tserverRedisFollowerFlag = "--redis_allow_reads_from_followers=true";
  protected final String tserverMaxStaleness = "--max_stale_read_bound_time_ms=10000";

  private YBTable redisTable = null;

  public TestReadFromFollowers(JedisClientType jedisClientType) {
    super(jedisClientType);
  }

  public int getTestMethodTimeoutSec() {
    return 720;
  }

  // Run each test with both Jedis and YBJedis clients.
  @Parameterized.Parameters
  public static Collection jedisClients() {
    return Arrays.asList(JedisClientType.JEDIS, JedisClientType.YBJEDIS);
  }

  @Override
  public void setUpBefore() {
    TestUtils.clearReservedPorts();
  }

  @Override
  public void setUpJedis() throws Exception {
    if (miniCluster == null) {
      return;
    }

    // Create the redis table.
    redisTable = miniCluster.getClient().createRedisTable(YBClient.REDIS_DEFAULT_TABLE_NAME);

    GetTableSchemaResponse tableSchema = miniCluster.getClient().getTableSchema(
        YBClient.REDIS_KEYSPACE_NAME, YBClient.REDIS_DEFAULT_TABLE_NAME);

    assertEquals(Common.PartitionSchemaPB.HashSchema.REDIS_HASH_SCHEMA,
        tableSchema.getPartitionSchema().getHashSchema());

    setUpJedisClient();
  }

  public void setUpJedisClient() throws Exception {
    // Setup the Jedis client.
    List<InetSocketAddress> redisContactPoints = miniCluster.getRedisContactPoints();

    switch (jedisClientType) {
      case JEDIS:
        LOG.info("Connecting to: " + redisContactPoints.get(0).toString());
        jedis_client = new Jedis(redisContactPoints.get(0).getHostName(),
            redisContactPoints.get(0).getPort(), JEDIS_SOCKET_TIMEOUT_MS);
        break;
      case YBJEDIS:
        LOG.info("Connecting to: " + redisContactPoints.get(0).toString());
        jedis_client = new YBJedis(redisContactPoints.get(0).getHostName(),
            redisContactPoints.get(0).getPort(), JEDIS_SOCKET_TIMEOUT_MS);
        break;
    }
  }

  public void testReadsSucceed(int nReads) throws Exception {
    RandomStringGenerator generator = new RandomStringGenerator.Builder()
        .withinRange('a', 'z').build();

    for (int i = 0; i < nReads; i++) {
      String s = generator.generate(20);
      LOG.info("Iteration {}. Setting and getting {}", i, s);
      boolean write_succeeded = false;
      String ret;
      for (int j = 0; j < 20; j++) {
        try {
          ret = jedis_client.set(s, "v");
        } catch (Exception e) {
          if (e.getMessage().contains("Not the leader")) {
            continue;
          }
          throw e;
        }
        if (ret.equals("OK")) {
          write_succeeded = true;
          break;
        }
      }

      assertTrue(write_succeeded);

      TestUtils.waitFor(() -> {
        if (jedis_client.get(s) == null) {
          return false;
        }
        return true;
      }, WAIT_FOR_FOLLOWER_TO_CATCH_UP_TIMEOUT_MS, 100);
      assertEquals("v", jedis_client.get(s));
    }
  }

  @Test
  public void testLocalOps() throws Exception {
    assertNull(miniCluster);

    // We don't want the tablets to move while we are testing.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverAssertLocalTablet = "--TEST_assert_local_tablet_server_selected=true";
    List<List<String>> tserverArgs = new ArrayList<List<String>>();
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    createMiniCluster(3, masterArgs, tserverArgs);

    // Setup the Jedis client.
    setUpJedis();

    testReadsSucceed(DEFAULT_NUM_READS);
  }

  @Test
  public void testSameZoneOps() throws Exception {
    assertNull(miniCluster);

    final String PLACEMENT_CLOUD = "testCloud";
    final String PLACEMENT_REGION = "testRegion";
    final String PLACEMENT_ZONE0 = "testZone0";
    final String PLACEMENT_ZONE1 = "testZone1";
    final String PLACEMENT_ZONE2 = "testZone2";
    final String PLACEMENT_UUID = "placementUuid";

    // We don't want the tablets to move while we are testing.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverAssertTSInZone = "--TEST_assert_tablet_server_select_is_in_zone=testZone0";
    List<List<String>> tserverArgs = new ArrayList<List<String>>();

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE0, "--placement_uuid=" + PLACEMENT_UUID));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE0, "--placement_uuid=" + PLACEMENT_UUID));

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE1, "--placement_uuid=" + PLACEMENT_UUID));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE1, "--placement_uuid=" + PLACEMENT_UUID));

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE2, "--placement_uuid=" + PLACEMENT_UUID));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE2, "--placement_uuid=" + PLACEMENT_UUID));

    createMiniCluster(3, masterArgs, tserverArgs);
    waitForTServersAtMasterLeader();

    YBClient syncClient = miniCluster.getClient();

    // Create the cluster config pb to be sent to the masters
    org.yb.Common.CloudInfoPB cloudInfo0 = org.yb.Common.CloudInfoPB.newBuilder()
        .setPlacementCloud(PLACEMENT_CLOUD)
        .setPlacementRegion(PLACEMENT_REGION)
        .setPlacementZone(PLACEMENT_ZONE0)
        .build();

    // Create the cluster config pb to be sent to the masters
    org.yb.Common.CloudInfoPB cloudInfo1 = org.yb.Common.CloudInfoPB.newBuilder()
        .setPlacementCloud(PLACEMENT_CLOUD)
        .setPlacementRegion(PLACEMENT_REGION)
        .setPlacementZone(PLACEMENT_ZONE1)
        .build();

    // Create the cluster config pb to be sent to the masters
    org.yb.Common.CloudInfoPB cloudInfo2 = org.yb.Common.CloudInfoPB.newBuilder()
        .setPlacementCloud(PLACEMENT_CLOUD)
        .setPlacementRegion(PLACEMENT_REGION)
        .setPlacementZone(PLACEMENT_ZONE2)
        .build();

    Master.PlacementBlockPB placementBlock0 =
        Master.PlacementBlockPB.newBuilder().setCloudInfo(cloudInfo0).setMinNumReplicas(1).build();

    Master.PlacementBlockPB placementBlock1 =
        Master.PlacementBlockPB.newBuilder().setCloudInfo(cloudInfo1).setMinNumReplicas(1).build();

    Master.PlacementBlockPB placementBlock2 =
        Master.PlacementBlockPB.newBuilder().setCloudInfo(cloudInfo2).setMinNumReplicas(1).build();

    List<Master.PlacementBlockPB> placementBlocksLive = new ArrayList<Master.PlacementBlockPB>();
    placementBlocksLive.add(placementBlock0);
    placementBlocksLive.add(placementBlock1);
    placementBlocksLive.add(placementBlock2);

    Master.PlacementInfoPB livePlacementInfo =
        Master.PlacementInfoPB.newBuilder().addAllPlacementBlocks(placementBlocksLive).
            setPlacementUuid(ByteString.copyFromUtf8(PLACEMENT_UUID)).build();

    ModifyClusterConfigLiveReplicas liveOperation =
        new ModifyClusterConfigLiveReplicas(syncClient, livePlacementInfo);
    try {
      liveOperation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      assertTrue(false);
    }

    // Setup the Jedis client.
    setUpJedis();

    testReadsSucceed(DEFAULT_NUM_READS);
  }

  private short DecodeHashValue(byte[] partitionKey) {
    assertEquals(2, partitionKey.length);
    byte[] tempKey = {partitionKey[0], partitionKey[1]};
    return ByteBuffer.wrap(tempKey).getShort();
  }

  private LocatedTablet FindFollowerTablet(String followerHostname) throws Exception {
    List<LocatedTablet> tabletLocations = redisTable.getTabletsLocations(10000);

    for (LocatedTablet locatedTablet : tabletLocations) {
      LocatedTablet.Replica replicaLeader = locatedTablet.getLeaderReplica();

      // We want to find a tablet for which the specified hostname is not a leader.
      if (!replicaLeader.getRpcHost().equals(followerHostname)) {

        // We want a tablet for which the specified tserver is a follower.
        for (LocatedTablet.Replica replica : locatedTablet.getReplicas()) {
          if (replica.getRpcHost().equals(followerHostname)) {
            LOG.info("Selecting tablet {}", locatedTablet.toString());
            return locatedTablet;
          }
        }
      }
    }
    return null;
  }

  @Test
  public void testLocalOpsWithStaleFollowerGoToLeader() throws Exception {
    assertNull(miniCluster);

    // We don't want the tablets to move while we are testing.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverAssertLocalTablet = "--TEST_assert_local_tablet_server_selected=true";
    String tserverAssertReadsRejectedBecauseStaleFollower =
        "--TEST_assert_reads_from_follower_rejected_because_of_staleness";
    String tserverRejectUpdateReplicaRequests =
        "--TEST_follower_reject_update_consensus_requests_seconds=300";

    List<List<String>> tserverArgs = new ArrayList<List<String>>();

    // We only want the first tserver to reject update consensus requests so the cluster can still
    // make progress.
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet,
        tserverMaxStaleness, tserverAssertReadsRejectedBecauseStaleFollower,
        tserverRejectUpdateReplicaRequests));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet,
        tserverMaxStaleness));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet,
        tserverMaxStaleness));
    createMiniCluster(3, masterArgs, tserverArgs);

    // Setup the Jedis client.
    setUpJedis();

    final List<InetSocketAddress> redisContactPoints = miniCluster.getRedisContactPoints();
    final String tserverHostName = redisContactPoints.get(0).getAddress().getHostAddress();
    LocatedTablet tablet = FindFollowerTablet(tserverHostName);
    assertNotNull(tablet);

    short start = 0;
    byte[] keyStart = tablet.getPartition().getPartitionKeyStart();
    if (keyStart.length != 0) {
      start = DecodeHashValue(keyStart);
    }

    short end = 16384;
    byte[] keyEnd = tablet.getPartition().getPartitionKeyEnd();
    if (keyEnd.length != 0) {
      end = DecodeHashValue(keyEnd);
    }

    LOG.info("Start key: {}, end key: {} for tablet {}", start, end, tablet.toString());

    // Find a key that will be in the selected tablet.
    int key = 0;
    int count = 0;
    while (count < NUMBER_INSERTS_AND_READS) {
      String keyString = Integer.toString(key++);
      int slot = JedisClusterCRC16.getSlot(keyString);
      if (slot >= start && slot < end) {
        LOG.info("Inserting key {}", keyString);
        assertEquals("OK", jedis_client.set(keyString, "v"));
        LOG.info("Reading key {}", keyString);
        assertEquals("v", jedis_client.get(keyString));
        ++count;
      }
    }
  }

  @Test
  public void testLocalOpsWithNonStaleFollowerAreServedByFollower() throws Exception {
    assertNull(miniCluster);

    // We don't want the tablets to move while we are testing.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverAssertLocalTablet = "--TEST_assert_local_tablet_server_selected=true";
    String tserverAssertReadsInFollower = "--TEST_assert_reads_served_by_follower=true";

    List<List<String>> tserverArgs = new ArrayList<List<String>>();

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverMaxStaleness,
        tserverAssertLocalTablet, tserverAssertReadsInFollower));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverMaxStaleness,
        tserverAssertLocalTablet, tserverAssertReadsInFollower));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverMaxStaleness,
        tserverAssertLocalTablet, tserverAssertReadsInFollower));
    createMiniCluster(3, masterArgs, tserverArgs);

    // Setup the Jedis client.
    setUpJedis();

    final List<InetSocketAddress> redisContactPoints = miniCluster.getRedisContactPoints();
    final String tserverHostName = redisContactPoints.get(0).getAddress().getHostAddress();
    LocatedTablet tablet = FindFollowerTablet(tserverHostName);
    assertNotNull(tablet);

    short start = 0;
    byte[] keyStart = tablet.getPartition().getPartitionKeyStart();
    if (keyStart.length != 0) {
      start = DecodeHashValue(keyStart);
    }

    short end = 16384;
    byte[] keyEnd = tablet.getPartition().getPartitionKeyEnd();
    if (keyEnd.length != 0) {
      end = DecodeHashValue(keyEnd);
    }

    LOG.info("Start key: {}, end key: {} for tablet {}", start, end, tablet.toString());

    // Find a key that will be in the selected tablet.
    int key = 0;
    int count = 0;
    while (count < NUMBER_INSERTS_AND_READS) {
      String keyString = Integer.toString(key++);
      int slot = JedisClusterCRC16.getSlot(keyString);
      if (slot >= start && slot < end) {
        LOG.info("Inserting key {}", keyString);
        assertEquals("OK", jedis_client.set(keyString, "v"));
        LOG.info("Reading key {}", keyString);
        String value = jedis_client.get(keyString);
        if (value != null && !value.equals("v")) {
          fail(String.format("Invalid value %s returned for key %s", value, keyString));
        }
       ++count;
      }
    }
  }

  @Test
  public void testLookupCacheGetsRefreshedWhenRequestsTimeout() throws Exception {
    assertNull(miniCluster);

    // We don't want the tablets to move while we are testing.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    // Setting this timeout to 4s since redis_service_yb_client_timeout_millis is 3s.
    String simulateTimeOutFailures = "--TEST_simulate_time_out_failures_msecs=4000";
    String verifyAllReplicasAlive = "--TEST_verify_all_replicas_alive=true";
    String lookupCacheRefreshSecs = "--lookup_cache_refresh_secs=1";

    List<List<String>> tserverArgs = new ArrayList<List<String>>();

    // For this test, we only need the first tserver to periodically update its cache.
    // If a requests times out (because of flag --TEST_simulate_time_out_failures), then
    // TS0 will make sure that the cache is updated either because of the periodic refresh of the
    // lookup cache (the new feature we are testing here), or because the selected TS leader replica
    // was marked as failed after the timeout (we already have code that handles this case, the new
    // code handles the case when a follower replica gets marked as failed).
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, lookupCacheRefreshSecs,
        verifyAllReplicasAlive, simulateTimeOutFailures));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, verifyAllReplicasAlive,
        simulateTimeOutFailures));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, verifyAllReplicasAlive,
        simulateTimeOutFailures));

    createMiniCluster(3, masterArgs, tserverArgs);

    // Setup the Jedis client.
    setUpJedis();

    RandomStringGenerator generator = new RandomStringGenerator.Builder()
        .withinRange('a', 'z').build();

    for (int i = 0; i < 200; i++) {
      String s = generator.generate(20);
      String ret;
      try {
        ret = jedis_client.set(s, "v");
      } catch (Exception e) {
        LOG.info("Got exception " + e.toString());
        i--;
        continue;
      }
      if (!ret.equals("OK")) {
        if (ret.equals("v")) {
          // There seems to be a jedis bug that happens during timeouts.
          // set responses start getting get responses and viceversa. Reset jedis client.
          jedis_client = getClientForDB(DEFAULT_DB_NAME);

        }
        LOG.info("Invalid response " + ret);
        i--;
        Thread.sleep(500);
        continue;
      }
      String value = null;
      boolean requestTimedOut = true;
      // If a read request times out, we want to continue reading the same key so that a refresh
      // of the lookup cache is triggered, otherwise we could end up with failed replicas in the
      // cache.
      while (requestTimedOut) {
        try {
          value = jedis_client.get(s);
          requestTimedOut = false;
        } catch (Exception e) {
          if (e.toString().contains("timed out")) {
            requestTimedOut = true;
            // Since the timeout simulated sleep (4000ms) is greater than the lookup cache refresh
            // period, there is no need to sleep to force a refresh of the lookup cache.
            continue;
          }
          LOG.info("Got unexpected exception: " +  e.toString());
          throw e;
        }
      }
      if (value != null && !value.equals("v")) {
        if (value.equals("OK")) {
          // There seems to be a jedis bug that happens during timeouts.
          // set responses start getting get responses and viceversa. Reset jedis client.
          jedis_client = getClientForDB(DEFAULT_DB_NAME);
          continue;
        }
        fail(String.format("Invalid value %s returned for key %s", value, s));
      }
    }
  }

  @Test
  public void testLookupCacheGetsRefreshedWhenTserversGetShutdown() throws Exception {
    assertNull(miniCluster);

    final int NUM_SERVERS_TO_REMOVE = TestUtils.isReleaseBuild() ? 6 : 3;

    final int LOOKUP_REFRESH_SECS = 1;
    final int CONSIDERED_FAILED_SECS = TestUtils.isReleaseBuild() ? 10 : 30;
    final int NUM_SHARDS_PER_TSERVER = TestUtils.isReleaseBuild() ? -1 : 1;
    final int RAFT_HEARTBEAT_INTERVAL_MS = TestUtils.isReleaseBuild() ? 100 : 500;
    final int CONSENSUS_RPC_TIMEOUT_MS = TestUtils.isReleaseBuild() ? 600 : 3000;

    String assertFailedReplicasLessThan = "--TEST_assert_failed_replicas_less_than=2";
    String lookupCacheRefreshSecs = String.format(
        "--lookup_cache_refresh_secs=%d", LOOKUP_REFRESH_SECS);
    String retryFailedReplicaMs = "--retry_failed_replica_ms=1000000";
    String followerUnavailableSecs = String.format(
        "--follower_unavailable_considered_failed_sec=%d", CONSIDERED_FAILED_SECS);
    String leaderMissedHeartBeats = "--leader_failure_max_missed_heartbeat_periods=20";
    String leaderFailureExpBackoffMaxDeltaMsecs = "--leader_failure_exp_backoff_max_delta_ms=100";
    String raftHeartBeatIntervalMsecs = String.format(
        "--raft_heartbeat_interval_ms=%d", RAFT_HEARTBEAT_INTERVAL_MS);
    String consensusRpcTimeoutMsecs = String.format(
        "--consensus_rpc_timeout_ms=%d", CONSENSUS_RPC_TIMEOUT_MS);
    String numShardsPerTserver = String.format(
        "--yb_num_shards_per_tserver=%d", NUM_SHARDS_PER_TSERVER);
    String heartbeatIntervalMs = String.format(
        "--heartbeat_interval_ms=%d", RAFT_HEARTBEAT_INTERVAL_MS * 2);
    String heartbeatRpcTimeoutMs = String.format(
        "--heartbeat_rpc_timeout_ms=%d", RAFT_HEARTBEAT_INTERVAL_MS * 2 * 15);

    String liveReplicaUuid = "live_replicas";
    String liveReplicaPlacementUuid = "--placement_uuid=" + liveReplicaUuid;

    String readReplicaUuid = "read_replicas";
    String readReplicaPlacementUuid = "--placement_uuid=" + readReplicaUuid;

    List<List<String>> tserverArgs = new ArrayList<List<String>>();

    // 9 live replicas.
    for (int i = 0; i < 9; i++) {
      tserverArgs.add(Arrays.asList(
          liveReplicaPlacementUuid,
          tserverRedisFollowerFlag,
          lookupCacheRefreshSecs,
          assertFailedReplicasLessThan,
          followerUnavailableSecs,
          retryFailedReplicaMs,
          leaderMissedHeartBeats,
          raftHeartBeatIntervalMsecs,
          consensusRpcTimeoutMsecs,
          leaderFailureExpBackoffMaxDeltaMsecs,
          numShardsPerTserver,
          heartbeatIntervalMs,
          heartbeatRpcTimeoutMs));
    }

    String tserverUnresponsiveTimeout = String.format("--tserver_unresponsive_timeout_ms=%d",
                                                      CONSIDERED_FAILED_SECS * 1000 - 2000);
    List<String> masterArgs = Arrays.asList(
        "--enable_load_balancing=true", tserverUnresponsiveTimeout);

    // 3 read-only replicas.
    for (int i = 0; i < 3; i++) {
      tserverArgs.add(Arrays.asList(
          readReplicaPlacementUuid,
          tserverRedisFollowerFlag,
          lookupCacheRefreshSecs,
          assertFailedReplicasLessThan,
          followerUnavailableSecs,
          retryFailedReplicaMs,
          leaderMissedHeartBeats,
          raftHeartBeatIntervalMsecs,
          consensusRpcTimeoutMsecs,
          leaderFailureExpBackoffMaxDeltaMsecs,
          numShardsPerTserver,
          heartbeatIntervalMs,
          heartbeatRpcTimeoutMs));
    }

    // Create a cluster with RF = 3: 9 live replicas, and 3 read-only replicas.
    createMiniCluster(3, masterArgs, tserverArgs);

    Master.PlacementInfoPB livePlacementInfo =
        Master.PlacementInfoPB.newBuilder()
            .setNumReplicas(3)
            .setPlacementUuid(ByteString.copyFromUtf8(liveReplicaUuid))
            .build();
    ModifyClusterConfigLiveReplicas liveOperation =
        new ModifyClusterConfigLiveReplicas(miniCluster.getClient(), livePlacementInfo);
    liveOperation.doCall();

    Master.PlacementInfoPB readOnlyPlacementInfo =
        Master.PlacementInfoPB.newBuilder()
            .setNumReplicas(3)
            .setPlacementUuid(ByteString.copyFromUtf8(readReplicaUuid))
            .build();
    List<Master.PlacementInfoPB> readOnlyPlacements = Arrays.asList(readOnlyPlacementInfo);
    ModifyClusterConfigReadReplicas readReplicasConfigChange =
        new ModifyClusterConfigReadReplicas(miniCluster.getClient(), readOnlyPlacements);
    readReplicasConfigChange.doCall();

    miniCluster.waitForTabletServers(12);
    LOG.info("All tablet servers ready");

    // Setup the Jedis client.
    setUpJedis();

    LOG.info("Done calling setUpJedis");

    JedisCommands jedisClient[] = new JedisCommands[3];
    String jedisClientHostNames[] = new String[3];

    // Create three different jedis client connections so that we can read from any of the first
    // three clients.
    List<InetSocketAddress> redisContactPoints = miniCluster.getRedisContactPoints();
    for (int i = 0; i < jedisClient.length; i++) {
      switch (jedisClientType) {
        case JEDIS:
          LOG.info("Connecting to: " + redisContactPoints.get(i).toString());
          jedisClient[i] = new Jedis(redisContactPoints.get(i).getHostName(),
              redisContactPoints.get(i).getPort(), JEDIS_SOCKET_TIMEOUT_MS * 3);
          break;
        case YBJEDIS:
          LOG.info("Connecting to: " + redisContactPoints.get(i).toString());
          jedisClient[i] = new YBJedis(redisContactPoints.get(i).getHostName(),
              redisContactPoints.get(i).getPort(), JEDIS_SOCKET_TIMEOUT_MS * 3);
          break;
      }
      jedisClientHostNames[i] = redisContactPoints.get(i).getHostName();
    }

    // Get tablet servers.
    Map<HostAndPort, MiniYBDaemon> tservers = miniCluster.getTabletServers();

    LOG.info("Done setting up jedis");

    RandomStringGenerator generator = new RandomStringGenerator.Builder()
        .withinRange('a', 'z').build();

    Random random = new Random();
    int tserversCount = 0;

    Set<String> readOnlyReplicas = new HashSet<>();
    List<LocatedTablet> tabletLocations = redisTable.getTabletsLocations(10000);
    for (LocatedTablet locatedTablet : tabletLocations) {
      for (LocatedTablet.Replica replica : locatedTablet.getReplicas()) {
        if (replica.getRole().equals("READ_REPLICA")) {
          readOnlyReplicas.add(replica.getRpcHost());
        }
      }
    }

    // We don't want to destroy any of the first three servers.
    for (Map.Entry<HostAndPort, MiniYBDaemon> entry : tservers.entrySet()) {
      if (entry.getKey().getHost().equals(jedisClientHostNames[0]) ||
          entry.getKey().getHost().equals(jedisClientHostNames[1]) ||
          entry.getKey().getHost().equals(jedisClientHostNames[2])) {
        LOG.info(String.format("Using host %s for reading/writing. Skipping ",
            entry.getKey().getHost()));
        continue;
      }
      if (readOnlyReplicas.contains(entry.getKey().getHost())) {
        LOG.info(String.format("Host %s is a read replica. Skipping",
            entry.getKey().getHost()));
        continue;
      }

      // Kill the tserver before sending any write/read requests so that the requests will force
      // a cache reload after the killed tserver is marked as a failed replica.
      LOG.info(String.format("[%d] Killing tserver on host %s",
                             tserversCount + 1, entry.getKey().getHost()));
      miniCluster.killTabletServerOnHostPort(entry.getKey());
      tserversCount++;
      LOG.info(String.format("[%d] About to sleep 20 seconds", tserversCount));

      // Sleep so that the killed TServer gets removed from all the raft configurations.
      Thread.sleep(CONSIDERED_FAILED_SECS * 1000);

      // Wait until the killed tserver is not part of any of the replicas.
      boolean killedTSInReplicaList = false;
      boolean allTabletsHaveALeader = true;
      boolean allTabletsHaveThreeVoters = true;
      do {
        killedTSInReplicaList = false;
        allTabletsHaveALeader = true;
        allTabletsHaveThreeVoters = true;

        tabletLocations = redisTable.getTabletsLocations(10000);
        for (LocatedTablet locatedTablet : tabletLocations) {
          int nVoters = 0;
          String tabletId = new String(locatedTablet.getTabletId());
          for (LocatedTablet.Replica replica : locatedTablet.getReplicas()) {
            // We want to find a tablet for which the specified hostname is not a leader.
            if (replica.getRpcHost().equals(entry.getKey().getHost())) {
              LOG.info(String.format("[%d] Found killed replica %s in tablet %s: %s",
                  tserversCount, replica.getRpcHost(), tabletId,
                  locatedTablet.toDebugString()));
              killedTSInReplicaList = true;
              // This doesn't seem to be very reliable. So try again.
              miniCluster.killTabletServerOnHostPort(entry.getKey());
              break;
            }
            if (replica.getMemberType().equals("VOTER")) {
              nVoters++;
            }
          }
          if (killedTSInReplicaList) {
            break;
          }

          // If we are here, the killed replica is not in the replica list for this tablet.
          if (nVoters < 3) {
            LOG.info(String.format("[%d] Tablet %s has %d VOTERS, but expected 3: %s",
                tserversCount, tabletId, nVoters, locatedTablet.toDebugString()));
            allTabletsHaveThreeVoters = false;
            break;
          }
          if (locatedTablet.getLeaderReplica() == null) {
            LOG.info(String.format("[%d] Tablet %s doesn't have a leader: %s",
                tserversCount, tabletId, locatedTablet.toDebugString()));
            allTabletsHaveALeader = false;
            break;
          }
        }
        Thread.sleep(1000);
      } while (killedTSInReplicaList || !allTabletsHaveThreeVoters || !allTabletsHaveALeader);

      LOG.info(String.format("[%d] Done sleeping", tserversCount));

      // Issue a few requests so that the killed replica gets marked as failed after the requests
      // that are sent to it time out.
      for (int i = 0; i < 100; i++) {
        int index = random.nextInt(3);
        String s = generator.generate(20);
        setWithRetries(jedis_client, s, "v");
        String value = null;
        try {
          value = jedisClient[index].get(s);
        } catch (Exception e) {
          LOG.warn("Got exception " + e.toString());
          i--;
          continue;
        }
        if (value != null && !value.equals("v")) {
          fail(String.format("[%d] Invalid value %s returned for key %s", tserversCount, value, s));
        }
      }
      LOG.info(String.format("[%d] Done sending some requests. About to sleep for %d milliseconds",
                             tserversCount, 2 * LOOKUP_REFRESH_SECS * 1000));

      // Sleep so that by the time the read requests are sent, the cache for each tablet in each of
      // the nodes will be stale and the lookup cache is refreshed.
      Thread.sleep(2 * LOOKUP_REFRESH_SECS * 1000);

      LOG.info(String.format("[%d] Done sleeping", tserversCount));

      for (int i = 0; i < 200; i++) {
        int index = random.nextInt(3);
        String s = generator.generate(20);
        String status = null;
        try {
          status = jedis_client.set(s, "v");
        } catch (Exception e) {
          LOG.warn("Write failed. Got exception " + e.toString());
          i--;
          continue;
        }
        assertEquals("OK", status);
        String value = null;
        try {
          value = jedisClient[index].get(s);
        } catch (Exception e) {
          LOG.warn("Got exception " + e.toString());
          i--;
          continue;
        }
        if (value != null && !value.equals("v")) {
          fail(String.format("[%d] Invalid value %s returned for key %s", tserversCount, value, s));
        }
        if ((i + 1) % 50 == 0) {
          LOG.info(String.format("[%d] Done %d requests", tserversCount, i + 1));
        }
      }

      // Because we check that we only remove live replicas, at the end, we finish with 3 live
      // replicas, and 3 read replicas in release builds.
      // 6 live replicas, and 3 read replicas in non-release builds.
      if (tserversCount == NUM_SERVERS_TO_REMOVE) {
        break;
      }
    }
  }

  @Test
  public void testRestartDuringFollowerRead() throws Exception {
    testRestartDuringFollowerReadWithValueSize(10);
  }

  @Test
  public void testRestartDuringFollowerReadWithFlushes() throws Exception {
    testRestartDuringFollowerReadWithValueSize(10240);
  }

  public void testRestartDuringFollowerReadWithValueSize(int valSize) throws Exception {
    assertNull(miniCluster);

    // We don't want the tablets to move while we are testing.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverAssertLocalTablet = "--TEST_assert_local_tablet_server_selected=false";
    List<List<String>> tserverArgs = new ArrayList<List<String>>();
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));

    createMiniCluster(3, masterArgs, tserverArgs);

    // Setup the Jedis client.
    setUpJedis();

    char[] arr = new char[valSize];
    Arrays.fill(arr, 'v');
    String val = new String(arr);

    for (int i = 0; i < 200; i++) {
      String s = "key-" + i;
      LOG.info("Inserting key {}", s);
      assertEquals("OK", jedis_client.set(s, val));
    }
    LOG.info("Done loading the data");
    for (int i = 0; i < 200; i++) {
      String s = "key-" + i;
      LOG.info("Reading key {}", s);
      String value = jedis_client.get(s);

      while (value == null || !value.equals(val)) {
        // We hope that all the followers have caught up to the updates by now. In case they aren't
        // we may loop here.
        LOG.error(String.format("Unexpected: Invalid value %s returned for key %s", value, s));

        value = jedis_client.get(s);
      }
    }
    LOG.info("Done reading the data before restart");

    miniCluster.restart();
    setUpJedisClient();

    LOG.info("Restarted after loading the data");
    int missingValues = 0;
    StringBuilder missing = new StringBuilder();
    for (int i = 0; i < 200; i++) {
      String s = "key-" + i;
      LOG.info("Reading key {}", s);
      String value;
      try {
        value = jedis_client.get(s);
      } catch (Exception e) {
        LOG.error("Caught exception while trying to read key " + s, e);
        i--;  // Retry the same key.
        continue;
      }
      if (value == null || !value.equals(val)) {
        // If we were able to read the value before the restart, we should be able to
        // read it after the restart as well. Failure here shall be considered a test failure.
        LOG.error(String.format("Invalid value %s returned for key %s", value, s));
        missingValues++;
        missing.append(s);
        missing.append(", ");
      } else {
        LOG.debug("Read value as expected. After restart.");
      }
    }
    // We may expect upto 1 missing edit/key-value for each tablet.
    // TODO: Reset this to 0, when we start persisting noops related to commit index advancement.
    int kMissingAllowed = 24;
    if (missingValues > kMissingAllowed) {
      fail(String.format("Missed %d values : %s", missingValues, missing.toString()));
    } else if (missingValues > 0) {
      LOG.info(String.format("[OK for now] Missed %d values : %s",
               missingValues, missing.toString()));
    }
    LOG.info("Done reading after restart.");
  }
}
