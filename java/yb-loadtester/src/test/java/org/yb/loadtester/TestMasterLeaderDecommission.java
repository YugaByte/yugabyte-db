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

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertTrue;

import com.google.common.net.HostAndPort;

import org.junit.Test;

import java.util.*;

/**
 * This is an integration test that ensures we can do a master decommission.
 */
import org.yb.util.YBTestRunnerNonTsanOnly;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestMasterLeaderDecommission extends TestClusterBase {
  @Test(timeout = TEST_TIMEOUT_SEC * 1000) // 20 minutes.
  public void testMasterLeaderDecommission() throws Exception {
    // Wait for load tester to generate traffic.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);
    // Disable heartbeats for all tservers.
    for (HostAndPort hp : miniCluster.getTabletServers().keySet()) {
      assertTrue(client.setFlag(hp, "tserver_disable_heartbeat_test_only", "true"));
    }

    // Disable becoming leader in 2 master followers.
    HostAndPort leaderMasterHp = client.getLeaderMasterHostAndPort();
    for (HostAndPort hp : miniCluster.getMasters().keySet()) {
      if (!hp.equals(leaderMasterHp)) {
        assertTrue(client.setFlag(hp, "TEST_do_not_start_election_test_only", "true"));
      }
    }

    // Add a new master to the config.
    Set<HostAndPort> newMaster = createAndAddNewMasters(1);

    // Decomission master leader, after this, the added master should become leader.
    removeMaster(leaderMasterHp);

    // Enable heartbeats for all tservers.
    for (HostAndPort hp : miniCluster.getTabletServers().keySet()) {
      assertTrue(client.setFlag(hp, "tserver_disable_heartbeat_test_only", "false"));
    }

    // Wait for tservers to find and heartbeat to new master.
    Thread.sleep(miniCluster.getClusterParameters().getTServerHeartbeatTimeoutMs() * 5);

    for (HostAndPort hp : miniCluster.getTabletServers().keySet()) {
      String masters = client.getMasterAddresses(hp);
      // Assert each tserver knows only the list of all 3 masters
      // as it should have heartbeated to master leader.
      assertEquals(3, masters.split(",").length);
      assertFalse(masters.contains(leaderMasterHp.getHost()));
      assertTrue(masters.contains(newMaster.iterator().next().getHost()));
    }

    // Wait for some ops and verify no failures in load tester.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);
    loadTesterRunnable.verifyNumExceptions();

    verifyClusterHealth();
  }
}
