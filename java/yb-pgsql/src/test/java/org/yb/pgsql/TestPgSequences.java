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

import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.*;
import java.util.*;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgSequences extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSequences.class);

  private static final int TURN_OFF_SEQUENCE_CACHE_FLAG = 0;

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("ysql_sequence_cache_minval", Integer.toString(TURN_OFF_SEQUENCE_CACHE_FLAG));
    return flagMap;
  }

  @After
  public void deleteSequences() throws Exception {
    if (connection == null) {
      LOG.warn("No connection created, skipping dropping sequences");
      return;
    }
    try (Statement statement = connection.createStatement()) {
      statement.execute("DROP SEQUENCE s1 CASCADE");
      statement.execute("DROP SEQUENCE s2 CASCADE");
    } catch (Exception e) {
      LOG.info("Exception while dropping sequences s1 and s2", e);
    }
  }

  @Test
  public void testSequencesSimple() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getInt("nextval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(2, rs.getInt("nextval"));
    }
  }

  @Test
  public void testCreateIfNotExistsSequence() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getInt("nextval"));

      statement.execute("CREATE SEQUENCE IF NOT EXISTS s1");
      rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(2, rs.getInt("nextval"));

      statement.execute("CREATE SEQUENCE IF NOT EXISTS s2 START 100");
      rs = statement.executeQuery("SELECT nextval('s2')");
      assertTrue(rs.next());
      assertEquals(100, rs.getInt("nextval"));
    }
  }

  @Test
  public void testSequencesWithCache() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CACHE 100");
      // Use only half of the cached values.
      for (int i = 1; i <= 50; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      // Because values are allocated in blocks of 100 numbers, the next value should be 101.
      assertEquals(101, rs.getInt("nextval"));
    }
  }

  @Test
  public void testSequencesWithCacheAndIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CACHE 50 INCREMENT 3");
      for (int i = 1; i <= 21; i+=3) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      // The previous session should have allocated 50 values: 1, 4, 7, 10 ... 145, 148. So the next
      // value should be 151.
      assertEquals(151, rs.getInt("nextval"));
    }
  }

  @Test
  public void testSequencesWithMaxValue() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 MAXVALUE 5");
      for (int i = 1; i <= 5; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached maximum value of sequence \"s1\" (5)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testSequenceWithMinValue() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 MINVALUE 5");
      ResultSet rs = statement.executeQuery("SELECT NEXTVAL('s1')");
      assertTrue(rs.next());
      assertEquals(5, rs.getInt("nextval"));
    }
  }

  @Test
  public void testCreateInvalidSequenceWithMinValueAndNegativeIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("MINVALUE (10) must be less than MAXVALUE (-1)");
      statement.execute("CREATE SEQUENCE s1 MINVALUE 10 INCREMENT -1");
    }
  }

  @Test
  public void testSequenceWithMinValueAndMaxValueAndNegativeIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 MINVALUE 100 MAXVALUE 105 INCREMENT -1");
      for (int i = 105; i >= 100; i--) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached minimum value of sequence \"s1\" (100)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testSequenceWithMaxValueAndCache() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 MAXVALUE 5 CACHE 10");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getInt("nextval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      // Since the previous client already got all the available sequence numbers in its cache,
      // we should get an error when we request another sequence number from another client.
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached maximum value of sequence \"s1\" (5)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  //------------------------------------------------------------------------------------------------
  // Drop tests.
  //------------------------------------------------------------------------------------------------
  @Test
  public void testDropSequence() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      statement.execute("DROP SEQUENCE s1");

      // Verify that the sequence was deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT relname FROM pg_class WHERE relkind = 'S'");
      assertFalse(rs.next());
    }
  }

  @Test
  public void testDropIfExistsSequence() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      statement.execute("DROP SEQUENCE IF EXISTS s1");

      // Verify that the sequence was deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT c.relname FROM pg_class c WHERE c.relkind = 'S'");
      assertFalse(rs.next());
    }
  }

  @Test
  public void testDropIfExistsSequenceForNonExistingSequence() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("DROP SEQUENCE IF EXISTS s1");
    }
  }

  @Test
  public void testDropWithDependingObjects() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE table t(k SERIAL)");

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("cannot drop sequence t_k_seq because other objects depend on it");
      statement.execute("DROP SEQUENCE t_k_seq");

      // Verify that the sequence was not deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT relname FROM pg_class WHERE relkind = 'S' AND relname = 't_k_seq");
      assertTrue(rs.next());
    }
  }

  @Test
  public void testDropRestrictedWithDependingObjects() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE table t(k SERIAL)");

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("cannot drop sequence t_k_seq because other objects depend on it");
      statement.execute("DROP SEQUENCE t_k_seq RESTRICT");

      // Verify that the sequence was not deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT relname FROM pg_class WHERE relkind = 'S' AND relname = 't_k_seq");
      assertTrue(rs.next());
    }
  }

  @Test
  public void testDropCascadeWithDependingObjects() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE table t(k SERIAL)");
      statement.execute("DROP SEQUENCE t_k_seq CASCADE");

      // Verify that the sequence was deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT relname FROM pg_class WHERE relkind = 'S'");
      assertFalse(rs.next());
    }
  }

  @Test
  public void testOwnedBy() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE t(k int)");
      statement.execute("CREATE SEQUENCE s OWNED BY t.k");
      statement.execute("DROP TABLE t");

      // Verify that the sequence was deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT relname FROM pg_class WHERE relkind = 'S'");
      assertFalse(rs.next());
    }
  }

  @Test
  public void testInt64LimitsInSequences() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute(
          "CREATE SEQUENCE s1 START -9223372036854775808 MINVALUE -9223372036854775808");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(-9223372036854775808L, rs.getLong("nextval"));

      statement.execute("CREATE SEQUENCE s2 START 9223372036854775807");
      rs = statement.executeQuery("SELECT nextval('s2')");
      assertTrue(rs.next());
      assertEquals(9223372036854775807L, rs.getLong("nextval"));
    }
  }

  @Test
  public void testMaxInt64FailureInSequence() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 START 9223372036854775807");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(9223372036854775807L, rs.getLong("nextval"));

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached maximum value of sequence \"s1\" (9223372036854775807)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testMaxInt64FailureInSequenceInDifferentSession() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 START 9223372036854775807");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(9223372036854775807L, rs.getLong("nextval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      // Since the previous client already got all the available sequence numbers in its cache,
      // we should get an error when we request another sequence number from another client.
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached maximum value of sequence \"s1\" (9223372036854775807)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testMaxInt64OverflowFailureInSequenceInDifferentSession() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 START 9223372036854775806 CACHE 100");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(9223372036854775806L, rs.getLong("nextval"));

      rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(9223372036854775807L, rs.getLong("nextval"));

      boolean exceptionOcurred = false;
      try {
        rs = statement.executeQuery("SELECT nextval('s1')");
      } catch (org.postgresql.util.PSQLException e) {
        assertTrue(e.getMessage().contains(
            "reached maximum value of sequence \"s1\" (9223372036854775807)"));
        exceptionOcurred = true;
      }
      assertTrue(exceptionOcurred);
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      // Since the previous client already got all the available sequence numbers in its cache,
      // we should get an error when we request another sequence number from another client.
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached maximum value of sequence \"s1\" (9223372036854775807)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testMinInt64FailureInSequence() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 START -9223372036854775808 " +
          "minvalue -9223372036854775808 INCREMENT -1");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(-9223372036854775808L, rs.getLong("nextval"));

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached minimum value of sequence \"s1\" (-9223372036854775808)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testMinInt64FailureInSequenceInDifferentSession() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 START -9223372036854775808 " +
          "minvalue -9223372036854775808 INCREMENT -1");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(-9223372036854775808L, rs.getLong("nextval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      // Since the previous client already got all the available sequence numbers in its cache,
      // we should get an error when we request another sequence number from another client.
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached minimum value of sequence \"s1\" (-9223372036854775808)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testMinInt64OverflowFailureInSequenceInDifferentSession() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 START -9223372036854775807 " +
          "minvalue -9223372036854775808 INCREMENT -1 CACHE 100");
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(-9223372036854775807L, rs.getLong("nextval"));

      rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(-9223372036854775808L, rs.getLong("nextval"));

      boolean exceptionOcurred = false;
      try {
        rs = statement.executeQuery("SELECT nextval('s1')");
      } catch (org.postgresql.util.PSQLException e) {
        assertTrue(e.getMessage().contains(
            "reached minimum value of sequence \"s1\" (-9223372036854775808)"));
        exceptionOcurred = true;
      }
      assertTrue(exceptionOcurred);
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      // Since the previous client already got all the available sequence numbers in its cache,
      // we should get an error when we request another sequence number from another client.
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached minimum value of sequence \"s1\" (-9223372036854775808)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  @Test
  public void testCurrvalFails() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("currval of sequence \"s1\" is not yet defined in this session");
      statement.executeQuery("SELECT currval('s1')");
    }
  }

  @Test
  public void testCurrval() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      statement.execute("SELECT nextval('s1')");
      ResultSet rs = statement.executeQuery("SELECT currval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("currval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      statement.execute("SELECT nextval('s1')");
      ResultSet rs = statement.executeQuery("SELECT currval('s1')");
      assertTrue(rs.next());
      assertEquals(2, rs.getLong("currval"));
    }
  }

  @Test
  public void testLastvalFails() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("lastval is not yet defined in this session");
      statement.execute("SELECT lastval()");
    }
  }

  @Test
  public void testLastvalInAnotherSessionFails() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      statement.execute("SELECT nextval('s1')");
      ResultSet rs = statement.executeQuery("SELECT lastval()");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("lastval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("lastval is not yet defined in this session");
      statement.execute("SELECT lastval()");
    }
  }

  @Test
  public void testLastval() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");
      statement.execute("CREATE SEQUENCE s2");

      statement.execute("SELECT nextval('s1')");
      ResultSet rs = statement.executeQuery("SELECT lastval()");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("lastval"));

      statement.execute("SELECT nextval('s2')");
      rs = statement.executeQuery("SELECT lastval()");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("lastval"));

      for (int i = 2; i <= 10; i++) {
        statement.execute("SELECT nextval('s1')");
      }
      rs = statement.executeQuery("SELECT lastval()");
      assertTrue(rs.next());
      assertEquals(10, rs.getLong("lastval"));

      statement.execute("SELECT nextval('s2')");
      rs = statement.executeQuery("SELECT lastval()");
      assertTrue(rs.next());
      assertEquals(2, rs.getLong("lastval"));
    }
  }

  @Test
  public void testNoCycle() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 MAXVALUE 2 NO CYCLE");
      for (int i = 1; i <= 2; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }

      thrown.expect(org.postgresql.util.PSQLException.class);
      thrown.expectMessage("reached maximum value of sequence \"s1\" (2)");
      statement.executeQuery("SELECT nextval('s1')");
    }
  }

  //------------------------------------------------------------------------------------------------
  // CYCLE tests.
  //------------------------------------------------------------------------------------------------
  @Test
  public void testCycle() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CYCLE MAXVALUE 2");
      for (int i = 1; i <= 2; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }
      // After reaching MAXVALUE the sequence should go back to 1.
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getInt("nextval"));
    }
  }

  // A cycled sequence always go back to the MINVALUE value in a sequence with a positive increment
  // regardless by how much the sequence is overflown.
  @Test
  public void testCycleWithBigPositiveIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      long increment = Long.MAX_VALUE / 2 + 1234567;
      statement.execute(String.format("CREATE SEQUENCE s1 CYCLE INCREMENT %d", increment));
      for (long i = 0; i < 2; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i * increment + 1L, rs.getLong("nextval"));
      }
      // After reaching MAXVALUE the sequence should go back to 1.
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("nextval"));
    }
  }

  // A cycled sequence always go back to the MINVALUE value in a sequence with a positive increment
  // regardless by how much the sequence is overflown.
  @Test
  public void testCycleWithBigNegativeIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      long increment = Long.MIN_VALUE / 2 - 1234567;
      statement.execute(
          String.format("CREATE SEQUENCE s1 MAXVALUE 0 START 0 CYCLE INCREMENT %d", increment));
      for (long i = 0; i < 2; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i * increment, rs.getLong("nextval"));
      }
      // After reaching MAXVALUE the sequence should go back to 1.
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(0, rs.getLong("nextval"));
    }
  }

  @Test
  public void testCycleWithPositiveIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CYCLE MINVALUE 5 START 8 MAXVALUE 10 INCREMENT 1");
      for (int i = 8; i <= 10; i++) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }
      // After reaching MAXVALUE the sequence should go back to MINVALUE which is 5.
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(5, rs.getInt("nextval"));
    }
  }

  @Test
  public void testCycleWithNegativeIncrement() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CYCLE MINVALUE 1 START 3 MAXVALUE 9 INCREMENT -1");
      for (int i = 3; i >= 1; i--) {
        ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("nextval"));
      }
      // After reaching MINVALUE the sequence should go back to MAXVALUE which is 3.
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(9, rs.getInt("nextval"));
    }
  }

  //------------------------------------------------------------------------------------------------
  // Newly-supported features tests.
  //------------------------------------------------------------------------------------------------
  @Test
  public void testSelectDirectlyFromSequenceTable() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");

      ResultSet rs = statement.executeQuery("SELECT last_value FROM s1");
      assertTrue(rs.next());
      assertEquals(1, rs.getInt("last_value"));
    }
  }

  @Test
  public void testSetvalAndSelect() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");

      ResultSet rs = statement.executeQuery("SELECT setval('s1', 45, false)");
      assertTrue(rs.next());
      assertEquals(45, rs.getInt("setval"));
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      ResultSet rs = statement.executeQuery("SELECT * FROM s1");
      assertTrue(rs.next());
      assertEquals(45, rs.getInt("last_value"));
      assertEquals(false, rs.getBoolean("is_called"));
    }
  }

  //------------------------------------------------------------------------------------------------
  // Serial type tests.
  //------------------------------------------------------------------------------------------------
  @Test
  public void testSerialTypes() throws Exception {
    List<String> serialTypes = Arrays.asList(
        "smallserial", "serial2", "serial", "serial4", "bigserial", "serial8");

    // Max values as defined in https://www.postgresql.org/docs/10/datatype-numeric.html
    List<Long> serialTypesMaxValues = Arrays.asList(
        32767L, 32767L, 2147483647L, 2147483647L, 9223372036854775807L, 9223372036854775807L);

    for (int i = 0; i < serialTypes.size(); i++) {
      String serialType = serialTypes.get(i);
      LOG.info("Testing serial type " + serialType);
      try (Statement statement = connection.createStatement()) {
        statement.execute(String.format("CREATE TABLE t(k %s primary key, v int)", serialType));
        for (int k = 1; k <= 10; k++) {
          statement.execute("INSERT INTO t(v) VALUES (3)");
          ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE k = " + k);
          assertTrue(rs.next());
        }
        ResultSet rs = statement.executeQuery(
            "SELECT max_value FROM pg_sequences WHERE sequencename = 't_k_seq'");
        assertTrue(rs.next());
        Long serialTypeMaxValue = serialTypesMaxValues.get(i);
        LOG.info(String.format("Expected max_value: %d, received max_value: ", serialTypeMaxValue,
            rs.getLong("max_value")));
        assertEquals(serialTypeMaxValue.longValue(), rs.getLong("max_value"));
        statement.execute("DROP TABLE t");
      }
    }
  }

  //------------------------------------------------------------------------------------------------
  // Test fix for https://github.com/YugaByte/yugabyte-db/issues/1783.
  //------------------------------------------------------------------------------------------------
  @Test
  public void testConcurrentInsertsWithSerialType() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE accounts(user_id SERIAL, username VARCHAR (50), " +
          "PRIMARY KEY(user_id, username))");
    }
    final int NUM_THREADS = 4;
    final int NUM_INSERTS_PER_THREAD = 100;
    ExecutorService ecs = Executors.newFixedThreadPool(NUM_THREADS);
    List<Future<?>> futures = new ArrayList<>();
    for (int i = 1; i <= NUM_THREADS; ++i) {
      final int threadIndex = i;
      Future<?> future = ecs.submit(() -> {
        try (Statement statement = connection.createStatement()) {
          for (int j = 0; j < NUM_INSERTS_PER_THREAD; ++j) {
            statement.execute(String.format("INSERT INTO accounts(username) VALUES ('user_%d_%d')",
                threadIndex, j));
            LOG.info(String.format("Inserted username user_%d_%d", threadIndex, j));
          }
        } catch (Exception e) {
          fail(e.getMessage());
        }
      });
      futures.add(future);
    }
    for (Future<?> future : futures) {
      future.get();
    }
    ecs.shutdown();
    ecs.awaitTermination(30, TimeUnit.SECONDS);
    try (Statement statement = connection.createStatement()) {
      ResultSet rs = statement.executeQuery("SELECT count(*) FROM accounts");
      assertTrue(rs.next());
      assertEquals(NUM_THREADS * NUM_INSERTS_PER_THREAD, rs.getLong("count"));

      rs = statement.executeQuery("SELECT max(user_id) FROM accounts");
      assertTrue(rs.next());
      assertEquals(NUM_THREADS * NUM_INSERTS_PER_THREAD, rs.getLong("max"));

      rs = statement.executeQuery("SELECT user_id, username FROM accounts ORDER BY user_id");
      for (int i = 1; i <= NUM_THREADS * NUM_INSERTS_PER_THREAD; ++i) {
        assertTrue(rs.next());
        assertEquals(i, rs.getInt("user_id"));
      }
    }
  }

  @Test
  public void testNextValAsDefaultValueInTable() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CACHE 20");
      statement.execute("CREATE TABLE t(k int NOT NULL DEFAULT nextval('s1'), v int)");
      for (int k = 1; k <= 10; k++) {
        statement.execute("INSERT INTO t(v) VALUES (10)");
        ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE k = " + k);
        assertTrue(rs.next());
      }
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      // Because of our current implementation, the first value is 22 for now instead of 21.
      for (int k = 21; k <= 30; k++) {
        statement.execute("INSERT INTO t(v) VALUES (10)");
        ResultSet rs = statement.executeQuery("SELECT * FROM t WHERE k = " + k);
        assertTrue(rs.next());
      }
    }
  }

  // Test that when we alter a sequence to be owned by a table's column, the sequence gets deleted
  // whenever the table gets deleted.
  @Test
  public void testOwnedSequenceGetsDeleted() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1 CACHE 100000");
      statement.execute("CREATE TABLE t(k int NOT NULL DEFAULT nextval('s1'))");
      statement.execute("ALTER SEQUENCE s1 OWNED BY t.k");
      statement.execute("DROP TABLE t");

      // Verify that the sequence was deleted.
      ResultSet rs = statement.executeQuery(
          "SELECT relname FROM pg_class WHERE relkind = 'S'");
      assertFalse(rs.next());
    }
  }

  @Test
  public void testAlterSequenceRestart() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE s1");

      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("nextval"));

      statement.execute("ALTER SEQUENCE s1 RESTART WITH 100");
    }

    try (Connection connection2 = getConnectionBuilder().connect();
        Statement statement = connection2.createStatement()) {
      ResultSet rs = statement.executeQuery("SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(100, rs.getLong("nextval"));
    }
  }

  // Retries once if we get error "Catalog Version Mismatch: A DDL occurred while processing this
  // query"
  private void ExecuteWithRetry(Statement statement, String stmt) throws Exception {
    for (int i = 0; i < 2; i++) {
      try {
        statement.execute(stmt);
        return;
      } catch (Exception e) {
        if (e.toString().contains("Catalog Version Mismatch: A DDL occurred while processing")) {
          continue;
        } else {
          throw e;
        }
      }
    }
    throw new RuntimeException("Failed to execute statement: " + stmt);
  }

  // Retries once if we get error "Catalog Version Mismatch: A DDL occurred while processing this
  // query"
  private ResultSet ExecuteQueryWithRetry(Statement statement, String query) throws Exception {
    for (int i = 0; i < 2; i++) {
      try {
        return statement.executeQuery(query);
      } catch (Exception e) {
        if (e.toString().contains("Catalog Version Mismatch: A DDL occurred while processing")) {
          continue;
        } else {
          throw e;
        }
      }
    }
    throw new RuntimeException("Failed to execute query");
  }

  private void WaitUntilTServerGetsNewYSqlCatalogVersion() throws Exception {
    // ysql_catalog_version gets propagated through the heartbeat. Wait at least one heartbeat
    // (500 ms set through flag raft_heartbeat_interval_ms) period to give TS2 enough time to
    // realize that its cache is invalid.
    waitForTServerHeartbeat();
  }

  @Test
  public void testAlterSequence() throws Exception {
    try (Statement statement = connection.createStatement();
        Connection connection2 = getConnectionBuilder().withTServer(1).connect();
        Statement statement2 = connection2.createStatement()) {

      statement.execute("CREATE SEQUENCE s1");
      ResultSet rs = ExecuteQueryWithRetry(statement, "SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("nextval"));

      // -------------------------------------------------------------------------------------------
      // Test INCREMENT option.
      // -------------------------------------------------------------------------------------------
      statement.execute("ALTER SEQUENCE s1 INCREMENT 100");

      rs = ExecuteQueryWithRetry(statement2, "SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(101, rs.getLong("nextval"));

      // -------------------------------------------------------------------------------------------
      // Test CACHE option.
      // -------------------------------------------------------------------------------------------
      statement.execute("ALTER SEQUENCE s1 CACHE 5");
      rs = ExecuteQueryWithRetry(statement, "SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(201, rs.getLong("nextval"));


      // CACHE is 5 elements. The previous request should have cached 201, 301, 401, 501, and 601.
      WaitUntilTServerGetsNewYSqlCatalogVersion();
      rs = ExecuteQueryWithRetry(statement2, "SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(701, rs.getLong("nextval"));

      // -------------------------------------------------------------------------------------------
      // Test RESTART option.
      // -------------------------------------------------------------------------------------------
      ExecuteWithRetry(statement, "ALTER SEQUENCE s1 RESTART CACHE 1");

      // Consume the rest of the numbers in the cache: 801, 901, 1001, 1101.
      for (int i = 0; i < 4; i++) {
        rs = statement2.executeQuery("SELECT nextval('s1')");
        assertTrue(rs.next());
        assertEquals(801 + i * 100, rs.getLong("nextval"));
      }

      // After all the elements in the cache have been used, the next value should be 1 again
      // because the sequence was restarted.
      WaitUntilTServerGetsNewYSqlCatalogVersion();
      rs = ExecuteQueryWithRetry(statement2,"SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("nextval"));

      // -------------------------------------------------------------------------------------------
      // Test RESTART WITH option.
      // -------------------------------------------------------------------------------------------
      statement.execute("ALTER SEQUENCE s1 RESTART WITH 9");

      WaitUntilTServerGetsNewYSqlCatalogVersion();
      rs = ExecuteQueryWithRetry(statement2,"SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(9, rs.getLong("nextval"));

      // -------------------------------------------------------------------------------------------
      // Test START WITH option.
      // -------------------------------------------------------------------------------------------
      ExecuteWithRetry(statement2, "ALTER SEQUENCE s1 START WITH 1000");

      // After RESTART the sequence should start with 1000 that was set by the previous statement.
      WaitUntilTServerGetsNewYSqlCatalogVersion();
      ExecuteWithRetry(statement, "ALTER SEQUENCE s1 RESTART");

      WaitUntilTServerGetsNewYSqlCatalogVersion();
      rs = ExecuteQueryWithRetry(statement2, "SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1000, rs.getLong("nextval"));

      // -------------------------------------------------------------------------------------------
      // Test CYCLE option.
      // -------------------------------------------------------------------------------------------
      ExecuteWithRetry(statement, "ALTER SEQUENCE s1 RESTART WITH 1 INCREMENT -1 CACHE 1");
      rs = ExecuteQueryWithRetry(statement2,"SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(1, rs.getLong("nextval"));

      WaitUntilTServerGetsNewYSqlCatalogVersion();
      // Verify that getting next value without CYCLE fails.
      try {
        rs = ExecuteQueryWithRetry(statement2,"SELECT nextval('s1')");
        fail("Expected exception but got none");
      } catch (Exception e) {
        assertTrue(e.getMessage().contains("reached minimum value of sequence \"s1\" (1)"));
      }

      // Alter sequence to add CYCLE option.
      statement2.execute("ALTER SEQUENCE s1 CYCLE");

      WaitUntilTServerGetsNewYSqlCatalogVersion();
      rs = ExecuteQueryWithRetry(statement,"SELECT nextval('s1')");
      assertTrue(rs.next());
      assertEquals(Long.MAX_VALUE, rs.getLong("nextval"));
    }
  }
}
