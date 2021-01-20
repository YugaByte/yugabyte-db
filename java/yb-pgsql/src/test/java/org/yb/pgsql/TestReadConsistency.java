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
import org.postgresql.util.PSQLException;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.Map;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.atomic.AtomicBoolean;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestReadConsistency extends BasePgSQLTest {
  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flags = super.getTServerFlags();
    flags.put("TEST_transactional_read_delay_ms", "100");
    return flags;
  }

  @Test
  public void testHighConcurrency() throws Exception {
    try(Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE parent(k INT PRIMARY KEY)");
      stmt.execute("CREATE TABLE child(k INT PRIMARY KEY, v INT REFERENCES parent(k))");
      stmt.execute("CREATE TABLE grandchild(k INT PRIMARY KEY, v INT REFERENCES child(k))");
      final int count = 30;
      final int itemsCount = 500;
      final AtomicBoolean fkViolationDetected = new AtomicBoolean(false);
      final CyclicBarrier barrier = new CyclicBarrier(count);
      Thread[] threads = new Thread[count];
      for (int i = 0; i < count; ++i) {
        Connection conn = getConnectionBuilder().connect();
        threads[i] = new Thread(() -> {
          try {
            try (Statement ls = conn.createStatement()) {
              for (int j = 0; j < itemsCount && !fkViolationDetected.get(); ++j) {
                barrier.await();
                final int parentItem = 100000 + j;
                final int childItem = 200000 + j;
                try {
                  ls.execute("ROLLBACK");
                  ls.execute("BEGIN");
                  ls.execute(
                    String.format("INSERT INTO parent VALUES(%d)", parentItem));
                  ls.execute(
                    String.format("INSERT INTO child VALUES(%d, %d)", childItem, parentItem));
                  ls.execute(
                    String.format("INSERT INTO grandchild VALUES(%d, %d)", 300000 + j, childItem));
                  ls.execute("COMMIT");
                } catch (PSQLException e) {
                  if (e.getMessage().contains("violates foreign key constraint")) {
                    fkViolationDetected.set(true);
                    barrier.reset();
                  }
                }
              }
            }
          } catch (Exception e) {
          }
        });
        threads[i].start();
      }
      for (int i = 0; i < count; ++i) {
        threads[i].join();
      }
      assertFalse(fkViolationDetected.get());
      ResultSet rs = stmt.executeQuery("SELECT COUNT(*) FROM grandchild");
      rs.next();
      final int rowCount = rs.getInt(1);
      assertEquals(itemsCount, rowCount);
    }
  }
}
