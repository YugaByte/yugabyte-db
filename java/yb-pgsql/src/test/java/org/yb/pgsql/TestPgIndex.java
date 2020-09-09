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

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.Arrays;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertTrue;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgIndex extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgIndex.class);

  @Test
  public void testConcurrentInsert() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE t(value INT NOT NULL UNIQUE, worker_idx INT NOT NULL)");
      final int recordCount = 500;
      Worker[] workers = new Worker[3];
      for (int i = 0; i < workers.length; ++i) {
        workers[i] = new Worker(getConnectionBuilder(), i, recordCount);
      }
      LOG.info("Starting workers");
      Arrays.stream(workers).forEach(Worker::start);
      LOG.info("Waiting for results");
      int insertedCount = Arrays.stream(workers).mapToInt(w -> w.result()).sum();

      assertEquals(recordCount, insertedCount);

      statement.execute("SELECT worker_idx, COUNT(value) FROM t GROUP BY worker_idx");
      ResultSet groups = statement.getResultSet();
      int groupCount = 0;
      while (groups.next()) {
        LOG.info("Worker {} inserted {} items", groups.getInt(1), groups.getInt(2));
        ++groupCount;
      }

      assertTrue(groupCount > 1);
      assertOneRow(statement, "SELECT COUNT(*) FROM t", (long) recordCount);
    }
  }

  private static class Worker implements Runnable {
    private volatile int insertedCount = 0;
    final private ConnectionBuilder connBldr;
    final private int idx;
    final private int count;
    final private Thread thread = new Thread(this);

    Worker(ConnectionBuilder connBldr, int idx, int count) {
      this.connBldr = connBldr;
      this.idx = idx;
      this.count = count;
    }

    void start() {
      thread.start();
    }

    int result() {
      try {
        thread.join();
      } catch (InterruptedException e) {
        LOG.error("Exception", e);
      }
      LOG.info("Worker {} finished with {} items", idx, insertedCount);
      return insertedCount;
    }

    @Override
    public void run() {
      LOG.info("Worker {} started", idx);
      int insertedCount = 0;
      try (Connection conn = connBldr.connect();
          Statement statement = conn.createStatement()) {
        for (int i = 0; i < count; ++i) {
          try {
            statement.execute(String.format("INSERT INTO t values(%d, %d)", i, idx));
            ++insertedCount;
            Thread.sleep(1);
          } catch (SQLException e) {
            LOG.info("Insert by worker#{} expectedly failed due to {}", idx, e.getMessage());
          }
        }
      } catch (Exception e) {
        LOG.error("Exception", e);
      }
      this.insertedCount = insertedCount;
    }
  }

  @Test
  public void testCreateTableWithDuplicateIndexes() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int," +
          "CONSTRAINT tt_id_pkey PRIMARY KEY(id)," +
          "CONSTRAINT tt_id_unq UNIQUE(id))");

      statement.execute("INSERT INTO test_table(id) VALUES (1), (2), (3)");

      // Primary key was added.
      runInvalidQuery(
          statement,
          "INSERT INTO test_table VALUES (1)",
          "duplicate key value violates unique constraint \"tt_id_pkey\""
      );

      // Unique constraint was not added.
      assertQuery(statement,
          "SELECT indexname FROM pg_indexes WHERE tablename='test_table'",
          new Row("tt_id_pkey"));
    }
  }
}
