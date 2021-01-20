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

import java.sql.Statement;
import java.util.Map;
import java.util.HashMap;

import static org.yb.AssertionWrappers.assertEquals;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgIndexSelectiveUpdate extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgIndexSelectiveUpdate.class);

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("TEST_export_intentdb_metrics", "true");
    return flagMap;
  }

  // Start server in RF=1 mode to make test non-flaky
  @Override
  protected int getReplicationFactor() {
    return 1;
  }

  private static final String TABLE_NAME = "test";
  private static final String METRIC_NAME = "intentsdb_rocksdb_write_self";
  private static final String[] index_list = {"idx_col3", "idx_col5", "idx_col6",
      "idx_col4_idx_col5_idx_col6", "idx_col8"};
  private static Map<String, Counter> tableWrites = new HashMap<>();

  private void prepareTest(Statement statement) throws Exception {
    statement.execute(String.format(
        "CREATE TABLE %s(pk int, col2 int, col3 int, col4 int, col5 int, col6 int, col7 int, "+
        "col8 int, col9 int, PRIMARY KEY (pk ASC))", TABLE_NAME));
    statement.execute(String.format(
        "CREATE INDEX %s on %s(col3 ASC) INCLUDE (col4,col5,col6)", index_list[0], TABLE_NAME));
    statement.execute(String.format(
        "CREATE INDEX %s on %s(col5 ASC) INCLUDE (col6,col7)", index_list[1], TABLE_NAME));
    statement.execute(String.format(
        "CREATE INDEX %s on %s(col6 ASC) INCLUDE (col9)", index_list[2], TABLE_NAME));
    statement.execute(String.format(
        "CREATE INDEX %s on %s(col4 ASC, col5 ASC, col6 ASC)", index_list[3], TABLE_NAME));
    statement.execute(String.format(
        "CREATE INDEX %s on %s(col8 ASC)", index_list[4], TABLE_NAME));

    // Inserting values into the table
    statement.execute(String.format("INSERT INTO %s VALUES (1,1,1,1,1,1,1,1,1)", TABLE_NAME));
    statement.execute(String.format("INSERT INTO %s VALUES (2,2,2,2,2,2,2,2,2)", TABLE_NAME));
    statement.execute(String.format("INSERT INTO %s VALUES (3,3,3,3,3,3,3,3,3)", TABLE_NAME));
    statement.execute(String.format("INSERT INTO %s VALUES (4,4,4,4,4,4,4,4,4)", TABLE_NAME));
    statement.execute(String.format("INSERT INTO %s VALUES (5,5,5,5,5,5,5,5,5)", TABLE_NAME));
    statement.execute(String.format("INSERT INTO %s VALUES (6,6,6,6,6,6,6,6,6)", TABLE_NAME));

    // Initializing a map datastructure to store metrics for each table name
    for (String table : index_list) {
      tableWrites.put(table, new Counter());
    }
  }

  private static class Counter {
    private long previousValue_ = 0;
    private long currentValue_ = 0;

    public void update(long newValue) {
      previousValue_ = currentValue_;
      currentValue_ = newValue;
    }

    public long getDelta() {
      return currentValue_ - previousValue_;
    }
  }

  private void checkWrites(int col3, int col5, int col6, int col4col5col6, int col8) {
    assertEquals(col3, tableWrites.get("idx_col3").getDelta());
    assertEquals(col5, tableWrites.get("idx_col5").getDelta());
    assertEquals(col6, tableWrites.get("idx_col6").getDelta());
    assertEquals(col4col5col6, tableWrites.get("idx_col4_idx_col5_idx_col6").getDelta());
    assertEquals(col8, tableWrites.get("idx_col8").getDelta());
  }

  private void updateCounter() throws Exception {
    for (String table : index_list) {
      Counter writes = tableWrites.get(table);
      writes.update(getTserverMetricCountForTable(METRIC_NAME, table));
    }
  }

  @Test
  public void testUpdateTableIndexWrites() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      prepareTest(stmt);
      // Add the value of metrics before updating the table test
      updateCounter();

      // column 4 is changed. this changes idx_col3, idx_col4_idx_col5_idx_col6.
      stmt.execute(String.format("update %s set col4=11 where pk=1", TABLE_NAME));
      updateCounter();
      checkWrites(3, 0, 0, 3, 0);

      // column 6 is changed. this changes idx_col3, idx_col5, idx_col6, idx_col4_idx_col5_idx_col6.
      stmt.execute(String.format("update %s set col6=12 where pk=1", TABLE_NAME));
      updateCounter();
      checkWrites(3, 3, 3, 3, 0);

      // column 5 is changed. this changes idx_col3, idx_col5, idx_col4_idx_col5_idx_col6.
      stmt.execute(String.format("update %s set col5=13 where pk=1", TABLE_NAME));
      updateCounter();
      checkWrites(3, 3, 0, 3, 0);

      // column 9 is changed. this changes idx_col6.
      stmt.execute(String.format("update %s set col9=14 where pk=1", TABLE_NAME));
      updateCounter();
      checkWrites(0, 0, 3, 0, 0);

      // column 2 is changed. this does not affect any index.
      stmt.execute(String.format("update %s set col2=15 where pk=1", TABLE_NAME));
      updateCounter();
      checkWrites(0, 0, 0, 0, 0);

      // column 9 is changed for multiple rows.
      stmt.execute(String.format("update %s set col9=21 where pk>1", TABLE_NAME));
      updateCounter();
      checkWrites(0, 0, 7, 0, 0);

      // column 8 is changed. No include columns hence just the table and index are updated.
      stmt.execute(String.format("update %s set col8=35 where pk=1", TABLE_NAME));
      updateCounter();
      checkWrites(0, 0, 0, 0, 2);
    }
  }
}
