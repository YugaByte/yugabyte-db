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
import java.sql.Statement;
import java.sql.SQLException;

import org.postgresql.util.PSQLException;
import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestOneOrTwoAdmins extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestOneOrTwoAdmins.class);

  private String executeOneOrTwoAdminsAssertion(
      Statement statement, boolean useCount) throws SQLException {
    // Count without using the COUNT statement.
    if (!useCount) {
      ResultSet result = statement.executeQuery("select * from staff where job = 'Admin'");
      int count = 0;
      while (result.next()) {
        count++;
      }
      if (1 <= count && count <= 2)
        return "passed";
      else
        return "failed";
    }

    ResultSet result = statement.executeQuery("select count(*) from staff where job = 'Admin'");
    assertTrue(result.next());
    int count = result.getInt(1);
    if (1 <= count && count <= 2)
      return "passed";
    else
      return "failed";
  }

  private static boolean isYBTxnException(PSQLException ex) {
    String msg = ex.getMessage();
    return msg.contains("Missing metadata for transaction:") ||
           msg.contains("Conflicts with higher priority transaction:") ||
           msg.contains("Transaction aborted:");
  }

  private int checkAssertion(String connDescription, Statement statement, boolean useCount)
      throws SQLException {
    LOG.info("Checking invariant on connection: " + connDescription + "; useCount=" + useCount);
    try {
      String result = executeOneOrTwoAdminsAssertion(statement, useCount);
      assertEquals("passed", result);
      return 1;
    } catch (PSQLException ex) {
      if (isYBTxnException(ex)) {
        return 0;
      }
      LOG.error("Unexpected exception:", ex);
      throw ex;
    }
  }

  @Test
  public void testOneOrTwoAdmins() throws Exception {
    try (Statement setupStatement = connection.createStatement()) {
      setupStatement.execute(
          "create table staff(" +
          "  name text constraint staff_name primary key, " +
          "  job  text constraint staff_job_id_nn not null, " +
          "            constraint staff_job_id_chk check (job in ( " +
          "              'Manager', "+
          "              'Admin', " +
          "              'Sales', " +
          "              'Marketing', " +
          "              'Developer'))" +
          "  );");
      setupStatement.execute(
          "insert into staff(name, job) values" +
          "  ('Mary',  'Manager')," +
          "  ('Susan', 'Developer')," +
          "  ('Helen', 'Sales')," +
          "  ('Bill',  'Marketing')," +
          "  ('Fred',  'Sales')," +
          "  ('John',  'Admin');");
    }

    try (
        Connection connection1 = getConnectionBuilder()
            .withIsolationLevel(IsolationLevel.SERIALIZABLE)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement statement1 = connection1.createStatement();

        Connection connection2 = getConnectionBuilder()
            .withIsolationLevel(IsolationLevel.SERIALIZABLE)
            .withAutoCommit(AutoCommit.DISABLED)
            .connect();
        Statement statement2 = connection2.createStatement();

        Connection connectionForCleanup = getConnectionBuilder()
            .withIsolationLevel(IsolationLevel.SERIALIZABLE)
            .withAutoCommit(AutoCommit.ENABLED)
            .connect();
        Statement statementForCleanup = connectionForCleanup.createStatement();

        Connection connectionForReading = getConnectionBuilder()
            .withIsolationLevel(IsolationLevel.REPEATABLE_READ)
            .withAutoCommit(AutoCommit.ENABLED)
            .connect();
        Statement statementForReading = connectionForReading.createStatement()) {

      int numSelectSuccess1 = 0;
      int numSelectSuccess2 = 0;
      final int NUM_ATTEMPTS = 100;
      for (int i = 1; i <= NUM_ATTEMPTS; ++i) {
        LOG.info("Starting iteration " + i);
        ResultSet rs = statementForReading.executeQuery("SELECT name, job FROM staff");
        while (rs.next()) {
          LOG.info(
              "Found existing row: name=" + rs.getString("name") + ", job=" + rs.getString("job"));
        }

        LOG.info("(txn 1) Adding Alice");
        statement1.execute("insert into staff(name, job) values('Alice', 'Admin');");
        LOG.info("(txn 2) Adding Bert");
        statement2.execute("insert into staff(name, job) values('Bert', 'Admin');");

        final boolean useCount = i % 2 == 0;
        numSelectSuccess1 += checkAssertion("connection 1", statement1, useCount);
        numSelectSuccess2 += checkAssertion("connection 2", statement2, useCount);

        connection1.commit();
        connection2.commit();

        LOG.info("Cleanup");
        statementForCleanup.execute("delete from staff where name in ('Alice', 'Bert')");
      }

      double skew = (numSelectSuccess1 - numSelectSuccess2) * 1.0 / NUM_ATTEMPTS;
      LOG.info("Stats: numSelectSuccess1=" + numSelectSuccess1 +
               ", numSelectSuccess2=" + numSelectSuccess2 +
               ", skew=" + skew);
      final double SKEW_LIMIT = 0.3;
      assertTrue("Skew must be less than " + SKEW_LIMIT + ": " + skew, skew < SKEW_LIMIT);
    }
  }

}
