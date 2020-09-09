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
package org.yb.pgsql;

import org.junit.Test;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.LeaderStepDownResponse;

import org.yb.client.YBClient;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.HashSet;
import java.util.Set;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;

import org.junit.runner.RunWith;
import org.yb.minicluster.MiniYBCluster;
import org.yb.util.YBTestRunnerNonTsanOnly;

@RunWith(YBTestRunnerNonTsanOnly.class)
public class TestPgCatalogVersionPersistence extends BasePgSQLTest {

  protected static final Logger LOG =
      LoggerFactory.getLogger(TestPgCatalogVersionPersistence.class);

  @Override
  protected int getReplicationFactor() {
    // Ensure we always use RF3 for this test (specifically need at least 3 masters).
    return 3;
  }

  /**
   * This unit test verifies that if the master changes the YSQL catalog (e.g. version) persists.
   */
  @Test
  public void testMasterChange() throws Exception {
    Set<Row> expectedRows = new HashSet<>();

    try (Statement statement = connection.createStatement()) {
      // Run a few statements (DDLs) to increment the catalog version.
      statement.execute("CREATE TABLE catver_test1(a int primary key, b int)");
      statement.execute("CREATE TABLE catver_test2(a text, b int, primary key (a,b))");
      statement.execute("INSERT INTO catver_test1 VALUES (1,1)");
      statement.execute("INSERT INTO catver_test2 VALUES (1,1)");
      statement.execute("DROP TABLE catver_test2");

      // Trigger a master leader change by asking leader to step down.
      YBClient client = miniCluster.getClient();
      LeaderStepDownResponse resp = client.masterLeaderStepDown();
      assertFalse(resp.hasError());

      // Wait for a few heartbeats to ensure new catalog version is propagated.
      Thread.sleep(5 * MiniYBCluster.TSERVER_HEARTBEAT_INTERVAL_MS);

      // Check that existing connections (and cluster) are still usable.
      statement.execute("INSERT INTO catver_test1 VALUES (2,2)");
      statement.execute("ALTER TABLE catver_test1 ADD COLUMN c int");
      statement.execute("INSERT INTO catver_test1 VALUES (3,3,3)");
      expectedRows.add(new Row(1, 1, null));
      expectedRows.add(new Row(2, 2, null));
      expectedRows.add(new Row(3, 3, 3));
      ResultSet rs = statement.executeQuery("SELECT * from catver_test1");
      assertEquals(expectedRows, getRowSet(rs));

      // Trigger a master leader change by killing current leader.
      miniCluster.killMasterOnHostPort(client.getLeaderMasterHostAndPort());

      // Wait for a few heartbeats to ensure new leader elected and catalog version is propagated.
      Thread.sleep(10 * MiniYBCluster.TSERVER_HEARTBEAT_INTERVAL_MS);

      // Check that existing connections (and cluster) are still usable.
      statement.execute("INSERT INTO catver_test1 VALUES (4,4,4)");
      rs = statement.executeQuery("SELECT * from catver_test1");
      expectedRows.add(new Row(4, 4, 4));
      assertEquals(expectedRows, getRowSet(rs));
    }

    // Test a new connections: it will get initialized with the new master's version.
    try (Connection connection2 = getConnectionBuilder().connect();
         Statement statement = connection2.createStatement()) {
      statement.execute("INSERT INTO catver_test1 VALUES (5,5,5)");
      expectedRows.add(new Row(5, 5, 5));
      ResultSet rs = statement.executeQuery("SELECT * from catver_test1");
      assertEquals(expectedRows, getRowSet(rs));
    }
  }

}
