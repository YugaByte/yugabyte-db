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
import org.junit.runner.RunWith;
import org.yb.util.MiscUtil;
import org.yb.util.MiscUtil.ThrowingRunnable;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestTransactionStatusTable extends BasePgSQLTest {
  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flags = super.getTServerFlags();
    flags.put("TEST_txn_status_table_tablet_creation_delay_ms", "10000");
    return flags;
  }

  @Test
  public void testCreation() throws Throwable {
    final int kTablesCount = 10;
    final CountDownLatch startSignal = new CountDownLatch(kTablesCount);
    List<ThrowingRunnable> cmds = new ArrayList<>();
    for (int i = 0; i <  kTablesCount; ++i) {
      final int idx = i;
      cmds.add(() -> {
        try (Statement stmt = getConnectionBuilder().connect().createStatement()) {
          startSignal.countDown();
          startSignal.await();
          // Check table is created in spite of the fact transaction status table is not ready yet.
          stmt.execute(String.format("CREATE TABLE t_%d(k INT, v INT)", idx));
          assertOneRow(stmt, String.format("SELECT COUNT(*) FROM t_%d", idx), 0L);
        }
      });
    }
    MiscUtil.runInParallel(cmds, startSignal, 60);
  }
}
