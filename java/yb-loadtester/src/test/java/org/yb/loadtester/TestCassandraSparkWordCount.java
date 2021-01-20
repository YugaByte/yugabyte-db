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
import com.yugabyte.sample.apps.CassandraSparkWordCount;
import com.yugabyte.sample.common.CmdLineOpts;
import org.junit.Ignore;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.cql.BaseCQLTest;
import org.yb.minicluster.MiniYBClusterBuilder;

import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.assertEquals;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.stream.Collectors;

import org.yb.util.YBTestRunnerNonTsanOnly;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestCassandraSparkWordCount extends BaseCQLTest {

    private CassandraSparkWordCount app = new CassandraSparkWordCount();

    @Override
    protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
        super.customizeMiniClusterBuilder(builder);
        // Disable the system.partitions vtable refresh bg thread.
        builder.yqlSystemPartitionsVtableRefreshSecs(0);
    }

    protected Map<String, String> getTServerFlags() {
        Map<String, String> flagMap = super.getTServerFlags();
        flagMap.put("cql_update_system_query_cache_msecs", "1000");
        return flagMap;
    }
    @Test
    public void testDefaultRun() throws Exception {
        // Set up config.
        String nodes = miniCluster.getCQLContactPoints().stream()
                .map(addr -> addr.getHostString() + ":" + addr.getPort())
                .collect(Collectors.joining(","));
        String[] args = {"--workload", "CassandraSparkWordCount", "--nodes", nodes};
        CmdLineOpts config = CmdLineOpts.createFromArgs(args);

        // Run the app.
        app.workloadInit(config, false);
        app.run();

        // Check row.
        Map<String, Integer> expectedValues = new HashMap<>();
        expectedValues.put("one", 1);
        expectedValues.put("two", 2);
        expectedValues.put("three", 3);
        expectedValues.put("four", 4);
        expectedValues.put("five", 5);
        expectedValues.put("six", 6);
        expectedValues.put("seven", 7);
        expectedValues.put("eight", 8);
        expectedValues.put("nine", 9);
        expectedValues.put("ten", 10);

        Iterator<Row> iterator = runSelect("SELECT * FROM ybdemo_keyspace.wordcounts");
        int rows_count = 0;
        while (iterator.hasNext()) {
            Row row = iterator.next();
            String word = row.getString("word");
            assertTrue(expectedValues.containsKey(word));
            assertEquals(expectedValues.get(word).intValue(), row.getInt("count"));
            rows_count++;
        }
        assertEquals(10, rows_count);
    }
}
