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

import static org.yb.AssertionWrappers.assertEquals;

import com.datastax.driver.core.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.YBTestRunner;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.util.MiscUtil;
import org.yb.util.MiscUtil.ThrowingRunnable;
import org.yb.util.SanitizerUtil;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;

@RunWith(value=YBTestRunner.class)
public class TestTransactionStatusTable extends BaseCQLTest {
  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.addCommonTServerArgs("--TEST_txn_status_table_tablet_creation_delay_ms=5000");
    // Adjust following flags, so delay of txn status tablets opening doesn't block the whole
    // tablets opening thread pool.
    builder.addCommonTServerArgs("--transaction_table_num_tablets=4");
    builder.addCommonTServerArgs("--num_tablets_to_open_simultaneously=8");
    // Reduce the number of tablets per table.
    builder.addMasterArgs("--yb_num_shards_per_tserver=1");
  }

  @Test
  public void testCreation() throws Throwable {
    final int kTablesCount = SanitizerUtil.nonTsanVsTsan(4, 2);
    final CountDownLatch startSignal = new CountDownLatch(kTablesCount);
    List<ThrowingRunnable> cmds = new ArrayList<>();
    List<Session> sessions = new ArrayList<>();
    final Cluster cluster = getDefaultClusterBuilder().build();
    for (int i = 0; i <  kTablesCount; ++i) {
      final int idx = i;
      final Session session = cluster.connect(DEFAULT_TEST_KEYSPACE);
      sessions.add(session);
      cmds.add(() -> {
        startSignal.countDown();
        startSignal.await();
        final String tableName = String.format("test_restart_%d", idx);
        LOG.info("Creating table " + tableName + "...");
        // Allow extra time on the create statements to finish because the load balancer seems to be
        // moving the leaders before all the alters corresponding to the Index Permissions finish.
        session.execute(
            new SimpleStatement(String.format(
                "create table %s (k int primary key, v int) " +
                "with transactions = {'enabled' : true};", tableName))
            .setReadTimeoutMillis((int) (36000 * SanitizerUtil.getTimeoutMultiplier())));
        LOG.info("Created table " + tableName);
        session.execute(String.format("create index on %s (v);", tableName));
        LOG.info("Created index on " + tableName);
        session.execute(String.format("insert into %s (k, v) values (1, 1000);", tableName));
        LOG.info("Inserted data into " + tableName);
        ResultSet rs = session.execute(String.format("select k, v from %s", tableName));
        LOG.info("Selected data from " + tableName);
        String actualResult = "";
        for (com.datastax.driver.core.Row row : rs) {
          actualResult += row.toString();
        }
        assertEquals("Row[1, 1000]", actualResult);
        LOG.info("Checked selected data from " + tableName);
      });
    }
    MiscUtil.runInParallel(cmds, startSignal, (int) (60 * SanitizerUtil.getTimeoutMultiplier()));
    for (Session s : sessions) {
      s.close();
    }
  }
}
