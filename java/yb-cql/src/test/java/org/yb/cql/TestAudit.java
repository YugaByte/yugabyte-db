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

import static org.yb.AssertionWrappers.*;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
import java.util.Objects;
import java.util.Optional;
import java.util.stream.Collectors;

import org.apache.commons.lang3.StringUtils;
import org.apache.commons.lang3.time.StopWatch;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.yb.YBTestRunner;
import org.yb.client.TestUtils;
import org.yb.minicluster.LogErrorListener;
import org.yb.minicluster.LogPrinter;
import org.yb.minicluster.MiniYBDaemon;

import com.datastax.driver.core.BatchStatement;
import com.datastax.driver.core.BoundStatement;
import com.datastax.driver.core.Cluster;
import com.datastax.driver.core.PreparedStatement;
import com.datastax.driver.core.Session;
import com.datastax.driver.core.SimpleStatement;
import com.google.common.net.HostAndPort;

@RunWith(value = YBTestRunner.class)
public class TestAudit extends BaseCQLTest {

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flags = super.getTServerFlags();
    flags.put("use_cassandra_authentication", "true");
    return flags;
  }

  @Override
  public Cluster.Builder getDefaultClusterBuilder() {
    return super.getDefaultClusterBuilder().withCredentials("cassandra", "cassandra");
  }

  @Before
  public void createRolesAndInitLogListeners() throws Exception {
    for (Entry<HostAndPort, MiniYBDaemon> entry : miniCluster.getTabletServers().entrySet()) {
      int port = entry.getKey().getPort();
      MiniYBDaemon tserver = entry.getValue();
      tserver.getLogPrinter().addErrorListener(new AuditLogListener(port));
    }
    session.execute("CREATE ROLE user1 WITH login = true AND password = '123'");

    AuditConfig config = new AuditConfig();
    config.enabled = true;
    applyAuditConfig(config);
    discardAuditRecords();
  }

  @After
  public void dropRoles() throws Exception {
    session.execute("DROP ROLE user1");
  }

  @Test
  public void auth() throws Exception {
    try (Session session = getSession("user1", "123")) {
      List<AuditLogEntry> records = fetchNewAuditRecords();
      List<AuditLogEntry> authRecords = records.stream()
          .filter(e -> e.category.equals("AUTH"))
          .collect(Collectors.toList());
      assertEquals(4, authRecords.size()); // 3 nodes + 1 control connection
      for (AuditLogEntry s : authRecords) {
        assertEquals("LOGIN_SUCCESS", s.type);
      }
    }

    try (Session session = getSession("user1", "321")) {
      fail("These credentials should be invalid!");
    } catch (com.datastax.driver.core.exceptions.AuthenticationException ex) {
      List<AuditLogEntry> records = fetchNewAuditRecords();
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "null", "LOGIN_ERROR", "AUTH",
                  null /* batchId */, null /* keyspace */, null /* scope */,
                  "LOGIN FAILURE; Provided username user1 and/or password are incorrect")),
          records);
    } catch (Exception ex) {
      throw ex;
    }

    // Plaintext password should be replaced by <REDACTED>.
    {
      // Just to use session for the first time - this gets called internally anyway.
      useKeyspace("cql_test_keyspace");
      discardAuditRecords();
      session.execute("CREATE ROLE user2 WITH login = true AND pAsSWorD=  'hide me!'");
      session.execute("ALTER ROLE user2 WITH PaSswORd   ='hide me too!'");
      List<AuditLogEntry> records = fetchNewAuditRecords();
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "CREATE_ROLE", "DCL",
                  null /* batchId */, null /* keyspace */, null /* scope */,
                  "CREATE ROLE user2 WITH login = true AND pAsSWorD=  <REDACTED>"),
              new AuditLogEntry('E', "cassandra", "ALTER_ROLE", "DCL",
                  null /* batchId */, null /* keyspace */, null /* scope */,
                  "ALTER ROLE user2 WITH PaSswORd   =<REDACTED>")),
          records);
    }
  }

  /** Issuing DML batch as YCQL plaintext: {@code START TXN; DML1; DML2; COMMIT} */
  @Test
  public void batchPlaintext() throws Exception {
    session.execute("CREATE TABLE batch_t (id int PRIMARY KEY)"
        + " WITH transactions = {'enabled': 'true'}");
    discardAuditRecords();

    String startTxnCql = "START TRANSACTION";
    String insertCql = "INSERT INTO batch_t (id) VALUES (1)";
    String commitCql = "COMMIT";

    // Execute as one-liner.
    {
      String oneLinerCql = StringUtils.joinWith("; ", startTxnCql, insertCql, insertCql, commitCql);
      session.execute(oneLinerCql);
      List<AuditLogEntry> records = fetchNewAuditRecords();
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "BATCH", "DML",
                  null /* batchId */, null /* keyspace */, null /* scope */,
                  oneLinerCql)),
          records);
    }

    // Execute line by line.
    // Batch request ID is not present in this case.
    // (For Cassandra, using BEGIN BATCH in this way is grammatically incorrect)
    {
      session.execute(startTxnCql);
      session.execute(insertCql);
      session.execute(insertCql);
      session.execute(commitCql);
      List<AuditLogEntry> records = fetchNewAuditRecords();
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "BATCH", "DML",
                  null /* batchId */, null /* keyspace */, null /* scope */,
                  startTxnCql),
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "batch_t",
                  insertCql),
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "batch_t",
                  insertCql),
              new AuditLogEntry('E', "cassandra", "BATCH", "DML",
                  null /* batchId */, null /* keyspace */, null /* scope */,
                  commitCql)),
          records);
    }
  }

  /** Issuing DML batch as driver-level {@code BatchStatement} */
  @Test
  public void batchDriverStatement() throws Exception {
    session.execute("CREATE TABLE batch_t (id int PRIMARY KEY)"
        + " WITH transactions = {'enabled': 'true'}");
    discardAuditRecords();

    String insertCql = "INSERT INTO batch_t (id) VALUES (1)";
    String insertPrepCql = "INSERT INTO batch_t (id) VALUES (?)";

    PreparedStatement preparedInsert = session.prepare(insertPrepCql);
    BatchStatement batch = new BatchStatement()
        .add(new SimpleStatement(insertCql))
        .add(preparedInsert.bind(2))
        .add(preparedInsert.bind(3));

    session.execute(batch);

    // Three PREPARE calls (one per tserver), three UPDATEs

    List<AuditLogEntry> records = fetchNewAuditRecords();
    String batchId = getBatchId(records);

    AuditLogEntry prepareEntry = new AuditLogEntry('E', "cassandra", "PREPARE_STATEMENT", "PREPARE",
        null /* batchId */, DEFAULT_TEST_KEYSPACE, "batch_t",
        insertPrepCql);
    assertAuditRecords(
        Arrays.asList(
            prepareEntry, prepareEntry, prepareEntry,
            new AuditLogEntry('E', "cassandra", "BATCH", "DML",
                batchId, null /* keyspace */, null /* scope */,
                "BatchId:[" + batchId + "] - BATCH of [3] statements"),
            new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                batchId, DEFAULT_TEST_KEYSPACE, "batch_t",
                insertCql),
            new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                batchId, DEFAULT_TEST_KEYSPACE, "batch_t",
                insertPrepCql),
            new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                batchId, DEFAULT_TEST_KEYSPACE, "batch_t",
                insertPrepCql)),
        records);
  }

  /** Verifies that driver-level batch request with stale metadata works. */
  @Test
  public void batchWithStaleMetadata() throws Exception {
    {
      session.execute("CREATE TABLE test_batch (h INT, r BIGINT, PRIMARY KEY ((h), r))");
      discardAuditRecords();

      String insertCql = "INSERT INTO test_batch (h, r) VALUES (1, 1)";

      BatchStatement batch = new BatchStatement();
      batch.add(new SimpleStatement("INSERT INTO test_batch (h, r) VALUES (1, 1)"));
      session.execute(batch);

      List<AuditLogEntry> records = fetchNewAuditRecords();
      String batchId = getBatchId(records);
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "BATCH", "DML",
                  batchId, null /* keyspace */, null /* scope */,
                  "BatchId:[" + batchId + "] - BATCH of [1] statements"),
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  batchId, DEFAULT_TEST_KEYSPACE, "test_batch",
                  insertCql)),
          records);
    }

    {
      session.execute("DROP TABLE test_batch");
      session.execute("CREATE TABLE test_batch (h INT, r TEXT, PRIMARY KEY ((h), r))");
      discardAuditRecords();

      String insertCql = "INSERT INTO test_batch (h, r) VALUES (1, ?)";

      BatchStatement batch = new BatchStatement();
      batch.add(new SimpleStatement(insertCql, "R" + 1));
      session.execute(batch);

      List<AuditLogEntry> records = fetchNewAuditRecords();
      String batchId = getBatchId(records);
      // TODO: We probably shouldn't log an error here as the user doesn't receive it.
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "BATCH", "DML",
                  batchId, null /* keyspace */, null /* scope */,
                  "BatchId:[" + batchId + "] - BATCH of [1] statements"),
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  batchId, DEFAULT_TEST_KEYSPACE, "test_batch",
                  insertCql),
              new AuditLogEntry('E', "cassandra", "REQUEST_FAILURE", "ERROR",
                  batchId, null /* keyspace */, null /* scope */,
                  insertCql + "; "
                      + "Invalid Arguments. unexpected number byte length: expected 8, provided 2"),
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  batchId, DEFAULT_TEST_KEYSPACE, "test_batch",
                  insertCql)),
          records);
    }
  }

  @Test
  public void dmlAndQuery() throws Exception {
    session.execute("CREATE TABLE t (id int PRIMARY KEY, v text)");
    discardAuditRecords();

    // INSERT
    {
      String insertCql = "INSERT INTO t (id, v) VALUES (1, 'v1')";
      session.execute(insertCql);
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  insertCql)),
          fetchNewAuditRecords());
    }

    // INSERT (prepared)
    {
      String insertBindCql = "INSERT INTO t (id, v) VALUES (?, ?)";
      BoundStatement bound = session.prepare(insertBindCql).bind(1, "v1-1");
      assertAuditRecords(
          Collections.nCopies(3 /* One per node */,
              new AuditLogEntry('E', "cassandra", "PREPARE_STATEMENT", "PREPARE",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  insertBindCql)),
          fetchNewAuditRecords());
      session.execute(bound);
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  insertBindCql)),
          fetchNewAuditRecords());
    }

    // SELECT
    {
      String selectCql = "SELECT * FROM t";
      assertQueryRowsOrdered(selectCql, "Row[1, v1-1]");
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "SELECT", "QUERY",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  selectCql)),
          fetchNewAuditRecords());
    }

    // UPDATE
    {
      String updateCql = "UPDATE t SET v = 'v1-2' WHERE id = 1";
      session.execute(updateCql);
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "UPDATE", "DML",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  updateCql)),
          fetchNewAuditRecords());
    }

    // SELECT (prepared)
    {
      String selectCql = "SELECT id, v FROM t WHERE id = ?";
      BoundStatement bound = session.prepare(selectCql).bind(1);
      assertAuditRecords(
          Collections.nCopies(3 /* One per node */,
              new AuditLogEntry('E', "cassandra", "PREPARE_STATEMENT", "PREPARE",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  selectCql)),
          fetchNewAuditRecords());
      assertQueryRowsOrdered(bound, Arrays.asList("Row[1, v1-2]"));
      assertAuditRecords(
          Arrays.asList(
              new AuditLogEntry('E', "cassandra", "SELECT", "QUERY",
                  null /* batchId */, DEFAULT_TEST_KEYSPACE, "t",
                  selectCql)),
          fetchNewAuditRecords());
    }
  }

  // FIXME: log level, KS, cat, user

  //
  // Helpers
  //

  private Session getSession(String username, String password) {
    Cluster.Builder cb = getDefaultClusterBuilder().withCredentials(username, password);
    Cluster c = cb.build();
    Session s = c.connect();
    return s;
  }

  private List<AuditLogEntry> auditRecords = Collections.synchronizedList(new ArrayList<>());

  /** Discard existing audit records so that only newer ones will be retrieved. */
  private void discardAuditRecords() throws Exception {
    Thread.sleep(100);
    auditRecords.clear();
  }

  /** Retrieve the audit records added to the log since last call. */
  private List<AuditLogEntry> fetchNewAuditRecords() throws Exception {
    Thread.sleep(100);
    synchronized (auditRecords) {
      List<AuditLogEntry> result = new ArrayList<>(auditRecords);
      auditRecords.clear();
      return result;
    }
  }

  private String getBatchId(List<AuditLogEntry> records) throws Exception {
    Optional<String> batchIdOpt = records.stream()
        .filter(e -> e.type.equals("BATCH")).map(e -> e.batchId).findFirst();
    assertTrue("No BATCH requests were present! Audit: " + records, batchIdOpt.isPresent());
    return batchIdOpt.get();
  }

  private void runProcess(List<String> args) throws Exception {
    assertEquals(0, new ProcessBuilder(args).start().waitFor());
  }

  /**
   * Applies non-null values from the given audit config to all tservers. Null values are not
   * changed.
   */
  private void applyAuditConfig(AuditConfig config) throws Exception {
    StopWatch sw = StopWatch.createStarted();
    List<String> processArgs = Arrays.asList(
        TestUtils.findBinary("yb-ts-cli"),
        "--server_address",
        "<PLACEHOLDER>",
        "set_flag",
        "<PLACEHOLDER>",
        "<PLACEHOLDER>");
    for (HostAndPort server : miniCluster.getTabletServers().keySet()) {
      processArgs.set(2, server.toString());
      for (Entry<String, String> entry : config.getGflags().entrySet()) {
        processArgs.set(4, entry.getKey());
        processArgs.set(5, entry.getValue());
        runProcess(processArgs);
      }
    }
    LOG.info("Audit config applied in " + sw.getTime() + " ms");
  }

  private void assertAuditRecords(List<AuditLogEntry> expected, List<AuditLogEntry> actual) {
    if (!Objects.equals(expected, actual)) {
      fail(String.format("expected:<\n  %s\n> but was:<\n  %s\n>",
          StringUtils.joinWith("\n  ", expected.toArray()),
          StringUtils.joinWith("\n  ", actual.toArray())));
    }
  }

  /**
   * Helper class for audit gflag configuration.
   * <p>
   * Null values mean that gflag should be removed.
   */
  private static class AuditConfig {
    public boolean enabled;
    public String logLevel;
    public String includedKeyspaces;
    public String excludedKeyspaces;
    public String includedCategories;
    public String excludedCategories;
    public String includedUsers;
    public String excludedUsers;

    public Map<String, String> getGflags() {
      Map<String, String> map = new LinkedHashMap<>();
      map.put("ycql_enable_audit_log", Boolean.toString(enabled));
      map.put("ycql_audit_log_level", logLevel);
      map.put("ycql_audit_included_keyspaces", includedKeyspaces);
      map.put("ycql_audit_excluded_keyspaces", excludedKeyspaces);
      map.put("ycql_audit_included_categories", includedCategories);
      map.put("ycql_audit_excluded_categories", excludedCategories);
      map.put("ycql_audit_included_users", includedUsers);
      map.put("ycql_audit_excluded_users", excludedUsers);
      map.values().removeIf(v -> v == null);
      return map;
    }
  }

  private class AuditLogListener implements LogErrorListener {
    private final String PREFIX = "AUDIT: ";

    public final int tserverPort;

    public AuditLogListener(int tserverPort) {
      this.tserverPort = tserverPort;
    }

    @Override
    public void associateWithLogPrinter(LogPrinter printer) {
      // NOOP
    }

    @Override
    public void reportErrorsAtEnd() {
      // NOOP
    }

    @Override
    public void handleLine(String line) {
      int idx = line.indexOf(PREFIX);
      if (idx == -1) {
        return;
      }

      AuditLogEntry entry = new AuditLogEntry();
      entry.tserverPort = tserverPort;
      entry.logLevel = line.charAt(0);

      // This would slip if an operation contains "|" character but we just won't issue those.
      String rest = line.substring(idx + PREFIX.length());
      for (String part : rest.split("\\|")) {
        String[] partsOfPart = part.split(":", 2);
        switch (partsOfPart[0]) {
          case "user":
            entry.user = partsOfPart[1];
            break;
          case "host":
            entry.host = partsOfPart[1];
            break;
          case "source":
            entry.source = partsOfPart[1];
            break;
          case "port":
            entry.source += ":" + partsOfPart[1];
            break;
          case "timestamp":
            entry.timestamp = partsOfPart[1];
            break;
          case "type":
            entry.type = partsOfPart[1];
            break;
          case "category":
            entry.category = partsOfPart[1];
            break;
          case "batch":
            entry.batchId = partsOfPart[1];
            break;
          case "ks":
            entry.keyspace = partsOfPart[1];
            break;
          case "scope":
            entry.scope = partsOfPart[1];
            break;
          case "operation":
            entry.operationAndErrorMessage = partsOfPart[1];
            break;
          default:
            throw new IllegalStateException("Unexpected prefix " + partsOfPart[0]
                + ", line: " + line);
        }
      }
      auditRecords.add(entry);
    }
  }

  private static class AuditLogEntry {
    public Character logLevel;
    public int tserverPort;
    public String user;
    public String host;
    public String source;
    public String timestamp;
    public String type;
    public String category;
    public String batchId;
    public String keyspace;
    public String scope;
    public String operationAndErrorMessage;

    public AuditLogEntry() {
    }

    // Auto-generated boilerplate.
    // (Ignores tserverPort, host, source and timestamp.)

    public AuditLogEntry(Character logLevel, String user, String type, String category,
        String batchId, String keyspace, String scope, String operationAndErrorMessage) {
      this.logLevel = logLevel;
      this.user = user;
      this.type = type;
      this.category = category;
      this.batchId = batchId;
      this.keyspace = keyspace;
      this.scope = scope;
      this.operationAndErrorMessage = operationAndErrorMessage;
    }

    @Override
    public int hashCode() {
      return Objects.hash(batchId, category, keyspace, logLevel, operationAndErrorMessage, scope,
          type, user);
    }

    @Override
    public boolean equals(Object obj) {
      if (this == obj) return true;
      if (obj == null) return false;
      if (getClass() != obj.getClass()) return false;
      AuditLogEntry other = (AuditLogEntry) obj;
      return Objects.equals(batchId, other.batchId) && Objects.equals(category, other.category)
          && Objects.equals(keyspace, other.keyspace) && Objects.equals(logLevel, other.logLevel)
          && Objects.equals(operationAndErrorMessage, other.operationAndErrorMessage)
          && Objects.equals(scope, other.scope) && Objects.equals(type, other.type)
          && Objects.equals(user, other.user);
    }

    @Override
    public String toString() {
      return "AuditLogEntry [logLevel=" + logLevel + ", user=" + user + ", type=" + type
          + ", category=" + category + ", batchId=" + batchId + ", keyspace=" + keyspace
          + ", scope=" + scope + ", operationAndErrorMessage=" + operationAndErrorMessage + "]";
    }
  }
}
