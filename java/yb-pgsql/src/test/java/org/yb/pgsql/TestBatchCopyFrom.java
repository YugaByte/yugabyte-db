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

import static org.yb.AssertionWrappers.fail;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.sql.SQLException;
import java.sql.Statement;

import org.apache.commons.lang3.StringUtils;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.postgresql.copy.CopyManager;
import org.postgresql.core.BaseConnection;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.TestUtils;
import org.yb.util.YBTestRunnerNonTsanOnly;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestBatchCopyFrom extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestBatchCopyFrom.class);

  private static final String INVALID_BATCH_OPTION_USAGE_WARNING_MSG =
      "Batched COPY is not supported";
  private static final String INVALID_BATCH_SIZE_ERROR_MSG =
      "argument to option \"rows_per_transaction\" must be a positive integer";
  private static final String INVALID_COPY_INPUT_ERROR_MSG =
      "invalid input syntax for integer";
  private static final String BATCH_TXN_SESSION_VARIABLE_NAME =
      "yb_default_copy_from_rows_per_transaction";
  private static final int BATCH_TXN_SESSION_VARIABLE_DEFAULT_ROWS = 1000;

  private String getAbsFilePath(String fileName) {
    return TestUtils.getBaseTmpDir() + "/" + fileName;
  }

  private void createFileInTmpDir(String absFilePath, int totalLines) throws IOException {
    File myObj = new File(absFilePath);
    myObj.createNewFile();

    BufferedWriter writer = new BufferedWriter(new FileWriter(myObj));
    writer.write("a,b,c,d\n");

    int totalValues = totalLines * 4;
    for (int i = 0; i < totalValues; i++) {
      if (i != 0 && (i + 1) % 4 == 0)
        writer.write("\n");
      else
        writer.write(i+",");
    }
    writer.close();
  }

  private void appendInvalidEntries(String absFilePath, int totalLines) throws IOException {
    FileWriter fileWriter = new FileWriter(absFilePath,true);
    BufferedWriter bufferedWriter = new BufferedWriter(fileWriter);
    for (int i = 0; i < totalLines; i++) {
      bufferedWriter.write("#,#,#,#\n");
    }
    bufferedWriter.close();
  }

  @Test
  public void testZeroBatchSize() throws Exception {
    String absFilePath = getAbsFilePath("zero-row-batch-copyfrom.txt");
    String tableName = "noBatchSizeTable";
    int totalValidLines = 2500;
    int totalInvalidLines = 500;

    createFileInTmpDir(absFilePath, totalValidLines);
    appendInvalidEntries(absFilePath, totalInvalidLines);

    try (Statement statement = connection.createStatement()) {
      // With batch size of 0, it will attempt to copy all rows in a single transaction.
      // But with invalid entries, it will rollback to 0 rows in the table.
      statement.execute(String.format("CREATE TABLE %s (a text, b text, c int, d int)", tableName));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION 0)",
              tableName, absFilePath),
          INVALID_COPY_INPUT_ERROR_MSG);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, 0);

      // Set the batch size to 1 which will copy all valid rows of 2500.
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION 1)",
              tableName, absFilePath),
          INVALID_COPY_INPUT_ERROR_MSG);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalValidLines);
    }
  }

  @Test
  public void testNegativeBatchSize() throws Exception {
    String absFilePath = getAbsFilePath("negative-rows-batch-copyfrom.txt");
    String tableName = "tempTable";
    int totalLines = 20;
    int batchSize = -1;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format(
          "CREATE TABLE %s (a int, b int, c int, d int)", tableName));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, absFilePath, batchSize),
          INVALID_BATCH_SIZE_ERROR_MSG);

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, 0);
    }
  }

  @Test
  public void testSmallBatchSize() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-small.txt");
    String tableName = "smallBatchSize";
    int totalLines = 100000;
    int batchSize = 100;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("CREATE TABLE %s (a int, b int, c text, d int)", tableName));
      statement.execute(String.format(
          "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
          tableName, absFilePath, batchSize));

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
    }
  }

  @Test
  public void testLargeBatchSize() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-large.txt");
    String tableName = "smallBatchSize";
    int totalLines = 100;
    int batchSize = 10000;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format(
          "CREATE TABLE %s (a serial primary key, b text, c text, d varchar)", tableName));
      statement.execute(String.format(
          "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
          tableName, absFilePath, batchSize));

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
    }
  }

  @Test
  public void testStatementLevelTriggersWithBatchCopy() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-with-statement-triggers.txt");
    String tableName = "testAfterTrigger";
    String dummyTableName = "dummyTable";
    int totalLines = 7;
    int batchSize = 1;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("CREATE TABLE %s (a int, b text, c int, d int)", tableName));
      statement.execute(String.format("CREATE TABLE %s (a int)", dummyTableName));
      statement.execute(String.format(
          "CREATE FUNCTION trigger_fn_on_dummy_table() RETURNS TRIGGER AS $$ BEGIN " +
          "INSERT INTO %s VALUES (1);" +
          "RETURN NEW; END; $$ LANGUAGE 'plpgsql';",
          dummyTableName));

      // test after-statement trigger
      statement.execute(String.format(
          "CREATE TRIGGER trig AFTER INSERT ON %s " +
          "FOR EACH STATEMENT EXECUTE PROCEDURE trigger_fn_on_dummy_table();",
          tableName));
      statement.execute(String.format(
          "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
          tableName, absFilePath, batchSize));
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 1);

      // test before-statement trigger
      statement.execute("DROP TRIGGER trig ON " + tableName);
      statement.execute(String.format(
          "CREATE TRIGGER trig BEFORE INSERT ON %s " +
          "FOR EACH STATEMENT EXECUTE PROCEDURE trigger_fn_on_dummy_table();",
          tableName));
      statement.execute(String.format(
          "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
          tableName, absFilePath, batchSize));
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines * 2);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 2);
    }
  }

  @Test
  public void testStatementLevelTriggersWithBatchCopyFailure() throws Exception {
    String absFilePath = getAbsFilePath("fail-batch-copyfrom-with-statement-triggers.txt");
    String tableName = "testAfterTrigger";
    String dummyTableName = "dummyTable";
    int totalValidLines = 7;
    int totalInvalidLines = 1;
    int batchSize = 1;

    createFileInTmpDir(absFilePath, totalValidLines);
    appendInvalidEntries(absFilePath, totalInvalidLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format(
          "CREATE TABLE %s (a serial, b text, c int, d int)", tableName));
      statement.execute(String.format("CREATE TABLE %s (a int)", dummyTableName));
      statement.execute(String.format(
          "CREATE FUNCTION trigger_fn_on_dummy_table() RETURNS TRIGGER AS $$ BEGIN " +
          "INSERT INTO %s VALUES (1);" +
          "RETURN NEW; END; $$ LANGUAGE 'plpgsql';",
          dummyTableName));

      // test after-statement trigger
      statement.execute(String.format(
          "CREATE TRIGGER trig AFTER INSERT ON %s " +
          "FOR EACH STATEMENT EXECUTE PROCEDURE trigger_fn_on_dummy_table();",
          tableName));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, absFilePath, batchSize),
          INVALID_COPY_INPUT_ERROR_MSG);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalValidLines);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 0);

      // test before-statement trigger
      statement.execute("DROP TRIGGER trig ON " + tableName);
      statement.execute(String.format(
          "CREATE TRIGGER trig BEFORE INSERT ON %s " +
          "FOR EACH STATEMENT EXECUTE PROCEDURE trigger_fn_on_dummy_table();",
          tableName));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, absFilePath, batchSize),
          INVALID_COPY_INPUT_ERROR_MSG);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalValidLines * 2);
      // note, before statement-level trigger will execute even if COPY FROM fails
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 1);
    }
  }

  @Test
  public void testOnTempTable() throws Exception {
    String absFilePath = getAbsFilePath("temp-table-batch-copyfrom.txt");
    String tableName = "tempTable";
    int totalLines = 20;
    int batchSize = 5;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format(
          "CREATE TEMPORARY TABLE %s (a int, b text, c int, d int)", tableName));
      verifyStatementWarning(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, absFilePath, batchSize),
          INVALID_BATCH_OPTION_USAGE_WARNING_MSG);

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
    }
  }

  @Test
  public void testInsideTransactionBlock() throws Exception {
    String absFilePath = getAbsFilePath("txn-block-batch-copyfrom.txt");
    String copyFromTableName = "batchedTable";
    int totalLines = 20;
    int batchSize = 5;
    String dummyTableName = "sampleTable";

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format(
          "CREATE TABLE %s (a int primary key, b text, c int, d text)", dummyTableName));
      statement.execute(String.format(
          "CREATE TABLE %s (a serial, b text primary key, c text, d varchar)", copyFromTableName));
      statement.execute("BEGIN TRANSACTION;");
      statement.execute(String.format("INSERT INTO %s (a, b) VALUES (1, 'val')", dummyTableName));
      verifyStatementWarning(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              copyFromTableName, absFilePath, batchSize),
          INVALID_BATCH_OPTION_USAGE_WARNING_MSG);
      statement.execute("END;");

      assertOneRow(statement, "SELECT COUNT(*) FROM " + copyFromTableName, totalLines);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 1);
    }
  }

  @Test
  public void testStdinCopy() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-stdin.txt");
    String tableName = "stdinBatchSizeTable";
    int totalLines = 5;
    int batchSize = 1;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("CREATE TABLE %s (a int, b int, c int, d text)", tableName));
      CopyManager copyManager = new CopyManager((BaseConnection) connection);
      copyManager.copyIn(
          String.format(
              "COPY %s FROM STDIN WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, batchSize),
          new BufferedReader(new FileReader(absFilePath))
      );

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
    }
  }

  @Test
  public void tesStdinCopyInTransactionBlockWithPreviousTransaction() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-stdin-txn-block-1.txt");
    String tableName = "stdinTxnBlockBatchSizeTable";
    String dummyTableName = "dummyTable";
    int totalLines = 5;
    int batchSize = 1;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("CREATE TABLE %s (a text, b int, c int, d int)", tableName));
      statement.execute(String.format("CREATE TABLE %s (a text)", dummyTableName));
      statement.execute("BEGIN TRANSACTION");
      statement.execute(String.format("INSERT INTO %s (a) VALUES ('abc')", dummyTableName));

      CopyManager copyManager = new CopyManager((BaseConnection) connection);
      // falls back to copying all rows in single transaction
      copyManager.copyIn(
          String.format(
              "COPY %s FROM STDIN WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, batchSize),
          new BufferedReader(new FileReader(absFilePath)));
      statement.execute("COMMIT TRANSACTION");

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 1);
    }
  }

  @Test
  public void tesStdinCopyFailureInTransactionBlockWithPreviousTransaction() throws Exception {
    String absFilePath = getAbsFilePath("fail-batch-copyfrom-stdin-txn-block.txt");
    String tableName = "stdinTxnBlockBatchSizeTable";
    String dummyTableName = "dummyTable";
    int totalValidLines = 7;
    int totalInvalidLines = 1;
    int batchSize = 1;

    createFileInTmpDir(absFilePath, totalValidLines);
    appendInvalidEntries(absFilePath, totalInvalidLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("CREATE TABLE %s (a int, b int, c int, d text)", tableName));
      statement.execute(String.format("CREATE TABLE %s (a int)", dummyTableName));
      statement.execute("BEGIN TRANSACTION");
      statement.execute(String.format("INSERT INTO %s (a) VALUES (1)", dummyTableName));

      try {
        CopyManager copyManager = new CopyManager((BaseConnection) connection);
        copyManager.copyIn(
            String.format(
                "COPY %s FROM STDIN WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
                tableName, batchSize),
            new BufferedReader(new FileReader(absFilePath)));
        fail(String.format("Statement did not fail with error: %s", INVALID_COPY_INPUT_ERROR_MSG));
      } catch (SQLException e) {
        if (StringUtils.containsIgnoreCase(e.getMessage(), INVALID_COPY_INPUT_ERROR_MSG)) {
          LOG.info("Expected exception", e);
        } else {
          fail(String.format("Unexpected Error Message. Got: '%s', Expected to contain: '%s'",
              e.getMessage(), INVALID_COPY_INPUT_ERROR_MSG));
        }
      }
      statement.execute("COMMIT TRANSACTION");

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, 0);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 0);
    }
  }

  @Test
  public void tesStdinCopyInTransactionBlockWithProceedingTransaction() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-stdin-txn-block-2.txt");
    String tableName = "stdinTxnBlockBatchSizeTable";
    String dummyTableName = "dummyTable";
    int totalLines = 5;
    int batchSize = 2;

    createFileInTmpDir(absFilePath, totalLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute(String.format("CREATE TABLE %s (a int, b int, c int, d int)", tableName));
      statement.execute(String.format("CREATE TABLE %s (a int)", dummyTableName));
      statement.execute("BEGIN TRANSACTION");
      CopyManager copyManager = new CopyManager((BaseConnection) connection);
      copyManager.copyIn(
          String.format(
              "COPY %s FROM STDIN WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, batchSize),
          new BufferedReader(new FileReader(absFilePath)));
      statement.execute(String.format("INSERT INTO %s (a) VALUES (1)", dummyTableName));
      statement.execute("COMMIT TRANSACTION");

      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, totalLines);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + dummyTableName, 1);
    }
  }

  @Test
  public void testSessionVariable() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-sessionvar.txt");
    String tableName = "batchSessionTable";
    int totalValidLines = 7;
    int totalInvalidLines = 1;
    int batchSessionSize = 2;
    // The first three batches of 2 rows will be successfully copied over,
    // since starting from the 8th row, entry is invalid.
    int expectedCopiedLines = batchSessionSize * 3;

    createFileInTmpDir(absFilePath, totalValidLines);
    appendInvalidEntries(absFilePath, totalInvalidLines);

    try (Statement statement = connection.createStatement()) {
      // set session variable
      statement.execute("SET " + BATCH_TXN_SESSION_VARIABLE_NAME + "=" + batchSessionSize);
      statement.execute(String.format(
          "CREATE TABLE %s (a Integer, b serial, c varchar, d int)", tableName));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER)", tableName, absFilePath),
          INVALID_COPY_INPUT_ERROR_MSG);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, expectedCopiedLines);

      // reset session variable (attempt to copy default of 1000 rows)
      statement.execute("RESET " + BATCH_TXN_SESSION_VARIABLE_NAME);
      assertOneRow(statement, "SHOW " + BATCH_TXN_SESSION_VARIABLE_NAME,
          String.valueOf(BATCH_TXN_SESSION_VARIABLE_DEFAULT_ROWS));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER)", tableName, absFilePath),
          INVALID_COPY_INPUT_ERROR_MSG);
      // total rows remain the same since none of the rows copied over
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, expectedCopiedLines);

      // set session variable to "default"
      statement.execute("SET " + BATCH_TXN_SESSION_VARIABLE_NAME + " = DEFAULT");
      assertOneRow(statement, "SHOW " + BATCH_TXN_SESSION_VARIABLE_NAME,
          String.valueOf(BATCH_TXN_SESSION_VARIABLE_DEFAULT_ROWS));
    }
  }

  @Test
  public void testSessionVariableWithRowsPerTransactionOption() throws Exception {
    String absFilePath = getAbsFilePath("batch-copyfrom-sessionvar-with-query-option.txt");
    String tableName = "batchSessionAndOptionTable";
    int totalValidLines = 7;
    int totalInvalidLines = 3;
    int batchSessionSize = 1;
    int batchOptionSize = 4;
    // Only the first batch of 4 rows will be successfully copied over
    // since starting from the 8th row, entry is invalid.
    int expectedCopiedLines = batchOptionSize;

    createFileInTmpDir(absFilePath, totalValidLines);
    appendInvalidEntries(absFilePath, totalInvalidLines);

    try (Statement statement = connection.createStatement()) {
      statement.execute("SET " + BATCH_TXN_SESSION_VARIABLE_NAME + "=" + batchSessionSize);
      statement.execute(String.format(
          "CREATE TABLE %s (a serial, b text, c varchar, d Integer)", tableName));
      runInvalidQuery(statement,
          String.format(
              "COPY %s FROM \'%s\' WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION %s)",
              tableName, absFilePath, batchOptionSize),
          INVALID_COPY_INPUT_ERROR_MSG);
      assertOneRow(statement, "SELECT COUNT(*) FROM " + tableName, expectedCopiedLines);
    }
  }
}
