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

import org.apache.commons.lang3.RandomUtils;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.postgresql.core.TransactionState;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.util.RandomNumberUtil;
import org.yb.util.SanitizerUtil;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Array;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.PreparedStatement;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.List;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Random;

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgTransactions extends BasePgSQLTest {

  private static final Logger LOG = LoggerFactory.getLogger(TestPgTransactions.class);

  private static boolean isYBTransactionError(PSQLException ex) {
    return ex.getSQLState().equals("40001");
  }

  private static boolean isTransactionAbortedError(PSQLException ex) {
    return PSQLState.IN_FAILED_SQL_TRANSACTION.getState().equals(ex.getSQLState());
  }

  private void checkTransactionFairness(
      int numFirstWinners,
      int numSecondWinners,
      int totalIterations) {
    // similar logic to cxx-test: PgLibPqTest.SerializableReadWriteOnConflict
    // break if we hit 25% accuracy
    // Coin Toss Problem: 100 iterations, 25 heads.  False positive probability == 1 in 1.6M
    assertLessThan("First Win Too Low", totalIterations / 4, numFirstWinners);
    assertLessThan("Second Win Too Low", totalIterations / 4, numSecondWinners);
  }

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.enablePgTransactions(true);
  }

  @Test
  public void testTableWithoutPrimaryKey() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE t (thread_id TEXT, attempt_id TEXT, k INT, v INT)");
    }
    final int NUM_THREADS = 4;
    final int NUM_INCREMENTS_PER_THREAD = 100;
    ExecutorService ecs = Executors.newFixedThreadPool(NUM_THREADS);
    final AtomicBoolean hadErrors = new AtomicBoolean();
    for (int i = 1; i <= NUM_THREADS; ++i) {
      final int threadIndex = i;
      ecs.submit(() -> {
        LOG.info("Workload thread " + threadIndex + " starting");
        int numIncrements = 0;
        try (Connection conn = getConnectionBuilder()
                .withIsolationLevel(IsolationLevel.REPEATABLE_READ)
                .withAutoCommit(AutoCommit.ENABLED)
                .connect()) {
          Statement stmt = conn.createStatement();
          int currentValue = 0x01010100 * threadIndex;
          stmt.execute("INSERT INTO t (thread_id, attempt_id, k, v) VALUES (" +
              "'thread_" + threadIndex + "', 'thread_" + threadIndex + "_attempt_0" +
              "', " + threadIndex + ", " + currentValue + ")");
          int attemptIndex = 1;
          while (numIncrements < NUM_INCREMENTS_PER_THREAD && !hadErrors.get()) {
            String attemptId = "thread_" + threadIndex + "_attempt_" + attemptIndex;
            try {
              LOG.info("Thread " + threadIndex + ": trying to update from " +
                       numIncrements + " to " + (numIncrements + 1));
              int rowsUpdated =
                  stmt.executeUpdate("UPDATE t SET v = v + 1, attempt_id = '" + attemptId +
                      "' WHERE k = " + threadIndex);
              assertEquals(1, rowsUpdated);

              numIncrements++;
              currentValue++;
              LOG.info("Thread " + threadIndex + " is verifying the value at attemptIndex=" +
                       attemptIndex + ": attemptId=" + attemptId);

              ResultSet res = stmt.executeQuery(
                  "SELECT attempt_id, v FROM t WHERE k = " + threadIndex);
              LOG.info(
                  "Thread " + threadIndex + " finished reading the result after attemptIndex=" +
                  attemptIndex);

              assertTrue(res.next());
              int value = res.getInt("v");
              if (value != currentValue) {
                assertEquals(
                    "Invalid result in thread " + threadIndex + ", attempt index " + attemptIndex +
                    ", num increments reported as successful: " + numIncrements +
                    ", expected value (hex): " + String.format("0x%08X", currentValue) +
                    ", actual value (hex): " + String.format("0x%08X", value) +
                    ", attempt id: " + attemptId,
                    currentValue, value);
                hadErrors.set(true);
              }
            } catch (PSQLException ex) {
              LOG.warn("Error updating/verifying in thread " + threadIndex);
            }
            attemptIndex++;
          }
        } catch (Exception ex) {
          LOG.error("Exception in thread " + threadIndex, ex);
          hadErrors.set(true);
        } finally {
          LOG.info("Workload thread " + threadIndex + " exiting, numIncrements=" + numIncrements);
        }
      });
    }
    ecs.shutdown();
    ecs.awaitTermination(30, TimeUnit.SECONDS);
    try (Statement statement = connection.createStatement()) {
      ResultSet rsAll = statement.executeQuery("SELECT k, v FROM t");
      while (rsAll.next()) {
        LOG.info("Row found at the end: k=" + rsAll.getInt("k") + ", v=" + rsAll.getInt("v"));
      }

      for (int i = 1; i <= NUM_THREADS; ++i) {
        ResultSet rs = statement.executeQuery(
            "SELECT v FROM t WHERE k = " + i);
        assertTrue("Did not find any values with k=" + i, rs.next());
        int v = rs.getInt("v");
        LOG.info("Value for k=" + i + ": " + v);
        assertEquals(0x01010100 * i + NUM_INCREMENTS_PER_THREAD, v);
      }
      assertFalse("Test had errors, look for 'Exception in thread' above", hadErrors.get());
    }
  }

  @Test
  public void testSnapshotReadDelayWrite() throws Exception {
    runReadDelayWriteTest(IsolationLevel.REPEATABLE_READ);
  }

  @Test
  public void testSerializableReadDelayWrite() throws Exception {
    runReadDelayWriteTest(IsolationLevel.SERIALIZABLE);
  }

  private void runReadDelayWriteTest(final IsolationLevel isolationLevel) throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE counters (k INT PRIMARY KEY, v INT)");
    }

    final int INCREMENTS_PER_THREAD = 250;
    final int NUM_COUNTERS = 2;
    final int numTServers = miniCluster.getNumTServers();
    final int numThreads = 4;

    // Initialize counters.
    {
      PreparedStatement insertStmt = connection.prepareStatement(
          "INSERT INTO counters (k, v) VALUES (?, ?)");
      for (int i = 0; i < NUM_COUNTERS; ++i) {
        insertStmt.setInt(1, i);
        insertStmt.setInt(2, 0);
        insertStmt.executeUpdate();
      }
    }

    List<Thread> threads = new ArrayList<Thread>();
    AtomicBoolean hadErrors = new AtomicBoolean(false);

    final Object lastValueLock = new Object();
    final int[] numIncrementsByCounter = new int[NUM_COUNTERS];

    for (int i = 1; i <= numThreads; ++i) {
      final int threadIndex = i;
      threads.add(new Thread(() -> {
        LOG.info("Workload thread " + threadIndex + " is starting");
        Random rand = new Random(System.currentTimeMillis() * 137 + threadIndex);
        int numIncrementsDone = 0;
        try (Connection conn = getConnectionBuilder().withTServer(threadIndex % numTServers)
                                                     .withIsolationLevel(isolationLevel)
                                                     .withAutoCommit(AutoCommit.DISABLED)
                                                     .connect()) {
          PreparedStatement selectStmt = conn.prepareStatement(
              "SELECT v FROM counters WHERE k = ?");
          PreparedStatement updateStmt = conn.prepareStatement(
              "UPDATE counters SET v = ? WHERE k = ?");
          long attemptId =
              1000 * 1000 * 1000L * threadIndex +
              1000 * 1000L * Math.abs(RandomNumberUtil.getRandomGenerator().nextInt(1000));
          while (numIncrementsDone < INCREMENTS_PER_THREAD && !hadErrors.get()) {
            ++attemptId;
            boolean committed = false;
            try {
              int counterIndex = rand.nextInt(NUM_COUNTERS);

              selectStmt.setInt(1, counterIndex);

              // The value of the counter that we'll read should be
              int initialValueLowerBound;
              synchronized (lastValueLock) {
                initialValueLowerBound = numIncrementsByCounter[counterIndex];
              }

              ResultSet rs = selectStmt.executeQuery();
              assertTrue(rs.next());
              int currentValue = rs.getInt(1);
              int delayMs = 2 + rand.nextInt(15);
              LOG.info(
                  "Thread " + threadIndex + " read counter " + counterIndex + ", got value " +
                  currentValue +
                  (currentValue == initialValueLowerBound
                      ? " as expected"
                      : " and expected to get at least " + initialValueLowerBound) +
                  ", will sleep for " + delayMs + " ms. attemptId=" + attemptId);

              Thread.sleep(delayMs);
              LOG.info("Thread " + threadIndex + " finished sleeping for " + delayMs + " ms" +
                       ", attemptId=" + attemptId);
              if (hadErrors.get()) {
                LOG.info("Thread " + threadIndex + " is exiting in the middle of an iteration " +
                         "because errors happened in another thread.");
                break;
              }

              int updatedValue = currentValue + 1;
              updateStmt.setInt(1, updatedValue);
              updateStmt.setInt(2, counterIndex);
              assertEquals(1, updateStmt.executeUpdate());
              LOG.info("Thread " + threadIndex + " successfully updated value of counter " +
                       counterIndex + " to " + updatedValue +", attemptId=" + attemptId);

              conn.commit();
              committed = true;
              LOG.info(
                  "Thread " + threadIndex + " successfully committed value " + updatedValue +
                  " to counter " + counterIndex + ". attemptId=" + attemptId);

              synchronized (lastValueLock) {
                numIncrementsByCounter[counterIndex]++;
              }
              numIncrementsDone++;

              if (currentValue < initialValueLowerBound) {
                assertTrue(
                    "IMPORTANT ERROR. In thread " + threadIndex + ": " +
                    "expected counter " + counterIndex + " to be at least at " +
                        initialValueLowerBound + " at the beginning of a successful increment, " +
                        "but got " + currentValue + ". attemptId=" + attemptId,
                    false);
                hadErrors.set(true);
              }
            } catch (PSQLException ex) {
              if (!isYBTransactionError(ex) && !isTransactionAbortedError(ex)) {
                throw ex;
              }
              LOG.info(
                  "Got an exception in thread " + threadIndex + ", attemptId=" + attemptId, ex);
            } finally {
              if (!committed) {
                LOG.info("Rolling back the transaction on thread " + threadIndex +
                         ", attemptId=" + attemptId);
                conn.rollback();
              }
            }
          }
        } catch (Exception ex) {
          LOG.error("Unhandled exception in thread " + threadIndex, ex);
          hadErrors.set(true);
        } finally {
          LOG.info("Workload thread " + threadIndex +
                   " has finished, numIncrementsDone=" + numIncrementsDone);
        }
      }));
    }

    for (Thread thread : threads) {
      thread.start();
    }

    for (Thread thread : threads) {
      thread.join();
    }

    Statement checkStmt = connection.createStatement();
    ResultSet finalResult = checkStmt.executeQuery("SELECT k, v FROM counters");
    int total = 0;
    while (finalResult.next()) {
      int k = finalResult.getInt("k");
      int v = finalResult.getInt("v");
      LOG.info("Final row: k=" + k + " v=" + v);
      total += v;
    }

    int expectedResult = 0;
    for (int i = 0; i < NUM_COUNTERS; ++i) {
      expectedResult += numIncrementsByCounter[i];
    }
    assertEquals(expectedResult, total);

    assertFalse("Had errors", hadErrors.get());
  }

  @Test
  public void testSerializableWholeHashVsScanConflict() throws Exception {
    createSimpleTable("test", "v", PartitioningMode.HASH);
    final IsolationLevel isolation = IsolationLevel.SERIALIZABLE;
    try (
        Connection connection1 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement statement1 = connection1.createStatement();

        Connection connection2 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.ENABLED)
            .connect();
        Statement statement2 = connection2.createStatement()) {

      int numSuccess1 = 0;
      int numSuccess2 = 0;
      final int TOTAL_ITERATIONS = 100;
      for (int i = 0; i < TOTAL_ITERATIONS; ++i) {
        int h1 = i;
        LOG.debug("Inserting the first row within a transaction but not committing yet");
        statement1.execute("INSERT INTO test(h, r, v) VALUES (" + h1 + ", 2, 3)");

        LOG.debug("Trying to read the first row from another connection");

        PSQLException ex2 = null;
        try {
          assertFalse(statement2.executeQuery("SELECT h, r, v FROM test WHERE h = " + h1).next());
        } catch (PSQLException ex) {
          ex2 = ex;
        }

        PSQLException ex1 = null;
        try {
          connection1.commit();
        } catch (PSQLException ex) {
          ex1 = ex;
        }

        final boolean succeeded1 = ex1 == null;
        final boolean succeeded2 = ex2 == null;
        assertNotEquals("Expecting exactly one transaction to succeed", succeeded1, succeeded2);
        LOG.info("ex1=" + ex1);
        LOG.info("ex2=" + ex2);
        if (ex1 != null) {
          assertTrue(isYBTransactionError(ex1));
        }
        if (ex2 != null) {
          assertTrue(isYBTransactionError(ex2));
        }
        if (succeeded1) {
          numSuccess1++;
        }
        if (succeeded2) {
          numSuccess2++;
        }
      }
      LOG.info("INSERT succeeded " + numSuccess1 + " times, " +
               "SELECT succeeded " + numSuccess2 + " times");
      checkTransactionFairness(numSuccess1, numSuccess2, TOTAL_ITERATIONS);
    }
  }

  @Test
  public void testBasicTransaction() throws Exception {
    createSimpleTable("test", "v", PartitioningMode.HASH);
    final IsolationLevel isolation = IsolationLevel.REPEATABLE_READ;
    try (
        Connection connection1 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement statement = connection1.createStatement();

        // For the second connection we still enable auto-commit, so that every new SELECT will see
        // a new snapshot of the database.
        Connection connection2 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.ENABLED)
            .connect();
        Statement statement2 = connection2.createStatement()) {

      for (int i = 0; i < 100; ++i) {
        try {
          int h1 = i * 10;
          int h2 = i * 10 + 1;
          LOG.debug("Inserting the first row within a transaction but not committing yet");
          statement.execute("INSERT INTO test(h, r, v) VALUES (" + h1 + ", 2, 3)");

          LOG.debug("Trying to read the first row from another connection");
          assertFalse(statement2.executeQuery("SELECT h, r, v FROM test WHERE h = " + h1).next());

          LOG.debug("Inserting the second row within a transaction but not committing yet");
          statement.execute("INSERT INTO test(h, r, v) VALUES (" + h2 + ", 5, 6)");

          LOG.debug("Trying to read the second row from another connection");
          assertFalse(statement2.executeQuery("SELECT h, r, v FROM test WHERE h = " + h2).next());

          LOG.debug("Committing the transaction");
          connection1.commit();

          LOG.debug("Checking first row from the other connection");
          ResultSet rs = statement2.executeQuery("SELECT h, r, v FROM test WHERE h = " + h1);
          assertTrue(rs.next());
          assertEquals(h1, rs.getInt("h"));
          assertEquals(2, rs.getInt("r"));
          assertEquals(3, rs.getInt("v"));

          LOG.debug("Checking second row from the other connection");
          rs = statement2.executeQuery("SELECT h, r, v FROM test WHERE h = " + h2);
          assertTrue(rs.next());
          assertEquals(h2, rs.getInt("h"));
          assertEquals(5, rs.getInt("r"));
          assertEquals(6, rs.getInt("v"));
        } catch (PSQLException ex) {
          LOG.error("Caught a PSQLException at iteration i=" + i, ex);
          throw ex;
        }
      }
    }
  }

  /**
   * This test runs conflicting transactions trying to insert the same row and verifies that
   * exactly one of them gets committed.
   */
  @Test
  public void testTransactionConflicts() throws Exception {
    createSimpleTable("test", "v");
    final IsolationLevel isolation = IsolationLevel.REPEATABLE_READ;

    try (
        Connection srcConnection1 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement srcStatement1 = srcConnection1.createStatement();

        Connection srcConnection2 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement srcStatement2 = srcConnection2.createStatement()) {
      // Declare and use local variables instead (so that they could be swapped),
      // and keep the original references for an automatic cleanup.
      Connection conn1 = srcConnection1;
      Connection conn2 = srcConnection2;
      Statement stmt1 = srcStatement1;
      Statement stmt2 = srcStatement2;

      int numFirstWinners = 0;
      int numSecondWinners = 0;
      final int totalIterations = SanitizerUtil.nonTsanVsTsan(300, 100);
      for (int i = 1; i <= totalIterations; ++i) {
        LOG.info("Starting iteration: i=" + i);
        if (RandomUtils.nextBoolean()) {
          // Shuffle the two connections between iterations.
          Connection tmpConn = conn1;
          conn1 = conn2;
          conn2 = tmpConn;

          Statement tmpStmt = stmt1;
          stmt1 = stmt2;
          stmt2 = tmpStmt;
        }

        executeWithTimeout(stmt1,
            String.format("INSERT INTO test(h, r, v) VALUES (%d, %d, %d)", i, i, 100 * i));
        boolean executed2 = false;
        try {
          executeWithTimeout(stmt2,
              String.format("INSERT INTO test(h, r, v) VALUES (%d, %d, %d)", i, i, 200 * i));
          executed2 = true;
        } catch (PSQLException ex) {
          // Not reporting a stack trace here on purpose, because this will happen a lot in a test.
          // [#1289] Don't think this should ever be a isTransactionAbortedError
          assertTrue(ex.getMessage(), isYBTransactionError(ex));
        }
        TransactionState txnState1BeforeCommit = getPgTxnState(conn1);
        TransactionState txnState2BeforeCommit = getPgTxnState(conn2);

        boolean committed1 = commitAndCatchException(conn1, "first connection");
        TransactionState txnState1AfterCommit = getPgTxnState(conn1);

        boolean committed2 = commitAndCatchException(conn2, "second connection");
        TransactionState txnState2AfterCommit = getPgTxnState(conn2);

        LOG.info("i=" + i +
            " executed2=" + executed2 +
            " committed1=" + committed1 +
            " committed2=" + committed2 +
            " txnState1BeforeCommit=" + txnState1BeforeCommit +
            " txnState2BeforeCommit=" + txnState2BeforeCommit +
            " txnState1AfterCommit=" + txnState1AfterCommit +
            " txnState2AfterCommit=" + txnState2AfterCommit +
            " numFirstWinners=" + numFirstWinners +
            " numSecondWinners=" + numSecondWinners);

        // Whether or not a transaction commits successfully, its state is changed to IDLE after the
        // commit attempt.
        assertEquals(TransactionState.IDLE, txnState1AfterCommit);
        assertEquals(TransactionState.IDLE, txnState2AfterCommit);

        if (!committed1 && !committed2) {
          // TODO: if this happens, look at why two transactions could fail at the same time.
          throw new AssertionError("Did not expect both transactions to fail!");
        }

        if (executed2) {
          assertFalse(committed1);
          assertTrue(committed2);
          assertEquals(TransactionState.OPEN, txnState1BeforeCommit);
          assertEquals(TransactionState.OPEN, txnState2BeforeCommit);
          numSecondWinners++;
        } else {
          assertTrue(committed1);
          // It looks like in case we get an error on an operation on the second connection, the
          // commit on that connection succeeds. This makes sense in a way since the client already
          // knows that the second transaction failed from the original operation failure. BTW the
          // second transaction is already in a FAILED state before we successfully "commit" it:
          //
          // executed2=false
          // committed1=true
          // committed2=true
          // txnState1BeforeCommit=OPEN
          // txnState2BeforeCommit=FAILED
          // txnState1AfterCommit=IDLE
          // txnState2AfterCommit=IDLE
          //
          // TODO: verify if this is consistent with vanilla PostgreSQL behavior.
          // assertFalse(committed2);
          assertEquals(TransactionState.OPEN, txnState1BeforeCommit);
          assertEquals(TransactionState.FAILED, txnState2BeforeCommit);

          numFirstWinners++;
        }
      }
      LOG.info(String.format(
          "First txn won in %d cases, second won in %d cases", numFirstWinners, numSecondWinners));
      checkTransactionFairness(numFirstWinners, numSecondWinners, totalIterations);
    }
  }

  @Test
  public void testFailedTransactions() throws Exception {
    createSimpleTable("test", "v");
    final IsolationLevel isolation = IsolationLevel.REPEATABLE_READ;
    try (
        Connection connection1 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.ENABLED)
            .connect();
        Statement statement1 = connection1.createStatement();

        Connection connection2 = getConnectionBuilder()
            .withIsolationLevel(isolation)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement statement2 = connection2.createStatement();

        Statement statementQ = connection.createStatement()) {

      Set<Row> expectedRows = new HashSet<>();
      String scanQuery = "SELECT * FROM test";

      // Check second-op failure with auto-commit (first op should get committed).
      statement1.execute("INSERT INTO test(h, r, v) VALUES (1, 1, 1)");
      expectedRows.add(new Row(1, 1, 1));
      runInvalidQuery(statement1,
                      "INSERT INTO test(h, r, v) VALUES (1, 1, 2)",
                      "Duplicate key");
      assertRowSet(statementQ, scanQuery, expectedRows);

      // Check second op failure with no-auto-commit (both ops should get aborted).
      statement2.execute("INSERT INTO test(h, r, v) VALUES (1, 2, 1)");
      runInvalidQuery(statement2,
                      "INSERT INTO test(h, r, v) VALUES (1, 1, 2)",
                      "Duplicate key");
      connection2.commit(); // Overkill, transaction should already be aborted, this will be noop.
      assertRowSet(statementQ, scanQuery, expectedRows);

      // Check failure for one value set -- primary key (1,1) already exists.
      runInvalidQuery(statement1,
                      "INSERT INTO test(h, r, v) VALUES (1, 2, 1), (1, 1, 2)",
                      "Duplicate key");
      // Entire query should get aborted.
      assertRowSet(statementQ, scanQuery, expectedRows);

      // Check failure for query with WITH clause side-effect -- primary key (1,1) already exists.
      String query = "WITH ret AS (INSERT INTO test(h, r, v) VALUES (2, 2, 2) RETURNING h, r, v) " +
          "INSERT INTO test(h,r,v) SELECT h - 1, r - 1, v FROM ret";
      runInvalidQuery(statement1, query, "Duplicate key");
      // Entire query should get aborted (including the WITH clause INSERT).
      assertRowSet(statementQ, scanQuery, expectedRows);

      // Check failure within function with side-effect -- primary key (1,1) already exists.
      statement1.execute("CREATE FUNCTION bar(in int) RETURNS int AS $$ " +
          "INSERT INTO test(h,r,v) VALUES($1,$1,$1) RETURNING h - 1;$$" +
          "LANGUAGE SQL;");
      runInvalidQuery(statement1,
                      "INSERT INTO test(h, r, v) VALUES (bar(2), 1, 1)",
                      "Duplicate key");
      // Entire query should get aborted (including the function's side-effect).
      assertRowSet(statementQ, scanQuery, expectedRows);
    }

  }

  private void testSingleRowTransactionGuards(List<String> stmts, List<String> guard_start_stmts,
                                              List<String> guard_end_stmts) throws Exception {
    Statement statement = connection.createStatement();

    // With guard (e.g. BEGIN/END, secondary index, trigger, etc.), statements should use txn path.
    for (String guard_start_stmt : guard_start_stmts) {
      statement.execute(guard_start_stmt);
    }
    for (String stmt : stmts) {
      verifyStatementTxnMetric(statement, stmt, 1);
    }

    // After ending guard, statements should go back to using non-txn path.
    for (String guard_end_stmt : guard_end_stmts) {
      statement.execute(guard_end_stmt);
    }
    for (String stmt : stmts) {
      verifyStatementTxnMetric(statement, stmt, 0);
    }
  }

  private void testSingleRowTransactionGuards(List<String> stmts, String guard_start_stmt,
                                              String guard_end_stmt) throws Exception {
    testSingleRowTransactionGuards(stmts,
                                   Arrays.asList(guard_start_stmt),
                                   Arrays.asList(guard_end_stmt));
  }

  private void testSingleRowStatements(List<String> stmts) throws Exception {
    // Verify standalone statements use non-txn path.
    Statement statement = connection.createStatement();
    for (String stmt : stmts) {
      verifyStatementTxnMetric(statement, stmt, 0);
    }

    // Test in txn block.
    testSingleRowTransactionGuards(
        stmts,
        "BEGIN",
        "END");

    // Test with secondary index.
    testSingleRowTransactionGuards(
        stmts,
        "CREATE INDEX test_index ON test (v)",
        "DROP INDEX test_index");

    // Test with trigger.
    testSingleRowTransactionGuards(
        stmts,
        "CREATE TRIGGER test_trigger BEFORE UPDATE ON test " +
        "FOR EACH ROW EXECUTE PROCEDURE suppress_redundant_updates_trigger()",
        "DROP TRIGGER test_trigger ON test");

    // Test with foreign key.
    testSingleRowTransactionGuards(
        stmts,
        Arrays.asList(
            "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE",
            "DROP TABLE IF EXISTS foreign_table",
            "CREATE TABLE foreign_table (v int PRIMARY KEY)",
            "INSERT INTO foreign_table VALUES (1), (2)",
            "DROP TABLE IF EXISTS test",
            "CREATE TABLE test (k int PRIMARY KEY, v int references foreign_table(v))"),
        Arrays.asList(
            "DROP TABLE test",
            "DROP TABLE foreign_table",
            "CREATE TABLE test (k int PRIMARY KEY, v int)",
            "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
  }

  @Test
  public void testSingleRowNoTransaction() throws Exception {
    Statement statement = connection.createStatement();
    statement.execute("CREATE TABLE test (k int PRIMARY KEY, v int)");

    // Test regular INSERT/UPDATE/DELETE single-row statements.
    testSingleRowStatements(
        Arrays.asList(
            "INSERT INTO test VALUES (1, 1)",
            "UPDATE test SET v = 2 WHERE k = 1",
            "DELETE FROM test WHERE k = 1"));

    // Test INSERT/UPDATE/DELETE single-row prepared statements.
    statement.execute("PREPARE insert_stmt (int, int) AS INSERT INTO test VALUES ($1, $2)");
    statement.execute("PREPARE delete_stmt (int) AS DELETE FROM test WHERE k = $1");
    statement.execute("PREPARE update_stmt (int, int) AS UPDATE test SET v = $2 WHERE k = $1");
    testSingleRowStatements(
        Arrays.asList(
            "EXECUTE insert_stmt (1, 1)",
            "EXECUTE update_stmt (1, 2)",
            "EXECUTE delete_stmt (1)"));

    // Verify statements with WITH clause use txn path.
    verifyStatementTxnMetric(statement,
                             "WITH test2 AS (UPDATE test SET v = 2 WHERE k = 1) " +
                             "UPDATE test SET v = 3 WHERE k = 1", 1);

    // Verify JDBC single-row prepared statements use non-txn path.
    long oldTxnValue = getMetricCounter(TRANSACTIONS_METRIC);

    PreparedStatement insertStatement =
      connection.prepareStatement("INSERT INTO test VALUES (?, ?)");
    insertStatement.setInt(1, 1);
    insertStatement.setInt(2, 1);
    insertStatement.executeUpdate();

    PreparedStatement deleteStatement =
      connection.prepareStatement("DELETE FROM test WHERE k = ?");
    deleteStatement.setInt(1, 1);
    deleteStatement.executeUpdate();

    PreparedStatement updateStatement =
      connection.prepareStatement("UPDATE test SET v = ? WHERE k = ?");
    updateStatement.setInt(1, 1);
    updateStatement.setInt(2, 1);
    updateStatement.executeUpdate();

    long newTxnValue = getMetricCounter(TRANSACTIONS_METRIC);
    assertEquals(oldTxnValue, newTxnValue);
  }

  /*
   * Execute a query and check that either is succeeds or it fails with a transaction (conflict)
   * error. Returns whether the query succeeded.
   */
  private boolean checkTxnExecute(Statement statement, String query) throws SQLException {
    try {
      statement.execute(query);
      return true;
    } catch (PSQLException e) {
      assertTrue(e.getMessage(), isYBTransactionError(e));
    }
    return false;
  }

  /*
   * Run two queries in two different connections for the number of requested iterations and in
   * varying order of operations. Check that exactly one succeeds each iteration.
   * Returns the number of successes for query1 (implying the rest are the query2 successes due to
   * the "exactly one succeeds" check above.
   */
  public int checkConflictingStatements(Statement statement1,
                                        String query1,
                                        Statement statement2,
                                        String query2,
                                        int totalIterations) throws SQLException {
    int txn1Successes = 0;

    for (int i = 0; i < totalIterations; i++) {
      // Vary the execution order throughout the runs.

      // Expect begin transaction to always succeed.
      if (i % 2 == 0) {
        statement1.execute("BEGIN");
        statement2.execute("BEGIN");
      } else {
        statement2.execute("BEGIN");
        statement1.execute("BEGIN");
      }

      boolean txn1_success;
      boolean txn2_success;

      // Run the queries.
      if (i % 4 < 2) { // i % 4 = 0,1
        txn1_success = checkTxnExecute(statement1, query1);
        txn2_success = checkTxnExecute(statement2, query2);
      } else { // i % 4 = 2,3
        txn2_success = checkTxnExecute(statement2, query2);
        txn1_success = checkTxnExecute(statement1, query1);
      }

      // End the transaction(s).
      if (i % 8 < 4) { // i % 8 = 0,1,2,3
        txn1_success = checkTxnExecute(statement1, "END") && txn1_success;
        txn2_success = checkTxnExecute(statement2, "END") && txn2_success;
      } else { // i % 8 = 4,5,6,7
        txn2_success = checkTxnExecute(statement2, "END") && txn2_success;
        txn1_success = checkTxnExecute(statement1, "END") && txn1_success;
      }

      // Check that exactly one txn succeeded.
      assertTrue(txn1_success ^ txn2_success);
      if (txn1_success) {
        txn1Successes += 1;
      }
    }

    return txn1Successes;
  }

  @Test
  public void testExplicitLocking() throws Exception {
    Statement statement = connection.createStatement();

    // Set up a simple key-value table.
    statement.execute("CREATE TABLE test (k int PRIMARY KEY, v int)");
    statement.execute("INSERT INTO test VALUES (1,1), (2,2), (3,3)");

    // Set up a key-value table with an index on the value.
    statement.execute("CREATE TABLE idx_test (k int PRIMARY KEY, v int)");
    statement.execute("CREATE INDEX idx_test_idx on idx_test(v)");
    statement.execute("INSERT INTO idx_test VALUES (1,1), (2,2), (3,3)");

    // Using a smaller number of iterations when expecting one txn to always win (i.e. explicit
    // locking vs regular transaction) and a larger number when expecting a coin-toss
    // (i.e. explicit locking vs explicit locking) to more accurately test for a fair
    // distribution in the latter case.
    int numItersSmall = 32;
    int numItersLarge = 120;

    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Connection connection2 = getConnectionBuilder().withTServer(1).connect();
         Statement statement1 = connection1.createStatement();
         Statement statement2 = connection2.createStatement()) {

      //--------------------------------------------------------------------------------------------
      // Test explicit locking on key columns.

      // Check that explicit locking always wins against regular transactions.
      String selectStmt = "SELECT * FROM test WHERE k = 1 %s";
      int txn1_successes = checkConflictingStatements(statement1,
                                                      String.format(selectStmt, "FOR UPDATE"),
                                                      statement2,
                                                      "UPDATE test SET v = 10 WHERE k = 1",
                                                      numItersSmall);
      assertEquals(numItersSmall, txn1_successes);

      txn1_successes = checkConflictingStatements(statement1,
                                                  String.format(selectStmt, "FOR NO KEY UPDATE"),
                                                  statement2,
                                                  "UPDATE test SET v = 10 WHERE k = 1",
                                                  numItersSmall);
      assertEquals(numItersSmall, txn1_successes);

      txn1_successes = checkConflictingStatements(statement1,
                                                  String.format(selectStmt, "FOR SHARE"),
                                                  statement2,
                                                  "UPDATE test SET v = 10 WHERE k = 1",
                                                  numItersSmall);
      assertEquals(numItersSmall, txn1_successes);

      // Check that for two explicit-locking statements exactly one succeeds.
      txn1_successes = checkConflictingStatements(statement1,
                                                  String.format(selectStmt, "FOR UPDATE"),
                                                  statement2,
                                                  String.format(selectStmt, "FOR NO KEY UPDATE"),
                                                  numItersLarge);
      checkTransactionFairness(txn1_successes, numItersLarge - txn1_successes, numItersLarge);

      //--------------------------------------------------------------------------------------------
      // Test explicit locking on value columns (without index).

      // Check that explicit locking always wins against regular transactions.
      txn1_successes = checkConflictingStatements(statement1,
                                                  "SELECT * FROM test WHERE v = 1 FOR UPDATE",
                                                  statement2,
                                                  "UPDATE test SET v = 10 WHERE k = 2",
                                                  numItersSmall);
      assertEquals(numItersSmall, txn1_successes);

      // Check that for two explicit-locking statements exactly one succeeds.
      txn1_successes = checkConflictingStatements(statement1,
                                                  "SELECT * FROM test WHERE v = 1 FOR UPDATE",
                                                  statement2,
                                                  "SELECT * FROM test WHERE v = 1 FOR SHARE",
                                                  numItersLarge);
      checkTransactionFairness(txn1_successes, numItersLarge - txn1_successes, numItersLarge);

      //--------------------------------------------------------------------------------------------
      // Test explicit locking on index columns.

      // Check that explicit locking always wins against regular transactions.
      // Update with condition on key column.
      txn1_successes = checkConflictingStatements(statement1,
                                                  "SELECT * FROM idx_test WHERE v = 1 FOR UPDATE",
                                                  statement2,
                                                  "UPDATE idx_test SET v = 10 WHERE k = 1",
                                                  numItersSmall);
      assertEquals(numItersSmall, txn1_successes);

      // Update with condition on value column.
      txn1_successes = checkConflictingStatements(statement1,
                                                  "SELECT * FROM idx_test WHERE v = 1 FOR UPDATE",
                                                  statement2,
                                                  "UPDATE idx_test SET v = 10 WHERE v = 1",
                                                  numItersSmall);
      assertEquals(numItersSmall, txn1_successes);

      // Check that for two explicit-locking statements exactly one succeeds.
      txn1_successes = checkConflictingStatements(statement1,
                                                  "SELECT * FROM idx_test WHERE v = 1 FOR UPDATE",
                                                  statement2,
                                                  "SELECT * FROM idx_test WHERE v = 1 FOR SHARE",
                                                  numItersLarge);
      checkTransactionFairness(txn1_successes, numItersLarge - txn1_successes, numItersLarge);
    }
  }
}
