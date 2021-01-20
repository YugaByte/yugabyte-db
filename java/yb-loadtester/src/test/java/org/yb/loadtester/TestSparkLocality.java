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

import com.datastax.driver.core.Row;
import com.yugabyte.sample.apps.CassandraSparkKeyValueCopy;
import com.yugabyte.sample.common.CmdLineOpts;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.cql.BaseCQLTest;
import org.yb.minicluster.IOMetrics;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.minicluster.MiniYBDaemon;

import java.util.Iterator;
import java.util.Map;
import java.util.stream.Collectors;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertTrue;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestSparkLocality extends BaseCQLTest {
  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
      super.customizeMiniClusterBuilder(builder);
      // Disable the system.partitions vtable refresh bg thread.
      builder.yqlSystemPartitionsVtableRefreshSecs(0);
  }

  // Timeout to wait for load balancing to complete.
  private static final int LOADBALANCE_TIMEOUT_MS = 120000; // 2 mins

  private static final int ROWS_COUNT = 1000;

  @Test
  public void testDefaultRun() throws Exception {
    // Set up config.
    String nodes = miniCluster.getCQLContactPoints().stream()
        .map(addr -> addr.getHostString() + ":" + addr.getPort())
        .collect(Collectors.joining(","));
    String[] args = {"--workload", "CassandraSparkKeyValueCopy", "--nodes", nodes};
    CmdLineOpts config = CmdLineOpts.createFromArgs(args);


    // Setup input table.
    session.execute("CREATE KEYSPACE ybdemo_keyspace");
    session.execute("USE ybdemo_keyspace");
    session.execute("CREATE TABLE CassandraKeyValue(k text primary key, v1 blob, v2 jsonb)");
    for (int i = 0; i < ROWS_COUNT; i++) {
      session.execute("INSERT INTO CassandraKeyValue(k, v1, v2) VALUES" +
                              "('" + i + "', 0xabcdef012345, '{\"a\" : "+ i +" }')");
    }

    // Set up the app.
    CassandraSparkKeyValueCopy app = new CassandraSparkKeyValueCopy();
    app.workloadInit(config, true);

    // Wait for load-balancing to complete to reduce chance of load-balancing affecting locality.
    miniCluster.getClient().waitForLoadBalance(LOADBALANCE_TIMEOUT_MS, NUM_TABLET_SERVERS);

    // Wait for load-balancing to become idle.
    miniCluster.getClient().waitForLoadBalancerIdle(LOADBALANCE_TIMEOUT_MS);

    // Get the initial metrics.
    Map<MiniYBDaemon, IOMetrics> initialMetrics = getTSMetrics();

    app.run();

    // Check the metrics again.
    IOMetrics totalMetrics = getCombinedMetrics(initialMetrics);

    // Get the number of partitions of the source table.
    int partitionsCount = cluster.getMetadata()
        .getTableSplitMetadata("ybdemo_keyspace", "cassandrakeyvalue").getPartitionMap().size();

    //----------------------------------------------------------------------------------------------
    // Expecting one read for every partition.

    // Check that the output table has the expected number of rows: ensure we hit all partitions.
    Iterator<Row> rows = session.execute("SELECT * from CassandraKeyValueCopy").iterator();
    int rowsCount = 0;
    while (rows.hasNext()) {
      rows.next();
      rowsCount += 1;
    }
    assertEquals(ROWS_COUNT, rowsCount);

    // Check that we did as many reads as partitions: we did not do redundant reads.
    assertEquals(partitionsCount, totalMetrics.readCount());

    //----------------------------------------------------------------------------------------------
    // Expecting one write for every row.
    assertEquals(ROWS_COUNT, totalMetrics.writeCount());

    //----------------------------------------------------------------------------------------------
    // Expect that the majority of read and write calls are local.
    //
    // With PartitionAwarePolicy, all calls should be local ideally but there is no 100% guarantee
    // because as soon as the test table has been created and the partition metadata has been
    // loaded, the cluster's load-balancer may still be rebalancing the leaders.
    assertTrue(totalMetrics.localReadCount > 2 * totalMetrics.remoteReadCount);
    assertTrue(totalMetrics.localWriteCount > 2 * totalMetrics.remoteWriteCount);
  }
}
