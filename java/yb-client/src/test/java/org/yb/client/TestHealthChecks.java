// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
package org.yb.client;

import com.google.common.net.HostAndPort;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.YBTestRunner;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.minicluster.MiniYBDaemon;
import org.yb.util.ServerInfo;

import static org.yb.AssertionWrappers.*;

import java.net.MalformedURLException;
import java.net.URL;
import java.util.Map;
import java.util.Scanner;

@RunWith(value= YBTestRunner.class)
public class TestHealthChecks extends BaseYBClientTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestHealthChecks.class);
  private static final String TABLE_NAME =
      TestMasterFailover.class.getName() + "-" + System.currentTimeMillis();

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.tserverHeartbeatTimeoutMs(5000);
  }

  @Override
  protected void afterStartingMiniCluster() throws Exception {
    super.afterStartingMiniCluster();
    createTable(TABLE_NAME, hashKeySchema, new CreateTableOptions());
  }

  @Override
  protected int overridableNumShardsPerTServer() {
    return 1;
  }

  protected void addNewTServers(int numTservers) throws Exception {
    int expectedTServers = miniCluster.getTabletServers().size() + numTservers;
    // Now double the number of tservers to expand the cluster and verify load spreads.
    for (int i = 0; i < numTservers; i++) {
      miniCluster.startTServer(null);
    }

    // Wait for tservers to be up.
    miniCluster.waitForTabletServers(expectedTServers);
  }

  private JsonElement getHealthValue(String key) throws Exception {
    return getHealthValue(key, 0);
  }

  private JsonElement getHealthValue(String key, int tserver_death_interval_msecs) throws Exception {
    try {
      // get connection info for master web port
      HostAndPort masterHostAndPort = syncClient.getLeaderMasterHostAndPort();
      assertNotNull(masterHostAndPort);
      Map<HostAndPort, MiniYBDaemon> masters = miniCluster.getMasters();
      assertTrue(String.format("Couldn't find master %s in list %s", masterHostAndPort,
            masters.keySet().toString()), masters.containsKey(masterHostAndPort));
      int masterLeaderWebPort = masters.get(masterHostAndPort).getWebPort();

      // call the health-check JSON endpoint
      URL url =
        new URL(String.format("http://%s:%d/api/v1/health-check?tserver_death_interval_msecs=%d",
                masterHostAndPort.getHost(), masterLeaderWebPort, tserver_death_interval_msecs));
      Scanner scanner = new Scanner(url.openConnection().getInputStream());
      JsonParser parser = new JsonParser();
      JsonElement tree = parser.parse(scanner.useDelimiter("\\A").next());
      JsonObject kvs = tree.getAsJsonObject();
      LOG.info("Health Value: " + key + " = " + kvs.get(key));
      return kvs.get(key);
    } catch (MalformedURLException e) {
      throw new InternalError(e.getMessage());
    }

  }

  @Test(timeout = 60000)
  public void testKillRestartTServer() throws Exception {
    int countMasters = masterHostPorts.size();
    if (countMasters < 3) {
      LOG.info("This test requires at least 3 master servers, but only " + countMasters +
               " are specified.");
      return;
    }

    final int HEARTBEAT_SEC =
        miniCluster.getClusterParameters().getTServerHeartbeatTimeoutMs() / 1000;

    // create a 4th TServer so we can kill a server and maintain normal replication
    addNewTServers(1);

    // verify all servers have stayed up for at least one heartbeat
    Thread.sleep((HEARTBEAT_SEC + 1) * 1000);
    assertGreaterThanOrEqualTo(getHealthValue("most_recent_uptime").getAsInt(), HEARTBEAT_SEC);

    // kill (don't stop) a TServer.
    JsonArray deadNodes = getHealthValue("dead_nodes").getAsJsonArray();
    assertEquals(deadNodes.size(), 0);
    JsonArray underReplicatedTablets = getHealthValue("under_replicated_tablets").getAsJsonArray();
    assertEquals(underReplicatedTablets.size(), 0);

    ListTabletServersResponse tsList = syncClient.listTabletServers();
    ServerInfo tServer = tsList.getTabletServersList().get(tsList.getTabletServersCount()-1);
    miniCluster.killTabletServerOnHostPort(
        HostAndPort.fromParts(tServer.getHost(), tServer.getPort()) );

    // ensure that it shows up in the 'dead_nodes'.
    Thread.sleep(2 * HEARTBEAT_SEC * 1000);
    deadNodes = getHealthValue("dead_nodes").getAsJsonArray();
    assertEquals(deadNodes.size(), 1);
    LOG.info("Dead Nodes: " + deadNodes.get(0) + " && " + tServer.getUuid());

    // With the default value of tserver_death_interval_msecs as 0, we should get some tablets that
    // are under replicated in this call.
    underReplicatedTablets = getHealthValue("under_replicated_tablets").getAsJsonArray();
    assertTrue(underReplicatedTablets.size() > 0);

    // With a higher value of tserver_death_interval_msecs, we should not get any tablets that are
    // listed as under-replicated.
    underReplicatedTablets = getHealthValue("under_replicated_tablets", 100000).getAsJsonArray();
    assertEquals(underReplicatedTablets.size(), 0);

    // BUG: 'dead_nodes' lists WebPort, we're using RpcPort
    assertEquals(deadNodes.get(0).getAsString(), tServer.getUuid());
    assertGreaterThanOrEqualTo(
        getHealthValue("most_recent_uptime").getAsInt(), 3 * HEARTBEAT_SEC);

    // start a new TServer, ensure that the most_recent_uptime just decreased.
    addNewTServers(1);
    TestUtils.waitFor(() -> {
      return getHealthValue("most_recent_uptime").getAsInt() < 3 * HEARTBEAT_SEC;
    }, HEARTBEAT_SEC * 3);
  }
}
