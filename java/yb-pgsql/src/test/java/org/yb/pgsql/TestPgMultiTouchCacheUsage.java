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
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.client.YBClient;
import org.yb.minicluster.MiniYBDaemon;
import org.postgresql.util.PSQLException;
import static org.yb.AssertionWrappers.*;

import java.net.MalformedURLException;
import java.net.URL;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.List;
import java.util.Map;
import java.util.Scanner;
import java.io.BufferedReader;
import java.io.InputStreamReader;

import com.google.common.net.HostAndPort;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgMultiTouchCacheUsage extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSelect.class);

  private int getMultiTouchHitCount() throws Exception {
    // Sum the multi touch hit count from all the 3 tservers.
    YBClient client = miniCluster.getClient();
    Map<HostAndPort, MiniYBDaemon> tservers = miniCluster.getTabletServers();
    int count = 0;

    for (Map.Entry<HostAndPort,MiniYBDaemon> entry : tservers.entrySet()) {
      HostAndPort hostPort = entry.getKey();
      int port = tservers.get(hostPort).getWebPort();

      // Call the prometheus-metrics endpoint and grep for the multi touch hit metric in the list.
      URL url = null;
      BufferedReader br = null;
      try {
        url = new URL(String.format("http://%s:%d/prometheus-metrics", hostPort.getHost(), port));
        br = new BufferedReader(new InputStreamReader(url.openStream()));;
      } catch (Exception ex) {
        LOG.error("Encountered error for reading metrics endpoint" + ex);
        throw new InternalError(ex.getMessage());
      }
      String line = null;
      while ((line = br.readLine()) != null) {
        if (line.contains("rocksdb_block_cache_multi_touch_hit")) {
          String inp[] = line.split(" ");
          String x = inp[inp.length - 2].trim();
          int val = Integer.parseInt(x);
          count += val;
        }
      }
    }
    LOG.info("The multi touch hit count is " + count);
    return count;
  }

  @Test
  public void testPgMultiTouchCacheUsage() throws Exception {
    String tableName = "CacheTest";
    Statement statement = connection.createStatement();
    int kRows = 5000;

    // Create a table with a split such that all of the values go to the same tablet.
    try {
      String query = "CREATE TABLE " + tableName +
                     "(a int, b int, c int, primary key(a asc)) split at values ((1000000))";
      statement.execute(query);

      for (int ii = 1; ii <= 50; ++ii) {
        int start = kRows * ii + 1;
        int end = start + kRows - 1;
        query = "insert into " + tableName +
          " select g.id, g.id + 1, g.id + 2 from generate_series(" + start + ", " + end +
          ") AS g(id)";
        statement.execute(query);
      }
    } catch (PSQLException ex) {
      LOG.error("Unexpected exception:", ex);
      throw ex;
    }

    int count1 = getMultiTouchHitCount();
    try {
      // Perform a read on the first value.
      String query = "select * from " + tableName + " where a=" + kRows + 1;
      statement.executeQuery(query);
    } catch (PSQLException ex) {
      LOG.error("Unexpected exception:", ex);
      throw ex;
    }
    int count2 = getMultiTouchHitCount();
    assertTrue(count2 > count1);
  }
}
