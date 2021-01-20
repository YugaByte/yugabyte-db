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
package org.yb.clientent;

import org.yb.client.*;

import java.util.*;

import com.google.protobuf.ByteString;

import org.junit.Test;
import org.junit.runner.RunWith;

import org.yb.Schema;
import org.yb.ColumnSchema;
import org.yb.master.Master;
import org.yb.minicluster.MiniYBCluster;
import org.yb.util.YBTestRunnerNonTsanAsan;

import static org.yb.AssertionWrappers.assertTrue;

@RunWith(value=YBTestRunnerNonTsanAsan.class)
public class TestReadReplicas extends TestYBClient {
  private static final String PLACEMENT_CLOUD = "testCloud";
  private static final String PLACEMENT_REGION = "testRegion";
  private static final String PLACEMENT_ZONE = "testZone";
  private static final String LIVE_TS = "live";
  private static final String READ_ONLY_TS = "readOnly";
  private static final String READ_ONLY_NEW_TS = "readOnlyNew";

  /**
   * Test for live and read only replica correct load balancing.
   * @throws Exception
   */
  @Test(timeout = 100000)
  public void testCreateTableWithAddRemoveNode() throws Exception {
    // Destroy the cluster so we can create a new one.
    destroyMiniCluster();

    List<String> livePlacement = Arrays.asList(
            "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
            "--placement_zone=" + PLACEMENT_ZONE, "--placement_uuid=" + LIVE_TS);

    List<String> readOnlyPlacement = Arrays.asList(
            "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
            "--placement_zone=" + PLACEMENT_ZONE, "--placement_uuid=" + READ_ONLY_TS);

    List<List<String>> tserverArgs = new ArrayList<List<String>>();
    // Create a live and read only cluster with 3 masters and 3 tservers in the same az. Although
    // this is not the most common use case, it is the most pathological and should be no different
    // from different azs.
    for (int i = 0; i < 3; i++) {
      tserverArgs.add(livePlacement);
      tserverArgs.add(readOnlyPlacement);
    }

    // Master args, used to speed up the test.
    List<String> masterArgs = Arrays.asList("--load_balancer_max_concurrent_adds=100",
            "--load_balancer_max_concurrent_moves=100",
            "--load_balancer_max_concurrent_removals=100");

    // Enable YSQL to generate the txn status table (in order to test txn status leader spread).
    createMiniCluster(3, masterArgs, tserverArgs, true /* enable_ysql */);

    // Create the cluster config pb to be sent to the masters
    org.yb.Common.CloudInfoPB cloudInfo0 = org.yb.Common.CloudInfoPB.newBuilder()
            .setPlacementCloud(PLACEMENT_CLOUD)
            .setPlacementRegion(PLACEMENT_REGION)
            .setPlacementZone(PLACEMENT_ZONE)
            .build();

    Master.PlacementBlockPB placementBlock0 = Master.PlacementBlockPB.newBuilder().
            setCloudInfo(cloudInfo0).setMinNumReplicas(3).build();

    List<Master.PlacementBlockPB> placementBlocksLive = new ArrayList<Master.PlacementBlockPB>();
    placementBlocksLive.add(placementBlock0);

    List<Master.PlacementBlockPB> placementBlocksReadOnly =
            new ArrayList<Master.PlacementBlockPB>();
    placementBlocksReadOnly.add(placementBlock0);

    Master.PlacementInfoPB livePlacementInfo =
            Master.PlacementInfoPB.newBuilder().addAllPlacementBlocks(placementBlocksLive).
                    setPlacementUuid(ByteString.copyFromUtf8(LIVE_TS)).build();

    Master.PlacementInfoPB readOnlyPlacementInfo =
            Master.PlacementInfoPB.newBuilder().addAllPlacementBlocks(placementBlocksReadOnly).
                    setPlacementUuid(ByteString.copyFromUtf8(READ_ONLY_TS)).build();

    List<Master.PlacementInfoPB> readOnlyPlacements = Arrays.asList(readOnlyPlacementInfo);
    ModifyClusterConfigReadReplicas readOnlyOperation =
            new ModifyClusterConfigReadReplicas(syncClient, readOnlyPlacements);
    try {
      readOnlyOperation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      assertTrue(false);
    }

    ModifyClusterConfigLiveReplicas liveOperation =
            new ModifyClusterConfigLiveReplicas(syncClient, livePlacementInfo);
    try {
      liveOperation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      assertTrue(false);
    }

    // Create a table with 8 tablets
    List<ColumnSchema> columns = new ArrayList<>(hashKeySchema.getColumns());
    Schema newSchema = new Schema(columns);
    CreateTableOptions tableOptions = new CreateTableOptions().setNumTablets(8);
    YBTable table = syncClient.createTable(
            DEFAULT_KEYSPACE_NAME, "CreateTableTest", newSchema, tableOptions);

    // Ensure that each live tserver has 8 live replicas each and each read only tserver has 8 read
    // only replicas each.
    Map<String, List<List<Integer>>> placementUuidMap =
            table.getMemberTypeCountsForEachTSType(DEFAULT_TIMEOUT_MS);
    List<List<Integer>> liveTsList = placementUuidMap.get(LIVE_TS);
    List<List<Integer>> readOnlyTsList = placementUuidMap.get(READ_ONLY_TS);

    LOG.info(liveTsList.get(0).toString());
    assertTrue(liveTsList.get(0).equals(Arrays.asList(8, 8, 8)));
    assertTrue(liveTsList.get(1).equals(Arrays.asList(0, 0, 0)));

    assertTrue(readOnlyTsList.get(0).equals(Arrays.asList(0, 0, 0)));
    assertTrue(readOnlyTsList.get(1).equals(Arrays.asList(8, 8, 8)));

    // Create another live and readOnly node.
    miniCluster.startTServer(tserverArgs.get(0));
    miniCluster.startTServer(tserverArgs.get(1));
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
    miniCluster.waitForTabletServers(8);

    // Test that now each live tsever has 6 live replicas and each read only tserver has 6 read
    // only replicas.
    placementUuidMap = table.getMemberTypeCountsForEachTSType(DEFAULT_TIMEOUT_MS);
    Map<String, List<List<Integer>>> expectedMap = new HashMap<String, List<List<Integer>>>();
    List<List<Integer>> expectedLiveTsList = Arrays.asList(Arrays.asList(6, 6, 6, 6),
            Arrays.asList(0, 0, 0, 0));
    List<List<Integer>> expectedReadOnlyTsList = Arrays.asList(Arrays.asList(0, 0, 0, 0),
            Arrays.asList(6, 6, 6, 6));
    expectedMap.put(LIVE_TS, expectedLiveTsList);
    expectedMap.put(READ_ONLY_TS, expectedReadOnlyTsList);

    assertTrue(syncClient.waitForExpectedReplicaMap(30000, table, expectedMap));

    // Now we create a new read only cluster with 3 nodes and RF=3 with uuid readOnlyNew in the
    // same zone.
    List<String> readOnlyPlacementNew = Arrays.asList(
            "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
            "--placement_zone=" + PLACEMENT_ZONE, "--placement_uuid=" + READ_ONLY_NEW_TS);

    for (int i = 0; i < 3; i++) {
      miniCluster.startTServer(readOnlyPlacementNew);
    }
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
    miniCluster.waitForTabletServers(11);

    List<Master.PlacementBlockPB> placementBlocksreadOnlyNew =
            new ArrayList<Master.PlacementBlockPB>();
    placementBlocksreadOnlyNew.add(placementBlock0);

    Master.PlacementInfoPB readOnlyPlacementInfoNew =
            Master.PlacementInfoPB.newBuilder().
                    addAllPlacementBlocks(placementBlocksreadOnlyNew).
                    setPlacementUuid(ByteString.copyFromUtf8(READ_ONLY_NEW_TS)).build();

    List<Master.PlacementInfoPB> readOnlyPlacementsNew =
            Arrays.asList(readOnlyPlacementInfoNew);
    ModifyClusterConfigReadReplicas readOnlyOperationNew =
            new ModifyClusterConfigReadReplicas(syncClient, readOnlyPlacementsNew);
    try {
      readOnlyOperationNew.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      assertTrue(false);
    }

    // We make sure that each tserver is this new zone has 8 read only replicas each, and
    // that none of the other tservers have changed replica counts.
    List<List<Integer>> expectedReadOnlyNewTsList = Arrays.asList(Arrays.asList(0, 0, 0),
            Arrays.asList(8, 8, 8));
    expectedMap.put(READ_ONLY_NEW_TS, expectedReadOnlyNewTsList);

    assertTrue(syncClient.waitForExpectedReplicaMap(60000, table, expectedMap));
    // From issue #6081, make sure that we ignore read replicas when checking if
    // transaction status tablet leaders are properly spread.
    assertTrue(syncClient.waitForAreLeadersOnPreferredOnlyCondition(DEFAULT_TIMEOUT_MS));
  }
}
