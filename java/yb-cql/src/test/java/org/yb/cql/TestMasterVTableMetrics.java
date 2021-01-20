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


import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import com.datastax.driver.core.exceptions.TransportException;
import com.google.common.net.HostAndPort;
import org.apache.commons.lang3.RandomStringUtils;
import org.junit.Test;
import org.yb.client.TestUtils;

import org.yb.minicluster.IOMetrics;
import org.yb.minicluster.Metrics;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.MiniYBDaemon;
import org.yb.minicluster.RocksDBMetrics;

import java.net.InetAddress;
import java.text.SimpleDateFormat;
import java.util.*;

import static org.yb.AssertionWrappers.assertNull;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertGreaterThan;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value = YBTestRunner.class)
public class TestMasterVTableMetrics extends BaseCQLTest {

  protected static final String[] TABLES = {
      "local",
      "partitions",
      "size_estimates" };

  @Override
  public int getTestMethodTimeoutSec() {
    // No need to adjust for TSAN vs. non-TSAN here, it will be done automatically.
    return 240;
  }

  private Map<String, Integer> getYcqlSystemQueryStats() throws Exception {
    Map<HostAndPort, MiniYBDaemon> masters = miniCluster.getMasters();
    Map<String, Integer> stats = new HashMap<String, Integer>();
    for (MiniYBDaemon master : masters.values()) {
      for (String i : TABLES) {
        Metrics.Histogram h = new Metrics(master.getLocalhostIP(),
                                          master.getWebPort(), "server")
                                          .getHistogram("ycql_queries_system_" + i);

        if (h != null) {
          Integer v = stats.get(i);
          if (v == null) {
            stats.put(i, h.totalSum);
          } else {
            v += h.totalSum;
          }
        }
      }
    }
    return stats;
  }

  @Test
  public void testMasterMetrics() throws Exception {
    Map<String, Integer> before = getYcqlSystemQueryStats();

    for (String i : TABLES) {
      session.execute(String.format("select * from system.%s", i));
    }

    Map<String, Integer> after = getYcqlSystemQueryStats();

    for (String i : TABLES) {
      int diff = after.get(i) - before.get(i);
      assertGreaterThan(diff, 0);
    }
  }
}
