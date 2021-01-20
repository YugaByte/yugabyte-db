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

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.minicluster.Metrics;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

import static org.yb.AssertionWrappers.*;

@RunWith(YBTestRunnerNonTsanOnly.class)
public class TestPgForeignKeyBatching extends BasePgSQLTest {
  private final static int MAX_BATCH_SIZE = 512;

  // Start server in RF=1 mode to simplify metrics analysis.
  @Override
  protected int getReplicationFactor() {
    return 1;
  }

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("ysql_session_max_batch_size", Integer.toString(MAX_BATCH_SIZE));
    flagMap.put("TEST_export_intentdb_metrics", "true");
    return flagMap;
  }

  private static class Counter {
    private int previousValue_ = 0;
    private int currentValue_ = 0;

    public void update(int newValue) {
      previousValue_ = currentValue_;
      currentValue_ = newValue;
    }

    public int value() {
      return currentValue_ - previousValue_;
    }
  }

  private static class OperationsCounter {
    public Map<String, Counter> tableWrites = new HashMap<>();
    public Counter rpc = new Counter();

    public OperationsCounter(String... tableNames) {
      for (String table : tableNames) {
        tableWrites.put(table, new Counter());
      }
    }
  }

  private OperationsCounter updateCounter(OperationsCounter counter) throws Exception {
    JsonArray[] metrics = getRawTSMetric();
    assertEquals(1, metrics.length);
    for (JsonElement el : metrics[0]) {
      JsonObject obj = el.getAsJsonObject();
      String metricType = obj.get("type").getAsString();
      if (metricType.equals("server") && obj.get("id").getAsString().equals("yb.tabletserver")) {
        counter.rpc.update(new Metrics(obj).getCounter("rpc_inbound_calls_created").value);
      } else if (metricType.equals("tablet")) {
        // Assume each table has single tablet, for this purposes PRIMARY KEY(k ASC) is used.
        String tableName = obj.getAsJsonObject("attributes").get("table_name").getAsString();
        Counter writes = counter.tableWrites.get(tableName);
        if (writes != null) {
          writes.update(new Metrics(obj).getCounter("intentsdb_rocksdb_write_self").value);
        }
      }
    }
    return counter;
  }

  @Test
  public void testInsertBatching() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE parent(k INT PRIMARY KEY)");
      stmt.execute("CREATE TABLE child(k INT, v INT REFERENCES parent(k), PRIMARY KEY(k ASC))");
      stmt.execute("INSERT INTO parent SELECT 1000000 + s FROM generate_series(1, 10000) AS s");
      // Warm up internal caches.
      stmt.execute("INSERT INTO child VALUES(0, 1000001)");
      OperationsCounter counter = updateCounter(new OperationsCounter("child"));
      stmt.execute("INSERT INTO child SELECT s, 1000000 + s FROM generate_series(1, 10000) AS s");
      updateCounter(counter);
      // Each row has at least one write and one read (for trigger).
      // Both read and write operations must be buffered.
      final int readOperationsCount = 10000;
      final int writeOperationsCount = 10000;
      final int childWrite = counter.tableWrites.get("child").value();
      assertEquals(childWrite, (int) Math.ceil((double)writeOperationsCount / MAX_BATCH_SIZE));
      assertLessThan(counter.rpc.value() - childWrite, readOperationsCount);
    }
  }

  @Test
  public void testUpdateBatching() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE parent(k INT PRIMARY KEY, v INT UNIQUE)");
      stmt.execute("CREATE TABLE child(k INT, v INT REFERENCES parent(v), PRIMARY KEY(k ASC))");
      stmt.execute("INSERT INTO parent SELECT s, 1000000 + s FROM generate_series(1, 10000) AS s");
      stmt.execute("INSERT INTO child SELECT s, 1000000 + s FROM generate_series(1, 10000) AS s");
      OperationsCounter counter = updateCounter(new OperationsCounter());
      stmt.execute("UPDATE child SET v = v + 1 WHERE k < 9999");
      updateCounter(counter);
      // Update operation is pushed down to DocDB. Each trigger requires read.
      final int operationsCount = 10000;
      assertLessThan(counter.rpc.value(), operationsCount);
    }
  }

  @Test
  public void testDeferredInsertBatching() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE parent(k INT PRIMARY KEY)");
      final int childrenCount = 15;
      stmt.execute("INSERT INTO parent SELECT 1000000 + s FROM generate_series(1, 10000) AS s");
      String[] tables = new String[childrenCount];
      for (int i = 0; i < childrenCount; ++i) {
        tables[i] = String.format("child_%d", i + 1);
        stmt.execute(
          String.format("CREATE TABLE %s(k INT, " +
                        "v INT REFERENCES parent(k) DEFERRABLE INITIALLY DEFERRED, " +
                        "PRIMARY KEY(k ASC))",
                        tables[i]));
        // Warm up internal caches.
        stmt.execute(String.format("INSERT INTO child_%d VALUES(0, 1000001)", i + 1));
      }
      stmt.execute("BEGIN");
      OperationsCounter counter = updateCounter(new OperationsCounter(tables));
      for (int i = 0; i < childrenCount; ++ i) {
        stmt.execute(String.format(
          "INSERT INTO child_%d SELECT s, 1000000 + s FROM generate_series(1, 10000) AS s", i + 1));
      }
      stmt.execute("COMMIT");
      updateCounter(counter);
      // Each row has one write.
      final int expectedWriteRPC = (int)Math.ceil(10000.0 / MAX_BATCH_SIZE);
      for (Counter tableWrite : counter.tableWrites.values()) {
        assertEquals(tableWrite.value(), expectedWriteRPC);
      }
      // Read for trigger must be called once due to internal FK cache.
      assertLessThan(counter.rpc.value() - childrenCount * expectedWriteRPC, 10000);
    }
  }

  @Test
  public void testConcurrency() throws Exception {
    try (Connection extraConnection = getConnectionBuilder().connect();
         Statement stmt = connection.createStatement();
         Statement extraStmt = extraConnection.createStatement()) {
      stmt.execute("SET yb_transaction_priority_lower_bound = 0.5");
      extraStmt.execute("SET yb_transaction_priority_upper_bound = 0.4");
      stmt.execute("CREATE TABLE parent(k INT PRIMARY KEY)");
      stmt.execute("CREATE TABLE child(k INT PRIMARY KEY, v INT REFERENCES parent(k))");
      stmt.execute("INSERT INTO parent VALUES (1), (2), (3), (4), (5)");
      stmt.execute("BEGIN");
      stmt.execute("INSERT INTO child VALUES(1, 1), (2, 2), (3, 3)");
      runInvalidQuery(extraStmt,
        "DELETE FROM parent WHERE k = 1",
        "Conflicts with higher priority transaction");
      stmt.execute("COMMIT");

      stmt.execute("DELETE FROM child");
      stmt.execute("BEGIN ISOLATION LEVEL REPEATABLE READ");
      stmt.execute("INSERT INTO child VALUES (1, 1), (2, 2), (3, 3)");
      runInvalidQuery(extraStmt,
        "DELETE FROM parent WHERE k = 1",
        "Conflicts with higher priority transaction");
      stmt.execute("COMMIT");
    }
  }
}
