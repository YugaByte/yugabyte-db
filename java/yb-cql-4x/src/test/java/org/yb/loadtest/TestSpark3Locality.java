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
package org.yb.loadtest;

import com.datastax.oss.driver.api.core.cql.Row;
import com.datastax.oss.driver.api.core.CqlSession;
import com.yugabyte.sample.apps.CassandraSparkKeyValueCopy;
import com.yugabyte.sample.common.CmdLineOpts;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.minicluster.BaseMiniClusterTest;
import org.yb.minicluster.IOMetrics;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.minicluster.MiniYBDaemon;
import com.yugabyte.oss.driver.api.core.DefaultPartitionMetadata;
import com.yugabyte.oss.driver.api.core.TableSplitMetadata;
import com.yugabyte.sample.apps.CassandraSparkWordCount;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.Iterator;
import java.util.Map;
import java.util.Optional;
import java.util.stream.Collectors;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertTrue;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestSpark3Locality extends BaseMiniClusterTest {
  private Logger logger = LoggerFactory.getLogger(TestSpark3Locality.class);

  // Timeout to wait for load balancing to complete.
  private static final int LOADBALANCE_TIMEOUT_MS = 120000; // 2 mins

  private static final int ROWS_COUNT = 1000;

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
      super.customizeMiniClusterBuilder(builder);
      // Disable the system.partitions vtable refresh bg thread.
      builder.yqlSystemPartitionsVtableRefreshSecs(0);
  }

  @Test
  public void testDefaultRun() throws Exception {
    // Set up config.
    String nodes = miniCluster.getCQLContactPoints().stream()
        .map(addr -> addr.getHostString() + ":" + addr.getPort())
        .collect(Collectors.joining(","));
    String[] args = {"--workload", "CassandraSparkKeyValueCopy", "--nodes", nodes};
    CmdLineOpts config = CmdLineOpts.createFromArgs(args);

    // Set up the app.
    CassandraSparkKeyValueCopy app = new CassandraSparkKeyValueCopy();
    app.workloadInit(config, true);

    CqlSession session = app.getCqlSession();
    app.createKeyspace(session, "ybdemo_keyspace");

    // Setup input table.
    //session.execute("USE ybdemo_keyspace");
    session.execute(
      "CREATE TABLE ybdemo_keyspace.CassandraKeyValue(k text primary key, v1 blob, v2 jsonb)");
    for (int i = 0; i < ROWS_COUNT; i++) {
      session.execute("INSERT INTO ybdemo_keyspace.CassandraKeyValue(k, v1, v2) VALUES" +
                              "('" + i + "', 0xabcdef012345, '{\"a\" : "+ i +" }')");
    }

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
    Optional<DefaultPartitionMetadata> metadata = session.getMetadata()
      .getDefaultPartitionMetadata();
    int partitionsCount = 0;
    if (metadata.isPresent()) {
      TableSplitMetadata splitMeta = metadata.get()
        .getTableSplitMetadata("ybdemo_keyspace", "cassandrakeyvalue");
      if (splitMeta != null) {
        partitionsCount = splitMeta.getPartitionMap().size();
        logger.info("retrieved split meta " + partitionsCount);
      }
    }

    //--------------------------------------------------------------------------------------------
    // Expecting one read for every partition.

    // Check that the output table has the expected number of rows: ensure we hit all partitions.
    Iterator<Row> rows = session.execute("SELECT * from ybdemo_keyspace.CassandraKeyValueCopy")
      .iterator();
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
