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

import com.datastax.driver.core.Host;
import com.google.common.net.HostAndPort;
import org.junit.Test;
import org.yb.minicluster.MiniYBCluster;

import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.assertEquals;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;
import org.yb.minicluster.MiniYBClusterBuilder;

@RunWith(value=YBTestRunner.class)
public class TestEditCQLNodes extends BaseCQLTest {

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.tserverHeartbeatTimeoutMs(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000 / 2);
  }

  @Test
  public void testEditCQLNodes() throws Exception {
    assertEquals(miniCluster.getCQLContactPoints().size(),
      cluster.getMetadata().getAllHosts().size());
    assertEquals(NUM_TABLET_SERVERS, cluster.getMetadata().getAllHosts().size());

    // Add a new node.
    miniCluster.startTServer(null);

    // Wait for node list refresh.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 2 * 1000);

    // Verify we have an extra node.
    assertEquals(miniCluster.getCQLContactPoints().size(),
      cluster.getMetadata().getAllHosts().size());
    assertEquals(NUM_TABLET_SERVERS + 1, cluster.getMetadata().getAllHosts().size());

    for (Host host : cluster.getMetadata().getAllHosts()) {
      assertTrue(host.isUp());
    }

    // Remove a node.
    HostAndPort tserver = miniCluster.getTabletServers().keySet().iterator().next();
    miniCluster.killTabletServerOnHostPort(tserver);

    // Wait for node list refresh.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 2 * 1000);

    // Verify we have one less node.
    assertEquals(miniCluster.getCQLContactPoints().size(),
      cluster.getMetadata().getAllHosts().size());
  }

}
