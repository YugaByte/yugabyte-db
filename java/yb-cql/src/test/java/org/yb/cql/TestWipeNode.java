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

import com.datastax.driver.core.utils.Bytes;
import com.google.common.net.HostAndPort;
import org.apache.commons.io.FileUtils;
import org.junit.Test;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.ListTabletServersResponse;
import org.yb.client.LocatedTablet;
import org.yb.client.TestUtils;
import org.yb.client.YBTable;
import org.yb.minicluster.MiniYBDaemon;
import org.yb.util.ServerInfo;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.net.URL;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;

import static junit.framework.TestCase.assertEquals;
import static junit.framework.TestCase.assertNotNull;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertNotEquals;
import static org.yb.AssertionWrappers.assertTrue;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestWipeNode  extends BaseCQLTest {

  protected static final Logger LOG = LoggerFactory.getLogger(TestWipeNode.class);
  protected static final int WAIT_FOR_OP_MS = 10000;

  private void writeRows(String tableName, int numRows) {
    for (int i = 0; i < numRows; i++) {
      session.execute(String.format("INSERT INTO %s (c1, c2) values (%d, %d)", tableName, i, i));
    }
  }

  private String getContentForUrl(URL url) throws Exception {
    String content = "";
    BufferedReader in = new BufferedReader(
        new InputStreamReader(url.openConnection().getInputStream()));
    try {
      String inputLine;
      while ((inputLine = in.readLine()) != null) {
        content += inputLine;
      }
    } finally {
      in.close();
    }

    return content;
  }

  /**
   * This unit test verifies that if we stop a tserver, wipe its data and bring it back up, it is
   * eventually removed from all quorums and we are still able to perform write operations.
   */
  @Test
  public void testWipeTserver() throws Exception {
    String tableName = "testwipetserver";
    session.execute(String.format("CREATE TABLE %s (c1 int, c2 int, PRIMARY KEY (c1))", tableName));
    writeRows(tableName, 100);

    Map.Entry<HostAndPort, MiniYBDaemon> daemon =
        miniCluster.getTabletServers().entrySet().iterator().next();
    String removedNodeHost = daemon.getKey().getHost();
    int removedNodePort = daemon.getKey().getPort();

    // Get the uuid of the node that we are going to remove.
    YBTable ybTable = miniCluster.getClient().openTable(DEFAULT_TEST_KEYSPACE, tableName);

    List<LocatedTablet> tablets = ybTable.getTabletsLocations(WAIT_FOR_OP_MS);
    assertFalse(tablets.isEmpty());

    String uuid = null;
    for (ServerInfo serverInfo :
         miniCluster.getClient().listTabletServers().getTabletServersList()) {
      if (serverInfo.getHost().equals(removedNodeHost) &&
          serverInfo.getPort() == removedNodePort) {
        uuid = serverInfo.getUuid();
      }
    }

    assertNotNull(uuid);
    final String removedNodeUuid = uuid;

    // Wipe one node now.
    LOG.info("Removing node: " + daemon.getKey().toString());
    miniCluster.killTabletServerOnHostPort(daemon.getKey());

    String dataDirPath = daemon.getValue().getDataDirPath();
    FileUtils.deleteDirectory(new File(dataDirPath));

    // Now start up the node on the same host, port.
    miniCluster.startTServer(null, removedNodeHost, removedNodePort);
    LOG.info("Started a new node. Host: {}, port: {}", removedNodeHost, removedNodePort);

    // Try writing rows.
    writeRows(tableName, 100);

    // Verify tablet metrics.
    TestUtils.waitFor(() -> {
      List<LocatedTablet> tabletsLocations = ybTable.getTabletsLocations(WAIT_FOR_OP_MS);
      assertFalse(tabletsLocations.isEmpty());
      for (LocatedTablet tablet : tabletsLocations) {
        // Verify the wiped node has no tablets.
        for (LocatedTablet.Replica replica : tablet.getReplicas()) {
          if (replica.getTsUuid().equals(removedNodeUuid)) {
            LOG.error(String.format("Removed node %s:%d, still has replica: %s", removedNodeHost,
                removedNodePort, replica.toString()));
            return false;
          }
        }

        HostAndPort hostPort = HostAndPort.fromParts(tablet.getLeaderReplica().getRpcHost(),
            tablet.getLeaderReplica().getRpcPort());
        MiniYBDaemon ybDaemon = miniCluster.getTabletServers().get(hostPort);
        assertNotNull(ybDaemon);

        URL url = new URL(String.format("http://%s:%d/tablet-consensus-status?id=%s",
            ybDaemon.getLocalhostIP(), ybDaemon.getWebPort(),
            new String(tablet.getTabletId())));

        String content = getContentForUrl(url);
        LOG.info("Content for " + url + ", " + content);
        // Validate that there are no OPs pending replication.
        if (content.indexOf("LogCacheStats(num_ops=0") == -1) {
          LOG.error(String.format("Ops still pending replication for tablet: %s",
              tablet.toString()));
          return false;
        }
      }
      return true;
    }, WAIT_FOR_OP_MS);

    // Writes should still succeed.
    writeRows(tableName, 100);

    // Verify master reports the correct number of tablet servers.
    assertEquals(NUM_TABLET_SERVERS,
        miniCluster.getClient().listTabletServers().getTabletServersCount());

    // Verify the master webpage.
    HostAndPort leaderMaster = miniCluster.getClient().getLeaderMasterHostAndPort();
    MiniYBDaemon leaderMasterDaemon = miniCluster.getMasters().get(leaderMaster);
    URL leaderMasterUrl = new URL(String.format("http://%s:%d/",
        leaderMasterDaemon.getLocalhostIP(), leaderMasterDaemon.getWebPort()));
    String leaderMasterHome = getContentForUrl(leaderMasterUrl);
    LOG.info("Content for " + leaderMasterUrl + ", " + leaderMasterHome);

    assertTrue(Pattern.matches(".*Num Nodes \\(TServers\\) [^0-9]*3.*", leaderMasterHome));
  }
}
