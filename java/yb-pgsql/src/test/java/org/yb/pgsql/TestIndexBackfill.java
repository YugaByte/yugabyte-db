package org.yb.pgsql;

import static org.yb.AssertionWrappers.*;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

import org.junit.*;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.yb.util.SanitizerUtil;
import org.yb.util.ThreadUtil;
import org.yb.util.YBTestRunnerNonTsanOnly;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestIndexBackfill extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestIndexBackfill.class);

  private static final int AWAIT_TIMEOUT_SEC = (int) (80 * SanitizerUtil.getTimeoutMultiplier());

  @Override
  protected Map<String, String> getMasterAndTServerFlags() {
    Map<String, String> flagMap = super.getMasterAndTServerFlags();
    flagMap.put("ysql_disable_index_backfill", "false");
    return flagMap;
  }

  @Override
  protected Map<String, String> getMasterFlags() {
    Map<String, String> flagMap = super.getMasterFlags();
    flagMap.put("TEST_slowdown_backfill_alter_table_rpcs_ms", "2000");
    return flagMap;
  }

  @Test
  public void insertsWhileCreatingIndex() throws Exception {
    int minThreads = 2;
    int insertsChunkSize = 100;
    String tableName = "inserts_while_creating_index";
    String indexName = tableName + "_idx";

    try (Statement stmt = connection.createStatement()) {
      stmt.executeUpdate("CREATE TABLE " + tableName + "(v int)");
    }

    ExecutorService es = Executors.newFixedThreadPool(minThreads);
    List<Future<?>> futures = new ArrayList<>();
    CountDownLatch insertDone = new CountDownLatch(1);
    CountDownLatch backfillThreadStarted = new CountDownLatch(1);
    AtomicInteger numInserts = new AtomicInteger();
    ConnectionBuilder connBldr = getConnectionBuilder();

    futures.add(es.submit(() -> {
      try (Connection conn = connBldr.connect();
          Statement stmt = conn.createStatement()) {
        backfillThreadStarted.countDown();
        insertDone.await(AWAIT_TIMEOUT_SEC, TimeUnit.SECONDS);
        // This will wait for pg_index.indisready=true
        stmt.executeUpdate("CREATE INDEX " + indexName + " ON " + tableName + "(v ASC)");
      } catch (Exception ex) {
        LOG.error("CREATE INDEX thread failed", ex);
        fail("CREATE INDEX thread failed: " + ex.getMessage());
      }
    }));

    futures.add(es.submit(() -> {
      try (Connection conn = connBldr.connect();
          Statement stmt = conn.createStatement()) {
        backfillThreadStarted.await(AWAIT_TIMEOUT_SEC, TimeUnit.SECONDS);
        // Perform a chunk of N inserts until an index is fully constructed
        int v = 0;
        do {
          LOG.info("Inserting a chunk of " + insertsChunkSize + " values");
          for (int i = 0; i < insertsChunkSize; i++) {
            if (Thread.interrupted()) return;
            try {
              stmt.executeUpdate("INSERT INTO " + tableName + " VALUES (" + v + ")");
              ++v;
            } catch (Exception ex) {
              if (!isIgnorableException(ex)) {
                throw ex;
              }
            }
            insertDone.countDown();
          }
        } while (!isIndexValid(conn, indexName));
        numInserts.set(v);
      } catch (Exception ex) {
        LOG.error("Insert thread failed", ex);
        fail("Insert thread failed: " + ex.getMessage());
      } finally {
        insertDone.countDown();
      }
    }));

    try {
      // Wait for inserts/backfill to return
      try {
        LOG.info("Waiting for INSERT and CREATE INDEX threads");
        for (Future<?> future : futures) {
          future.get(AWAIT_TIMEOUT_SEC, TimeUnit.SECONDS);
        }
      } catch (TimeoutException ex) {
        LOG.warn("Threads info:\n\n" + ThreadUtil.getAllThreadsInfo());
        // It's very likely that cause lies on a YB side (e.g. unexpected performance slowdown),
        // not in test.
        fail("Waiting for future threads timed out, this is unexpected!");
      }
    } finally {
      LOG.info("Shutting down executor service");
      es.shutdownNow(); // This should interrupt all submitted threads
      if (es.awaitTermination(10, TimeUnit.SECONDS)) {
        LOG.info("Executor shutdown complete");
      } else {
        LOG.info("Executor shutdown failed (timed out)");
      }
    }

    // Make sure that index contains everything
    String countSql = "SELECT COUNT(*) FROM " + tableName + " WHERE v >= 0";
    try (Statement stmt = connection.createStatement()) {
      assertTrue(isIndexOnlyScan(stmt, countSql, indexName));
      assertQuery(stmt, countSql, new Row(numInserts.get()));
    }
  }

  /**
   * Same as TestPgUniqueConstraint#createIndexViolatingUniqueness except use
   * index backfill.
   */
  @Test
  public void createIndexViolatingUniqueness() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test(id int PRIMARY KEY, v int)");

      // Get the OID of the 'test' table from pg_class
      long tableOid = getRowList(
          stmt.executeQuery("SELECT oid FROM pg_class WHERE relname = 'test'")).get(0).getLong(0);

      // Two entries in pg_depend table, one for pg_type and the other for pg_constraint
      assertQuery(stmt, "SELECT COUNT(*) FROM pg_depend WHERE refobjid=" + tableOid,
          new Row(2));

      stmt.executeUpdate("INSERT INTO test VALUES (1, 1)");
      stmt.executeUpdate("INSERT INTO test VALUES (2, 1)");

      runInvalidQuery(stmt, "CREATE UNIQUE INDEX test_v on test(v)", "duplicate key");

      // Ensure index exists but is invalid
      assertQuery(
          stmt,
          ("SELECT indisvalid FROM pg_class INNER JOIN pg_index ON oid = indexrelid"
           + " WHERE relname = 'test_v'"),
          new Row(false));

      // Clean up invalid index
      stmt.execute("DROP INDEX test_v");

      // Make sure index has no leftovers
      assertNoRows(stmt, "SELECT oid FROM pg_class WHERE relname = 'test_v'");
      assertQuery(stmt, "SELECT COUNT(*) FROM pg_depend WHERE refobjid=" + tableOid,
          new Row(2));
      assertQuery(stmt, "SELECT * FROM test WHERE v = 1",
          new Row(1, 1), new Row(2, 1));

      // We can still insert duplicate elements
      stmt.executeUpdate("INSERT INTO test VALUES (3, 1)");
      assertQuery(stmt, "SELECT * FROM test WHERE v = 1",
          new Row(1, 1), new Row(2, 1), new Row(3, 1));
    }
  }

  /** Inspect {@code pg_index.indisvalid} column for the given index */
  private boolean isIndexValid(Connection conn, String indexName) throws Exception {
    String selectIndexSql = "SELECT i.indisvalid FROM pg_index i"
        + " INNER JOIN pg_class c ON i.indexrelid = c.oid"
        + " WHERE c.relname = ?";
    try (PreparedStatement pstmt = conn.prepareStatement(selectIndexSql)) {
      pstmt.setString(1, indexName);
      List<Row> rows = getRowList(pstmt.executeQuery());
      if (!rows.isEmpty()) {
        return rows.get(0).getBoolean(0);
      } else {
        return false;
      }
    }
  }

  /** Whether we expect this exception to casually happen during a concurrent workflow */
  private boolean isIgnorableException(Exception ex) {
    String msgLc = ex.getMessage().toLowerCase();
    return msgLc.contains("schema version mismatch")
        || msgLc.contains("catalog version mismatch")
        || (msgLc.contains("resource unavailable") && msgLc.contains("rocksdb"));
  }
}
