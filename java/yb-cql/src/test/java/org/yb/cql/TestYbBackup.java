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
package org.yb.cql;

import java.text.SimpleDateFormat;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.io.BufferedReader;
import java.io.InputStreamReader;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.json.JSONObject;

import org.yb.client.TestUtils;
import org.yb.util.YBTestRunnerNonTsanAsan;

import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

@RunWith(value=YBTestRunnerNonTsanAsan.class)
public class TestYbBackup extends BaseCQLTest {
  private final static int defaultYbBackupTimeoutInSeconds = 180;

  @Override
  public int getTestMethodTimeoutSec() {
    return 360; // Usual time for a test ~90 seconds. But can be much more on Jenkins.
  }

  @Override
  protected int overridableNumShardsPerTServer() {
    return 2;
  }

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("allow_index_table_read_write", "1");
    return flagMap;
  }

  protected String runProcess(List<String> args, int timeoutSeconds) throws Exception {
    String processStr = "";
    for (String arg: args) {
      processStr += (processStr.isEmpty() ? "" : " ") + arg;
    }
    LOG.info("RUN:" + processStr);

    ProcessBuilder processBuilder = new ProcessBuilder(args);
    final Process process = processBuilder.start();
    String line = null;

    final BufferedReader stderrReader =
        new BufferedReader(new InputStreamReader(process.getErrorStream()));
    while ((line = stderrReader.readLine()) != null) {
      LOG.info("STDERR: " + line);
    }

    final BufferedReader stdoutReader =
        new BufferedReader(new InputStreamReader(process.getInputStream()));
    StringBuilder stdout = new StringBuilder();
    while ((line = stdoutReader.readLine()) != null) {
      stdout.append(line + "\n");
    }

    if (!process.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
      fail("Timeout of process run (" + timeoutSeconds + " seconds): [" + processStr + "]");
    }

    final int exitCode = process.exitValue();
    LOG.info("Process [" + processStr + "] exit code: " + exitCode);

    if (exitCode != 0) {
      LOG.info("STDOUT:\n" + stdout.toString());
      fail("Failed process with exit code " + exitCode + ": [" + processStr + "]");
    }

    return stdout.toString();
  }

  protected String runYbBackup(List<String> args) throws Exception {
    final String ybAdminPath = TestUtils.findBinary("yb-admin");
    final String ybBackupPath = TestUtils.findBinary("../../../managed/devops/bin/yb_backup.py");

    List<String> processCommand = new ArrayList<String>(Arrays.asList(
        ybBackupPath,
        "--masters", masterAddresses,
        "--remote_yb_admin_binary=" + ybAdminPath,
        "--storage_type", "nfs",
        "--no_ssh",
        "--no_auto_name"));
    if (!TestUtils.IS_LINUX) {
      processCommand.add("--mac");
      // Temporary flag to get more detailed log while the tests are failing on MAC: issue #4924.
      processCommand.add("--verbose");
    }

    processCommand.addAll(args);
    assert(processCommand.contains("create") || processCommand.contains("restore"));
    final String output = runProcess(processCommand, defaultYbBackupTimeoutInSeconds);
    LOG.info("yb_backup output: " + output);

    JSONObject json = new JSONObject(output);
    if (json.has("error")) {
      final String error = json.getString("error");
      LOG.info("yb_backup failed with error: " + error);
      fail("yb_backup failed with error: " + error);
    }

    return output;
  }

  public static String getTempBackupDir() {
    return TestUtils.getBaseTmpDir() + "/backup";
  }

  protected void runYbBackupCreate(String... args) throws Exception {
    List<String> processCommand = new ArrayList<String>(Arrays.asList(
        "--backup_location", getTempBackupDir(),
        "create"));
    processCommand.addAll(Arrays.asList(args));
    final String output = runYbBackup(processCommand);
    JSONObject json = new JSONObject(output);
    final String url = json.getString("snapshot_url");
    LOG.info("SUCCESS. Backup-create operation result - snapshot url: " + url);
  }

  protected void runYbBackupRestore(String... args) throws Exception {
    List<String> processCommand = new ArrayList<String>(Arrays.asList(
        "--backup_location", getTempBackupDir(),
        "restore"));
    processCommand.addAll(Arrays.asList(args));
    final String output = runYbBackup(processCommand);
    JSONObject json = new JSONObject(output);
    final boolean resultOk = json.getBoolean("success");
    LOG.info("SUCCESS. Backup-restore operation result: " + resultOk);
    assert(resultOk);
  }

  public enum TableProperty {
    TRANSACTIONAL,
    NON_TRANSACTIONAL;
  }

  public void setupTablesBeforeBackup(TableProperty tp) throws Exception {
    final String tableProp = (tp == TableProperty.TRANSACTIONAL ?
        "with transactions = { 'enabled' : true };" : ";");
    session.execute("create table test_tbl " +
        "(h int, r1 int, r2 int, c int, primary key ((h), r1, r2)) " + tableProp);

    final String indexTrans =
        " transactions = {'enabled' : false, 'consistency_level' : 'user_enforced'};";
    final String withIndexProp = (tp == TableProperty.TRANSACTIONAL ? ";" : " with" + indexTrans);
    final String andIndexProp = (tp == TableProperty.TRANSACTIONAL ? ";" : " and" + indexTrans);
    session.execute("create index i1 on test_tbl (h, r2, r1) include (c)" + withIndexProp);
    // Special case - reordering PK columns.
    session.execute("create index i2 on test_tbl (r2, r1, h)" + withIndexProp);
    // Unique index.
    session.execute("create unique index i3 on test_tbl (r1, r2, c) " +
                    "with clustering order by (r2 desc, c asc)" + andIndexProp);

    session.execute("insert into test_tbl (h, r1, r2, c) values (1, 2, 3, 4);");

    // Different types testing.
    session.execute("create table test_types (" +
                    "c1 tinyint, c2 smallint, c3 integer, c4 bigint, " +
                    "c5 float, c6 double, " +
                    "c7 varchar, c8 text, " +
                    "c9 boolean, " +
                    "c10 date, c11 time, c12 timestamp, " +
                    "c13 inet, " +
                    "c14 uuid, " +
                    "c15 timeuuid, " +
                    "c16 jsonb, " +
                    "fm frozen<map<int, text>>, " +
                    "fs frozen<set<inet>>, " +
                    "fl frozen<list<double>>," +
                    "primary key (c1))" + tableProp);
    session.execute("create index c2i on test_types (c2)" + withIndexProp);
    session.execute("create index c3i on test_types (c3)" + withIndexProp);
    session.execute("create index c4i on test_types (c4)" + withIndexProp);
    session.execute("create index c5i on test_types (c5)" + withIndexProp);
    session.execute("create index c6i on test_types (c6)" + withIndexProp);
    session.execute("create index c7i on test_types (c7)" + withIndexProp);
    session.execute("create index c8i on test_types (c8)" + withIndexProp);
    session.execute("create index c9i on test_types (c9)" + withIndexProp);
    session.execute("create index c10i on test_types (c10)" + withIndexProp);
    session.execute("create index c11i on test_types (c11)" + withIndexProp);
    session.execute("create index c12i on test_types (c12)" + withIndexProp);
    session.execute("create index c13i on test_types (c13)" + withIndexProp);
    session.execute("create index c14i on test_types (c14)" + withIndexProp);
    session.execute("create index c15i on test_types (c15)" + withIndexProp);
    runInvalidStmt("create index c16i on test_types (c16)" + withIndexProp,
                   "Invalid Primary Key Column Datatype");
    session.execute("create index fmi on test_types (fm)" + withIndexProp);
    session.execute("create index fsi on test_types (fs)" + withIndexProp);
    session.execute("create index fli on test_types (fl)" + withIndexProp);

    session.execute("insert into test_types (c1, c2, c3, c4, c5, c6, c7, c8, " +
                    "c9, c10, c11, c12, c13, c14, c15, c16, fm, fs, fl) values " +
                    "(1, 2, 3, 4, 5.5, 6.6, '7', '8', true, '2020-7-29', '1:2:3.123456789', " +
                    "'2020-7-29 13:24:56.987+01:00', '127.0.0.1', " +
                    "11111111-2222-3333-4444-555555555555, " +
                    "f58ba3dc-3422-11e7-a919-92ebcb67fe33, " +
                    "'{\"a\":0}', {1:'a',2:'b'}, {'1.2.3.4','5.6.7.8'}, [1.1, 2.2]);");

    // Test index on JSON-attribute.
    session.execute("create table test_json_tbl (h int primary key, j jsonb) " + tableProp);
    session.execute("create index json_idx on test_json_tbl (j->'a'->>'b')" + withIndexProp);

    session.execute("insert into test_json_tbl (h, j) " +
                    "values (1, '{\"a\":{\"b\":\"b4\"},\"c\":4}');");

    // Update manually only user_enforced indexes.
    if (tp == TableProperty.NON_TRANSACTIONAL) {
      session.execute("insert into i1 (\"C$_h\", \"C$_r2\", \"C$_r1\", \"C$_c\") " +
                      "values (1, 3, 2, 4);");
      session.execute("insert into i2 (\"C$_r2\", \"C$_r1\", \"C$_h\") " +
                      "values (3, 2, 1);");
      session.execute("insert into i3 (\"C$_r1\", \"C$_r2\", \"C$_c\", \"C$_h\") " +
                      "values (2, 3, 4, 1);");

      session.execute("insert into c2i (\"C$_c2\", \"C$_c1\") values (2, 1);");
      session.execute("insert into c3i (\"C$_c3\", \"C$_c1\") values (3, 1);");
      session.execute("insert into c4i (\"C$_c4\", \"C$_c1\") values (4, 1);");
      session.execute("insert into c5i (\"C$_c5\", \"C$_c1\") values (5.5, 1);");
      session.execute("insert into c6i (\"C$_c6\", \"C$_c1\") values (6.6, 1);");
      session.execute("insert into c7i (\"C$_c7\", \"C$_c1\") values ('7', 1);");
      session.execute("insert into c8i (\"C$_c8\", \"C$_c1\") values ('8', 1);");
      session.execute("insert into c9i (\"C$_c9\", \"C$_c1\") values (true, 1);");
      session.execute("insert into c10i (\"C$_c10\", \"C$_c1\") values ('2020-7-29', 1);");
      session.execute("insert into c11i (\"C$_c11\", \"C$_c1\") values ('1:2:3.123456789', 1);");
      session.execute("insert into c12i (\"C$_c12\", \"C$_c1\") values " +
                      "('2020-7-29 13:24:56.987+01:00', 1);");
      session.execute("insert into c13i (\"C$_c13\", \"C$_c1\") values ('127.0.0.1', 1);");
      session.execute("insert into c14i (\"C$_c14\", \"C$_c1\") values " +
                      "(11111111-2222-3333-4444-555555555555, 1);");
      session.execute("insert into c15i (\"C$_c15\", \"C$_c1\") values " +
                      "(f58ba3dc-3422-11e7-a919-92ebcb67fe33, 1);");
      session.execute("insert into fmi (\"C$_fm\", \"C$_c1\") values ({1:'a',2:'b'}, 1);");
      session.execute("insert into fsi (\"C$_fs\", \"C$_c1\") values ({'1.2.3.4','5.6.7.8'}, 1);");
      session.execute("insert into fli (\"C$_fl\", \"C$_c1\") values ([1.1, 2.2], 1);");

      session.execute("insert into json_idx (\"C$_j->\'J$_a\'->>\'J$_b\'\", \"C$_h\") " +
                      "values ('b4', 1);");
    }
  }

  public void updateValuesInTables(String keyspace, TableProperty tp) throws Exception {
    session.execute("insert into " + keyspace + ".test_tbl (h, r1, r2, c) values (1, 2, 3, 99);");
    session.execute("insert into " + keyspace + ".test_types (c1, c2, c3, c4, c5, c6, c7, c8, " +
                    "c9, c10, c11, c12, c13, c14, c15, c16, fm, fs, fl) values " +
                    "(1, 12, 13, 14, 15.5, 16.6, '17', '18', false, '2021-7-29', " +
                    "'11:2:3.123456789', '2021-7-29 13:24:56.987+01:00', '127.1.0.1', " +
                    "11111111-2222-3333-4444-999999999999, " +
                    "f58ba3dc-3422-11e7-a919-92ebcb67fe99, " +
                    "'{\"a\":10}', {11:'a',12:'b'}, {'11.2.3.4','15.6.7.8'}, [11.1, 12.2]);");

    session.execute("insert into " + keyspace + ".test_json_tbl (h, j) " +
                    "values (1, '{\"a\":{\"b\":\"b99\"},\"c\":99}');");

    // Update manually only user_enforced indexes.
    if (tp == TableProperty.NON_TRANSACTIONAL) {
      session.execute("insert into " + keyspace + ".i1 (\"C$_h\", \"C$_r2\", \"C$_r1\", \"C$_c\")" +
                      " values (1, 3, 2, 99);");
      session.execute("insert into " + keyspace + ".i2 (\"C$_r2\", \"C$_r1\", \"C$_h\")" +
                      " values (3, 2, 1);");
      session.execute("insert into " + keyspace + ".i3 (\"C$_r1\", \"C$_r2\", \"C$_c\", \"C$_h\")" +
                      " values (2, 3, 99, 1);");

      session.execute("insert into c2i (\"C$_c2\", \"C$_c1\") values (12, 1);");
      session.execute("insert into c3i (\"C$_c3\", \"C$_c1\") values (13, 1);");
      session.execute("insert into c4i (\"C$_c4\", \"C$_c1\") values (14, 1);");
      session.execute("insert into c5i (\"C$_c5\", \"C$_c1\") values (15.5, 1);");
      session.execute("insert into c6i (\"C$_c6\", \"C$_c1\") values (16.6, 1);");
      session.execute("insert into c7i (\"C$_c7\", \"C$_c1\") values ('17', 1);");
      session.execute("insert into c8i (\"C$_c8\", \"C$_c1\") values ('18', 1);");
      session.execute("insert into c9i (\"C$_c9\", \"C$_c1\") values (false, 1);");
      session.execute("insert into c10i (\"C$_c10\", \"C$_c1\") values ('2021-7-29', 1);");
      session.execute("insert into c11i (\"C$_c11\", \"C$_c1\") values ('11:2:3.123456789', 1);");
      session.execute("insert into c12i (\"C$_c12\", \"C$_c1\") values " +
                      "('2021-7-29 13:24:56.987+01:00', 1);");
      session.execute("insert into c13i (\"C$_c13\", \"C$_c1\") values ('127.1.0.1', 1);");
      session.execute("insert into c14i (\"C$_c14\", \"C$_c1\") values " +
                      "(11111111-2222-3333-4444-999999999999, 1);");
      session.execute("insert into c15i (\"C$_c15\", \"C$_c1\") values " +
                      "(f58ba3dc-3422-11e7-a919-92ebcb67fe99, 1);");
      session.execute("insert into fmi (\"C$_fm\", \"C$_c1\") values ({11:'a',12:'b'}, 1);");
      session.execute("insert into fsi (\"C$_fs\", \"C$_c1\") values " +
                      "({'11.2.3.4','15.6.7.8'}, 1);");
      session.execute("insert into fli (\"C$_fl\", \"C$_c1\") values ([11.1, 12.2], 1);");

      session.execute("insert into " + keyspace + ".json_idx " +
                      "(\"C$_j->\'J$_a\'->>\'J$_b\'\", \"C$_h\") values ('b99', 1);");
    }
  }

  public enum ValuesUpdateState {
    SOURCE,
    UPDATED;
  }

  public void checkValuesInTables(String keyspace,
                                  TableProperty tp,
                                  ValuesUpdateState state) throws Exception {
    SimpleDateFormat isoFormat = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss");
    isoFormat.setTimeZone(TimeZone.getTimeZone("UTC"));

    final String value_c = (state == ValuesUpdateState.UPDATED ? "99" : "4");
    assertQuery("select * from " + keyspace + ".test_tbl;",
                "Row[1, 2, 3, " + value_c + "]");
    assertQuery("select * from " + keyspace + ".i1;",
                "Row[1, 2, 3, " + value_c + "]");
    assertQuery("select * from " + keyspace + ".i2;", "Row[1, 2, 3]");
    assertQuery("select * from " + keyspace + ".i3;",
                "Row[1, 2, 3, " + value_c + "]");

    // Index 'i2' only scan.
    String select_stmt = "select r1, r2, h from " + keyspace + ".test_tbl where r2=3;";
    String result = session.execute("EXPLAIN " + select_stmt).all().toString();
    assertTrue("Should use index only scan", result.contains("Index Only Scan using"));
    assertTrue("Should use index i2",
               result.contains("using " + keyspace + ".i2 on " + keyspace + ".test_tbl"));
    assertQuery(select_stmt, "Row[2, 3, 1]");

    // Index 'i2' scan.
    select_stmt = "select * from " + keyspace + ".test_tbl where r2=3;";
    result = session.execute("EXPLAIN " + select_stmt).all().toString();
    assertTrue("Should use index scan", result.contains("Index Scan using"));
    assertTrue("Should use index i2",
               result.contains("using " + keyspace + ".i2 on " + keyspace + ".test_tbl"));
    assertQuery(select_stmt, "Row[1, 2, 3, " + value_c + "]");

    // Check unique index.
    if (tp == TableProperty.NON_TRANSACTIONAL) {
      runInvalidStmt("insert into " + keyspace + ".i3 (\"C$_r1\", \"C$_r2\", \"C$_c\", \"C$_h\")" +
                     " values (2, 3, " + value_c + ", 9);",
                     "Duplicate value disallowed by unique index");
    } else {
      runInvalidStmt("insert into " + keyspace + ".test_tbl (h, r1, r2, c) " +
                     "values (9, 2, 3, " + value_c + ");",
                     "Duplicate value disallowed by unique index");
    }

    if (state == ValuesUpdateState.UPDATED) {
      String dateStr = isoFormat.parse("2021-07-29T12:24:56").toString();
      assertQuery("select * from " + keyspace + ".test_types;",
                  "Row[1, 12, 13, 14, 15.5, 16.6, 17, 18, false, 2021-07-29, 39723123456789, " +
                    dateStr + ", /127.1.0.1, " +
                  "11111111-2222-3333-4444-999999999999, " +
                  "f58ba3dc-3422-11e7-a919-92ebcb67fe99, " +
                  "{\"a\":10}, {11=a, 12=b}, [/11.2.3.4, /15.6.7.8], [11.1, 12.2]]");

      assertQuery("select * from " + keyspace + ".c2i;", "Row[1, 12]");
      assertQuery("select * from " + keyspace + ".c3i;", "Row[1, 13]");
      assertQuery("select * from " + keyspace + ".c4i;", "Row[1, 14]");
      assertQuery("select * from " + keyspace + ".c5i;", "Row[1, 15.5]");
      assertQuery("select * from " + keyspace + ".c6i;", "Row[1, 16.6]");
      assertQuery("select * from " + keyspace + ".c7i;", "Row[1, 17]");
      assertQuery("select * from " + keyspace + ".c8i;", "Row[1, 18]");
      assertQuery("select * from " + keyspace + ".c9i;", "Row[1, false]");
      assertQuery("select * from " + keyspace + ".c10i;", "Row[1, 2021-07-29]");
      assertQuery("select * from " + keyspace + ".c11i;", "Row[1, 39723123456789]");
      assertQuery("select * from " + keyspace + ".c12i;", "Row[1, " + dateStr + "]");
      assertQuery("select * from " + keyspace + ".c13i;", "Row[1, /127.1.0.1]");
      assertQuery("select * from " + keyspace + ".c14i;",
                  "Row[1, 11111111-2222-3333-4444-999999999999]");
      assertQuery("select * from " + keyspace + ".c15i;",
                  "Row[1, f58ba3dc-3422-11e7-a919-92ebcb67fe99]");
      assertQuery("select * from " + keyspace + ".fmi;", "Row[1, {11=a, 12=b}]");
      assertQuery("select * from " + keyspace + ".fsi;", "Row[1, [/11.2.3.4, /15.6.7.8]]");
      assertQuery("select * from " + keyspace + ".fli;", "Row[1, [11.1, 12.2]]");
    } else {
      String dateStr = isoFormat.parse("2020-07-29T12:24:56").toString();
      assertQuery("select * from " + keyspace + ".test_types;",
                  "Row[1, 2, 3, 4, 5.5, 6.6, 7, 8, true, 2020-07-29, 3723123456789, " +
                    dateStr + ", /127.0.0.1, " +
                  "11111111-2222-3333-4444-555555555555, " +
                  "f58ba3dc-3422-11e7-a919-92ebcb67fe33, " +
                  "{\"a\":0}, {1=a, 2=b}, [/1.2.3.4, /5.6.7.8], [1.1, 2.2]]");

      assertQuery("select * from " + keyspace + ".c2i;", "Row[1, 2]");
      assertQuery("select * from " + keyspace + ".c3i;", "Row[1, 3]");
      assertQuery("select * from " + keyspace + ".c4i;", "Row[1, 4]");
      assertQuery("select * from " + keyspace + ".c5i;", "Row[1, 5.5]");
      assertQuery("select * from " + keyspace + ".c6i;", "Row[1, 6.6]");
      assertQuery("select * from " + keyspace + ".c7i;", "Row[1, 7]");
      assertQuery("select * from " + keyspace + ".c8i;", "Row[1, 8]");
      assertQuery("select * from " + keyspace + ".c9i;", "Row[1, true]");
      assertQuery("select * from " + keyspace + ".c10i;", "Row[1, 2020-07-29]");
      assertQuery("select * from " + keyspace + ".c11i;", "Row[1, 3723123456789]");
      assertQuery("select * from " + keyspace + ".c12i;", "Row[1, " + dateStr + "]");
      assertQuery("select * from " + keyspace + ".c13i;", "Row[1, /127.0.0.1]");
      assertQuery("select * from " + keyspace + ".c14i;",
                  "Row[1, 11111111-2222-3333-4444-555555555555]");
      assertQuery("select * from " + keyspace + ".c15i;",
                  "Row[1, f58ba3dc-3422-11e7-a919-92ebcb67fe33]");
      assertQuery("select * from " + keyspace + ".fmi;", "Row[1, {1=a, 2=b}]");
      assertQuery("select * from " + keyspace + ".fsi;", "Row[1, [/1.2.3.4, /5.6.7.8]]");
      assertQuery("select * from " + keyspace + ".fli;", "Row[1, [1.1, 2.2]]");
    }

    // Testing JSONB.
    assertQuery("select * from " + keyspace + ".test_json_tbl;",
                "Row[1, {\"a\":{\"b\":\"b" + value_c + "\"},\"c\":" + value_c + "}]");
    assertQuery("select * from " + keyspace + ".json_idx;",
                "Row[1, b" + value_c + "]");
    assertQuery("select * from " + keyspace + ".test_json_tbl " +
                "where j->'a'->>'b'='b" + value_c + "';",
                "Row[1, {\"a\":{\"b\":\"b" + value_c + "\"},\"c\":" + value_c + "}]");
  }

  public void testYCQLRestoreIntoKeyspace(TableProperty tp,
                                          String keyspace,
                                          String... createBackupArgs) throws Exception {
    setupTablesBeforeBackup(tp);
    checkValuesInTables(DEFAULT_TEST_KEYSPACE, tp, ValuesUpdateState.SOURCE);
    runYbBackupCreate(createBackupArgs);
    updateValuesInTables(DEFAULT_TEST_KEYSPACE, tp);
    checkValuesInTables(DEFAULT_TEST_KEYSPACE, tp, ValuesUpdateState.UPDATED);

    if (keyspace == DEFAULT_TEST_KEYSPACE) {
      runYbBackupRestore();
    } else {
      runYbBackupRestore("--keyspace", keyspace);
      checkValuesInTables(DEFAULT_TEST_KEYSPACE, tp, ValuesUpdateState.UPDATED);
    }

    checkValuesInTables(keyspace, tp, ValuesUpdateState.SOURCE);
    // Test writes into the restored tables.
    updateValuesInTables(keyspace, tp);
    checkValuesInTables(keyspace, tp, ValuesUpdateState.UPDATED);
  }

  @Test
  public void testYCQLKeyspaceBackup() throws Exception {
    // Using keyspace name only to test full-keyspace backup.
    testYCQLRestoreIntoKeyspace(TableProperty.NON_TRANSACTIONAL, "ks2",
        "--keyspace", DEFAULT_TEST_KEYSPACE);
  }

  @Test
  public void testYCQLKeyspaceBackup_Transactional() throws Exception {
    // Using keyspace name only to test full-keyspace backup.
    testYCQLRestoreIntoKeyspace(TableProperty.TRANSACTIONAL, "ks3",
        "--keyspace", DEFAULT_TEST_KEYSPACE);
  }

  @Test
  public void testYCQLTablesWithIndexesBackup() throws Exception {
    // Using explicit keyspace/table pairs to test multi-table backup.
    testYCQLRestoreIntoKeyspace(TableProperty.NON_TRANSACTIONAL, "ks4",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_tbl",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_types",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_json_tbl");
  }

  @Test
  public void testYCQLTablesWithIndexesBackup_Transactional() throws Exception {
    // Using explicit keyspace/table pairs to test multi-table backup.
   testYCQLRestoreIntoKeyspace(TableProperty.TRANSACTIONAL, "ks5",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_tbl",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_types",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_json_tbl");
  }

  @Test
  public void testYCQLBackupRestoringIntoOriginalKeyspace() throws Exception {
    // Using keyspace name only to test full-keyspace backup.
    testYCQLRestoreIntoKeyspace(TableProperty.NON_TRANSACTIONAL, DEFAULT_TEST_KEYSPACE,
        "--keyspace", DEFAULT_TEST_KEYSPACE);
  }

  @Test
  public void testYCQLBackupRestoringIntoOriginalKeyspace_Transactional() throws Exception {
    // Using keyspace name only to test full-keyspace backup.
    testYCQLRestoreIntoKeyspace(TableProperty.TRANSACTIONAL, DEFAULT_TEST_KEYSPACE,
        "--keyspace", DEFAULT_TEST_KEYSPACE);
  }

  @Test
  public void testYCQLBackupRestoringIntoOriginalTables() throws Exception {
    // Using explicit keyspace/table pairs to test multi-table backup.
    testYCQLRestoreIntoKeyspace(TableProperty.NON_TRANSACTIONAL, DEFAULT_TEST_KEYSPACE,
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_tbl",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_types",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_json_tbl");
  }

  @Test
  public void testYCQLBackupRestoringIntoOriginalTables_Transactional() throws Exception {
    // Using explicit keyspace/table pairs to test multi-table backup.
    testYCQLRestoreIntoKeyspace(TableProperty.TRANSACTIONAL, DEFAULT_TEST_KEYSPACE,
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_tbl",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_types",
        "--keyspace", DEFAULT_TEST_KEYSPACE, "--table", "test_json_tbl");
  }

  @Test
  public void testYCQLBackupWithUniqueIndex() throws Exception {
    session.execute("create table test_tbl (i int, j int, k int, l int, m int, n int, " +
                    "primary key (i, j, k, l)) with transactions = { 'enabled' : true };");

    session.execute("create UNIQUE INDEX i1 on test_tbl (i, j, k) COVERING (m, n);");
    session.execute("insert into test_tbl (i, j, k, l, m, n) values (1, 1, 1, 1, 1, 1);");
    session.execute("insert into test_tbl (i, j, k, l, m, n) values (2, 1, 1, 1, 1, 1);");
    session.execute("insert into test_tbl (i, j, k, l, m, n) values (1, 2, 1, 1, 1, 1);");
    session.execute("insert into test_tbl (i, j, k, l, m, n) values (1, 1, 2, 1, 1, 1);");

    // Violate the UNIQUE INDEX.
    runInvalidStmt("insert into test_tbl (i, j, k, l, m, n) values (1, 1, 1, 2, 2, 2);");
    runInvalidStmt("insert into test_tbl (i, j, k, l, m, n) values (2, 1, 1, 2, 2, 2);");
    runInvalidStmt("insert into test_tbl (i, j, k, l, m, n) values (1, 2, 1, 2, 2, 2);");
    runInvalidStmt("insert into test_tbl (i, j, k, l, m, n) values (1, 1, 2, 2, 2, 2);");

    runYbBackupCreate("--keyspace", DEFAULT_TEST_KEYSPACE);
    session.execute("insert into test_tbl (i, j, k, l, m, n) values (2, 2, 2, 1, 1, 1);");
    runYbBackupRestore("--keyspace", "ks6");

    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".test_tbl;",
                "Row[1, 1, 1, 1, 1, 1]" +
                "Row[1, 1, 2, 1, 1, 1]" +
                "Row[1, 2, 1, 1, 1, 1]" +
                "Row[2, 1, 1, 1, 1, 1]" +
                "Row[2, 2, 2, 1, 1, 1]");
    runInvalidStmt("insert into " + DEFAULT_TEST_KEYSPACE + ".test_tbl (i, j, k, l, m, n) " +
                   "values (1, 1, 1, 2, 2, 2);");
    runInvalidStmt("insert into " + DEFAULT_TEST_KEYSPACE + ".test_tbl (i, j, k, l, m, n) " +
                   "values (2, 1, 1, 2, 2, 2);");
    runInvalidStmt("insert into " + DEFAULT_TEST_KEYSPACE + ".test_tbl (i, j, k, l, m, n) " +
                   "values (1, 2, 1, 2, 2, 2);");
    runInvalidStmt("insert into " + DEFAULT_TEST_KEYSPACE + ".test_tbl (i, j, k, l, m, n) " +
                   "values (1, 1, 2, 2, 2, 2);");

    assertQuery("select * from ks6.test_tbl;",
                "Row[1, 1, 1, 1, 1, 1]" +
                "Row[1, 1, 2, 1, 1, 1]" +
                "Row[1, 2, 1, 1, 1, 1]" +
                "Row[2, 1, 1, 1, 1, 1]");
    runInvalidStmt("insert into ks6.test_tbl (i, j, k, l, m, n) values (1, 1, 1, 2, 2, 2);");
    runInvalidStmt("insert into ks6.test_tbl (i, j, k, l, m, n) values (2, 1, 1, 2, 2, 2);");
    runInvalidStmt("insert into ks6.test_tbl (i, j, k, l, m, n) values (1, 2, 1, 2, 2, 2);");
    runInvalidStmt("insert into ks6.test_tbl (i, j, k, l, m, n) values (1, 1, 2, 2, 2, 2);");
  }

  @Test
  public void testYCQLBackupWithJsonIndex() throws Exception {
    session.execute("create table test_json_tbl (h int primary key, j jsonb) " +
                    "with transactions = { 'enabled' : true };");
    session.execute("create index json_idx on test_json_tbl (j->'a'->>'b');");

    for (int i = 1; i <= 2000; ++i) {
      String s = String.valueOf(i);
      session.execute("insert into test_json_tbl (h, j) " +
                      "values (" + s + ", '{\"a\":{\"b\":\"b" + s + "\"},\"c\":" + s + "}');");
    }

    runYbBackupCreate("--keyspace", DEFAULT_TEST_KEYSPACE);

    assertQuery("select count(*) from " + DEFAULT_TEST_KEYSPACE + ".test_json_tbl;",
                "Row[2000]");
    session.execute("insert into test_json_tbl (h, j) " +
                    "values (9999, '{\"a\":{\"b\":\"b9999\"},\"c\":9999}');");

    runYbBackupRestore("--keyspace", "ks7");

    assertQuery("select count(*) from " + DEFAULT_TEST_KEYSPACE + ".test_json_tbl;",
                "Row[2001]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".test_json_tbl where h=1;",
                "Row[1, {\"a\":{\"b\":\"b1\"},\"c\":1}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".test_json_tbl where " +
                "j->'a'->>'b'='b1';", "Row[1, {\"a\":{\"b\":\"b1\"},\"c\":1}]");
    assertQuery("select * from " + DEFAULT_TEST_KEYSPACE + ".test_json_tbl where h=9999;",
                "Row[9999, {\"a\":{\"b\":\"b9999\"},\"c\":9999}]");

    assertQuery("select count(*) from ks7.test_json_tbl;",
                "Row[2000]");
    assertQuery("select * from ks7.test_json_tbl where h=1;",
                "Row[1, {\"a\":{\"b\":\"b1\"},\"c\":1}]");
    assertQuery("select * from ks7.test_json_tbl where j->'a'->>'b'='b1';",
                "Row[1, {\"a\":{\"b\":\"b1\"},\"c\":1}]");
    assertQuery("select * from ks7.test_json_tbl where h=9999;", "");
  }
}
