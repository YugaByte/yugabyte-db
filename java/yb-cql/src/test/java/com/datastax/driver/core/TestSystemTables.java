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
package com.datastax.driver.core;

import org.junit.Test;
import org.yb.cql.BaseCQLTest;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import static junit.framework.TestCase.assertTrue;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertNotEquals;

// This test verifies that 'system.peers' table doesn't return an entry for the node that we're
// connected to. In addition to this, we also test that the 'system.local' table returns only the
// node that we are connected to. For this purpose, we need to look into a few internals of the
// datastax driver and hence we are in the 'com.datastax.driver.core' package.
import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestSystemTables extends BaseCQLTest {

  protected Map<String, String> getTServerFlags() {
      Map<String, String> flagMap = super.getTServerFlags();
      flagMap.put("cql_update_system_query_cache_msecs", "0");
      return flagMap;
  }

  @Test
  public void testSystemTables() throws Exception {
    List<InetSocketAddress> contactPoints = miniCluster.getCQLContactPoints();
    assertEquals(NUM_TABLET_SERVERS, contactPoints.size());

    TranslatedAddressEndPoint translatedContactPointToConnect;
    // Chose one contact point to connect.
    for (InetSocketAddress contactPointToConnect : contactPoints) {

      // Initialize connection.
      translatedContactPointToConnect = new TranslatedAddressEndPoint(contactPointToConnect);
      Host host = new Host(translatedContactPointToConnect, cluster.manager.convictionPolicyFactory,
            cluster.manager);
      Connection connection = cluster.manager.connectionFactory.open(host);

      // Run query and verify.
      DefaultResultSetFuture peersFuture = new DefaultResultSetFuture(null,
        cluster.manager.protocolVersion(), new Requests.Query("SELECT * FROM system.peers"));
      connection.write(peersFuture);
      List<Row> rows = peersFuture.get().all();
      assertEquals(NUM_TABLET_SERVERS - 1, rows.size());

      // Build set of all contact points.
      Set<InetAddress> contactPointsSet = new HashSet<>();
      for (InetSocketAddress contactPoint : contactPoints) {
        contactPointsSet.add(contactPoint.getAddress());
      }

      for (Row row : rows) {
        // Connected host should not be in result.
        InetAddress peer = row.getInet("peer");
        assertNotEquals(peer, contactPointToConnect.getAddress());

        // Other contact points should be present.
        assertTrue(contactPointsSet.remove(peer));
      }

      // Only connected host should be left in the set.
      assertEquals(1, contactPointsSet.size());
      assertTrue(contactPointsSet.contains(contactPointToConnect.getAddress()));

      // Verify the local table contains the connected host.
      DefaultResultSetFuture localFuture = new DefaultResultSetFuture(null,
        cluster.manager.protocolVersion(), new Requests.Query("SELECT * FROM system.local"));
      connection.write(localFuture);
      rows = localFuture.get().all();
      assertEquals(1, rows.size());
      assertEquals(contactPointToConnect.getAddress(), rows.get(0).getInet("broadcast_address"));
      assertEquals(contactPointToConnect.getAddress(), rows.get(0).getInet("listen_address"));
      assertEquals(contactPointToConnect.getAddress(), rows.get(0).getInet("rpc_address"));
    }
  }
}
