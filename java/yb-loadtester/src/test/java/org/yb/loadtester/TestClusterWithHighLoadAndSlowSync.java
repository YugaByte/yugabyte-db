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

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.util.SanitizerUtil;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestClusterWithHighLoadAndSlowSync extends TestClusterBase {
  private static final Logger LOG = LoggerFactory.getLogger(TestClusterBase.class);

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    // Tests hearbeat batching (initially disabled) which is very beneficial during slow sync.
    builder.addMasterArgs(
        "--catalog_manager_report_batch_size=10"
    );
    builder.addCommonTServerArgs(
        "--log_inject_latency",
        "--log_inject_latency_ms_mean=100",
        "--log_inject_latency_ms_stddev=50"
    );
  }

  @Before
  public void startLoadTester() throws Exception {
    // Start the load tester.
    LOG.info("Using contact points for load tester: " + cqlContactPoints);
    int nThreads =  SanitizerUtil.nonTsanVsTsan(16, 1);
    loadTesterRunnable = new LoadTester(WORKLOAD, cqlContactPoints, nThreads, nThreads);
    loadTesterThread = new Thread(loadTesterRunnable);
    loadTesterThread.start();
    LOG.info("Loadtester start.");
  }

  /**
   * This is an integration test that ensures we can fully move a YB cluster with a heavy load
   * without any significant impact to the load.
   */
  @Test(timeout = TEST_TIMEOUT_SEC * 1000) // 20 minutes.
  public void testClusterFullMoveWithHighLoadAndSlowSync() throws Exception {
    // Wait for load tester to generate traffic.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);

    // First try to move all the masters.
    performFullMasterMove();

    // Wait for some ops and verify no failures in load tester.
    loadTesterRunnable.waitNumOpsIncrement(NUM_OPS_INCREMENT);
    loadTesterRunnable.verifyNumExceptions();

    // Now perform a full tserver move.
    performTServerExpandShrink(true);

    verifyClusterHealth();
  }
}
