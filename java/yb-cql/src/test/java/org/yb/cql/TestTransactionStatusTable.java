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

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;

@RunWith(value=YBTestRunner.class)
public class TestTransactionStatusTable extends BaseCQLTest {
  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.addCommonTServerArgs("--TEST_txn_status_table_tablet_creation_delay_ms=5000");
  }

  @Test
  public void testCreation() throws Throwable {
    final int kTablesCount = 10;
    final CountDownLatch startSignal = new CountDownLatch(kTablesCount);
    List<ThrowingRunnable> cmds = new ArrayList<>();
    List<Session> sessions = new ArrayList<>();
    final Cluster cluster = Cluster.builder()
                                   .addContactPointsWithPorts(miniCluster.getCQLContactPoints())
                                   .build();
    for (int i = 0; i <  kTablesCount; ++i) {
      final int idx = i;
      final Session session = cluster.connect(DEFAULT_TEST_KEYSPACE);
      sessions.add(session);
      cmds.add(() -> {
        startSignal.countDown();
        startSignal.await();
        // Allow extra time on the create statements to finish because the load balancer seems to be
        // moving the leaders before all the alters corresponding to the Index Permissions finish.
        session.execute(new SimpleStatement(String.format(
            "create table test_restart_%d (k int primary key, v int) " +
            "with transactions = {'enabled' : true};", idx)).setReadTimeoutMillis(36000));
        session.execute(String.format("create index on test_restart_%d (v);", idx));
        session.execute(String.format("insert into test_restart_%s (k, v) values (1, 1000);", idx));
        ResultSet rs = session.execute(String.format("select k, v from test_restart_%d", idx));
        String actualResult = "";
        for (com.datastax.driver.core.Row row : rs) {
          actualResult += row.toString();
        }
        assertEquals("Row[1, 1000]", actualResult);
      });
    }
    MiscUtil.runInParallel(cmds, startSignal, 60);
    for (Session s : sessions) {
      s.close();
    }
  }
}
