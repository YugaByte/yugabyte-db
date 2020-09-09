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

import com.datastax.driver.core.*;
import org.junit.After;
import org.junit.Before;
import org.junit.BeforeClass;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

import com.datastax.driver.core.Cluster;
import com.datastax.driver.core.ConsistencyLevel;
import com.datastax.driver.core.QueryOptions;
import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import com.datastax.driver.core.SimpleStatement;
import com.datastax.driver.core.Statement;
import com.datastax.driver.core.Session;
import com.datastax.driver.core.SocketOptions;
import com.datastax.driver.core.exceptions.QueryValidationException;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.yb.client.YBClient;
import org.yb.minicluster.BaseMiniClusterTest;
import org.yb.minicluster.Metrics;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.minicluster.MiniYBDaemon;
import org.yb.minicluster.RocksDBMetrics;

import java.io.Closeable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Collectors;

public class BaseCQLTest extends BaseMiniClusterTest {
  protected static final Logger LOG = LoggerFactory.getLogger(BaseCQLTest.class);

  // Integer.MAX_VALUE seconds is the maximum allowed TTL by Cassandra.
  protected static final int MAX_TTL_SEC = Integer.MAX_VALUE;

  protected static final String DEFAULT_TEST_KEYSPACE = "cql_test_keyspace";

  protected static final String TSERVER_READ_METRIC =
    "handler_latency_yb_tserver_TabletServerService_Read";

  protected static final String TSERVER_SELECT_METRIC =
      "handler_latency_yb_cqlserver_SQLProcessor_SelectStmt";

  protected static final String TSERVER_FLUSHES_METRIC =
      "handler_latency_yb_cqlserver_SQLProcessor_NumFlushesToExecute";

  // CQL and Redis settings.
  protected static boolean startCqlProxy = true;
  protected static boolean startRedisProxy = false;
  protected static int systemQueryCacheMsecs = 4000;
  protected static boolean systemQueryCacheEmptyResponses = false;

  protected Cluster cluster;
  protected Session session;

  float float_infinity_positive = createFloat(0, 0b11111111, 0);
  float float_infinity_negative = createFloat(1, 0b11111111, 0);
  float float_nan_0 = createFloat(0, 0b11111111, 1);
  float float_nan_1 = createFloat(1, 0b11111111, 1);
  float float_nan_2 = createFloat(0, 0b11111111, 0b10);
  float float_zero_positive = createFloat(0, 0, 0);
  float float_zero_negative = createFloat(1, 0, 0);
  float float_sub_normal = createFloat(0, 0, 1);

  double double_infinity_positive = createDouble(0, 0b11111111111, 0);
  double double_infinity_negative = createDouble(1, 0b11111111111, 0);
  double double_nan_0 = createDouble(0, 0b011111111111, 1);
  double double_nan_1 = createDouble(0, 0b111111111111, 1);
  double double_nan_2 = createDouble(0, 0b011111111111, 0b10);
  double double_zero_positive = createDouble(0, 0, 0);
  double double_zero_negative = createDouble(1, 0, 0);
  double double_sub_normal = createDouble(0, 0, 1);

  float[] float_all_literals = { float_infinity_positive, float_infinity_negative, float_nan_0,
      float_nan_1, float_nan_2, float_zero_positive, float_zero_negative, float_sub_normal };
  double[] double_all_literals = { double_infinity_positive, double_infinity_negative, double_nan_0,
      double_nan_1, double_nan_2, double_zero_positive, double_zero_negative, double_sub_normal };

  public float createFloat(int sign, int exp, int fraction) {
    return Float.intBitsToFloat((sign << 31) | (exp << 23) | fraction);
  }

  public double createDouble(long sign, long exp, long fraction) {
    return Double.longBitsToDouble((sign << 63) | (exp << 52) | fraction);
  }

  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = new TreeMap<>();

    flagMap.put("start_cql_proxy", Boolean.toString(startCqlProxy));
    flagMap.put("start_redis_proxy", Boolean.toString(startRedisProxy));
    flagMap.put("cql_update_system_query_cache_msecs", Integer.toString(systemQueryCacheMsecs));
    flagMap.put("cql_system_query_cache_empty_responses",
        Boolean.toString(systemQueryCacheEmptyResponses));

    return flagMap;
  }

  protected Map<String, String> getMasterFlags() {
    return new TreeMap<>();
  }

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    for (Map.Entry<String, String> entry : getTServerFlags().entrySet()) {
      builder.addCommonTServerArgs("--" + entry.getKey() + "=" + entry.getValue());
    }
    for (Map.Entry<String, String> entry : getMasterFlags().entrySet()) {
      builder.addMasterArgs("--" + entry.getKey() + "=" + entry.getValue());
    }
    builder.enablePostgres(false);
  }

  @BeforeClass
  public static void setUpBeforeClass() throws Exception {
    LOG.info("BaseCQLTest.setUpBeforeClass is running");
    BaseMiniClusterTest.tserverArgs.add("--client_read_write_timeout_ms=180000");
    // Disable extended peer check, to ensure "SELECT * FROM system.peers" works without
    // all columns.
    System.setProperty("com.datastax.driver.EXTENDED_PEER_CHECK", "false");
  }

  public Cluster.Builder getDefaultClusterBuilder() {
    // Set default consistency level to strong consistency
    QueryOptions queryOptions = new QueryOptions();
    queryOptions.setConsistencyLevel(ConsistencyLevel.YB_STRONG);
    // Set a long timeout for CQL queries since build servers might be really slow (especially Mac
    // Mini).
    SocketOptions socketOptions = new SocketOptions();
    socketOptions.setReadTimeoutMillis(120 * 1000);
    socketOptions.setConnectTimeoutMillis(120 * 1000);
    return Cluster.builder()
              .addContactPointsWithPorts(miniCluster.getCQLContactPoints())
              .withQueryOptions(queryOptions)
              .withSocketOptions(socketOptions);
  }

  public Session buildDefaultSession(Cluster cluster) {
    return cluster.connect();
  }

  @Before
  public void setUpCqlClient() throws Exception {
    LOG.info("BaseCQLTest.setUpCqlClient is running");

    if (miniCluster == null) {
      final String errorMsg =
          "Mini-cluster must already be running by the time setUpCqlClient is invoked";
      LOG.error(errorMsg);
      throw new RuntimeException(errorMsg);
    }
    try {
      cluster = getDefaultClusterBuilder().build();
    } catch (Exception ex) {
      LOG.error("Error while setting up a CQL client", ex);
      throw ex;
    }
    LOG.info("Connected to cluster: " + cluster.getMetadata().getClusterName());
    session = buildDefaultSession(cluster);

    final int numTServers = miniCluster.getTabletServers().size();
    final int expectedNumPeers = Math.max(0, Math.min(numTServers - 1, 2));
    LOG.info("Waiting until system.peers contains at least " + expectedNumPeers + " entries (" +
        "number of tablet servers: " + numTServers + ")");
    int attemptsMade = 0;
    boolean waitSuccessful = false;

    while (attemptsMade < 30) {
      int numPeers = 0;
      for (Row row : execute("select peer from system.peers")) {
        numPeers++;
      }
      if (numPeers >= expectedNumPeers) {
        waitSuccessful = true;
        break;
      }
      LOG.info("system.peers still contains only " + numPeers + " entries, waiting");
      Thread.sleep(1000);
    }
    if (waitSuccessful) {
      LOG.info("Succeeded waiting for " + expectedNumPeers + " peers to show up in system.peers");
    } else {
      LOG.warn("Timed out waiting for " + expectedNumPeers + " peers to show up in system.peers");
    }

    // Create and use test keyspace to be able to use short table names later.
    createKeyspaceIfNotExists(DEFAULT_TEST_KEYSPACE);
    useKeyspace();
  }

  public void useKeyspace() throws Exception{
    useKeyspace(DEFAULT_TEST_KEYSPACE);
  }

  private static void closeIfNotNull(String logPrefix,
                                     String clusterOrSessionStr,
                                     Closeable closeable) throws Exception {
    if (closeable != null) {
      LOG.info(logPrefix + "closing " + clusterOrSessionStr);
      try {
        closeable.close();
      } catch (IOException ex) {
        LOG.error(logPrefix + ": exception while trying to close " + clusterOrSessionStr, ex);
        throw ex;
      }
    } else {
      LOG.info(logPrefix + clusterOrSessionStr + " is already null, nothing to close");
    }
  }

  @After
  public void tearDownAfter() throws Exception {
    final String logPrefix = "BaseCQLTest.tearDownAfter: ";
    LOG.info(logPrefix + "End test: " + getCurrentTestMethodName());

    if (miniCluster == null) {
      LOG.info(logPrefix + "mini cluster has been destroyed");
      return;
    }

    LOG.info(logPrefix + "dropping tables / types / keyspaces");

    // Clean up all tables before end of each test to lower high-water-mark disk usage.
    dropTables();

    // Also delete all types to make sure we can delete keyspaces below.
    dropUDTypes();

    // Also delete all keyspaces, because we may
    dropKeyspaces();

    closeIfNotNull(logPrefix, "session", session);
    // Cannot move this check to closeIfNotNull, because isClosed() is not part of Closeable.
    if (session != null && !session.isClosed()) {
      LOG.warn(logPrefix + "session is still not closed!");
    }
    session = null;

    closeIfNotNull(logPrefix, "cluster", cluster);
    // See the comment above.
    if (cluster  != null && !cluster.isClosed()) {
      LOG.warn(logPrefix + "cluster is still not closed!");
    }
    cluster = null;

    LOG.info(logPrefix +
        "finished attempting to drop tables and trying to close CQL session/client");
    afterBaseCQLTestTearDown();
  }

  protected void afterBaseCQLTestTearDown() throws Exception {
  }

  protected void createTable(String tableName, String columnType) throws Exception {
    final long startTimeMs = System.currentTimeMillis();
    LOG.info("CREATE TABLE " + tableName);
    String create_stmt = String.format("CREATE TABLE %s " +
                    " (h1 int, h2 %2$s, " +
                    " r1 int, r2 %2$s, " +
                    " v1 int, v2 %2$s, " +
                    " primary key((h1, h2), r1, r2));",
            tableName, columnType);
    session.execute(create_stmt);
    long elapsedTimeMs = System.currentTimeMillis() - startTimeMs;
    LOG.info("Table creation took " + elapsedTimeMs + " for table " + tableName +
        " with column type " + columnType);
  }

  protected void createTable(String tableName) throws Exception {
    createTable(tableName, "varchar");
  }

  public void setupTable(String tableName, int numRows) throws Exception {
    createTable(tableName);

    if (numRows <= 0) {
      LOG.info("No rows to create in setupTable (numRows=" + numRows + ")");
      return;
    }

    LOG.info("INSERT INTO TABLE " + tableName);
    final long startTimeMs = System.currentTimeMillis();
    final AtomicLong lastLogTimeMs = new AtomicLong(startTimeMs);
    final int LOG_EVERY_MS = 5 * 1000;
    // INSERT: A valid statement with a column list.
    final PreparedStatement preparedStmt = session.prepare(
        "INSERT INTO " + tableName + "(h1, h2, r1, r2, v1, v2) VALUES(" +
            "?, ?, ?, ?, ?, ?);");

    final int numThreads = 4;
    ExecutorService executor = Executors.newFixedThreadPool(numThreads);
    final AtomicInteger currentRowIndex = new AtomicInteger();
    final AtomicInteger numInsertedRows = new AtomicInteger();
    List<Future<Void>> futures = new ArrayList<>();
    for (int i = 0; i < numThreads; ++i) {
      Callable<Void> task =
          () -> {
            int idx;
            while ((idx = currentRowIndex.get()) < numRows) {
              if (!currentRowIndex.compareAndSet(idx, idx + 1)) {
                // Not using incrementAndGet because we don't want currentRowIndex to become higher
                // than numRows.
                continue;
              }
              session.execute(preparedStmt.bind(
                  idx, "h" + idx, idx + 100, "r" + (idx + 100), idx + 1000, "v" + (idx + 1000)));
              numInsertedRows.incrementAndGet();
              if (idx % 10 == 0) {
                long currentTimeMs;
                long currentLastLogTimeMs;
                while ((currentTimeMs = System.currentTimeMillis()) -
                       (currentLastLogTimeMs = lastLogTimeMs.get()) > LOG_EVERY_MS) {
                  if (!lastLogTimeMs.compareAndSet(currentLastLogTimeMs, currentTimeMs)) {
                    // Someone updated lastLogTimeMs concurrently.
                    continue;
                  }
                  final int numRowsInsertedSoFar = numInsertedRows.get();
                  LOG.info("Inserted " + numRowsInsertedSoFar + " rows out of " + numRows + " (" +
                      String.format(
                          "%.2f", numRowsInsertedSoFar * 1000.0 / (currentTimeMs - startTimeMs)) +
                      " rows/second on average)."
                  );
                }
              }
            }
            return null;
          };
      futures.add(executor.submit(task));
    }

    // Wait for all writer threads to complete.
    for (Future<Void> future : futures) {
      future.get();
    }
    executor.shutdown();
    executor.awaitTermination(10, TimeUnit.SECONDS);

    final long totalTimeMs = System.currentTimeMillis() - startTimeMs;
    assertEquals(numRows, currentRowIndex.get());
    assertEquals(numRows, numInsertedRows.get());
    LOG.info("INSERT INTO TABLE " + tableName + " FINISHED: inserted " + numInsertedRows.get() +
             " rows (total time: " + totalTimeMs + " ms, " +
             String.format("%.2f", numRows * 1000.0 / totalTimeMs) + " rows/sec)");
  }

  public void setupTable(String tableName, int []ttls) throws Exception {
    LOG.info("CREATE TABLE " + tableName);
    String create_stmt = String.format("CREATE TABLE %s " +
                    " (h1 int, h2 %2$s, " +
                    " r1 int, r2 %2$s, " +
                    " v1 int, v2 int, " +
                    " primary key((h1, h2), r1, r2));",
            tableName, "varchar");
    session.execute(create_stmt);

    LOG.info("INSERT INTO TABLE " + tableName);
    for (int idx = 0; idx < ttls.length; idx++) {
      // INSERT: Valid statement with column list.
      String insert_stmt = String.format(
        "INSERT INTO %s(h1, h2, r1, r2, v1, v2) VALUES(%d, 'h%d', %d, 'r%d', %d, %d) using ttl %d;",
        tableName, idx, idx, idx+100, idx+100, idx+1000, idx+1000, ttls[idx]);
      session.execute(insert_stmt);
    }
    LOG.info("INSERT INTO TABLE " + tableName + " FINISHED");
  }

  protected void dropIndex(String indexName) throws Exception {
    String dropStmt = String.format("DROP INDEX %s;", indexName);
    session.execute(dropStmt);
  }

  protected void dropTable(String tableName) throws Exception {
    String dropStmt = String.format("DROP TABLE %s;", tableName);
    LOG.info("Executing drop table: " + dropStmt);
    session.execute(dropStmt);
  }

  protected void dropUDType(String keyspaceName, String typeName) throws Exception {
    String drop_stmt = String.format("DROP TYPE %s.%s;", keyspaceName, typeName);
    session.execute(drop_stmt);
  }

  protected boolean IsSystemKeyspace(String keyspaceName) {
    return keyspaceName.equals("system") ||
           keyspaceName.equals("system_auth") ||
           keyspaceName.equals("system_distributed") ||
           keyspaceName.equals("system_traces") ||
           keyspaceName.equals("system_schema");
  }

  private boolean IsRedisKeyspace(String keyspaceName) {
    return keyspaceName.equals(YBClient.REDIS_KEYSPACE_NAME);
  }

  protected void dropTables() throws Exception {
    if (cluster == null) {
      return;
    }
    for (Row row : session.execute("SELECT keyspace_name, table_name FROM system_schema.tables")) {
      if (!IsSystemKeyspace(row.getString("keyspace_name"))) {
        dropTable(row.getString("keyspace_name") + "." + row.getString("table_name"));
      }
    }
  }

  protected void dropUDTypes() throws Exception {
    if (cluster == null) {
      return;
    }
    for (Row row : session.execute("SELECT keyspace_name, type_name FROM system_schema.types")) {
      if (!IsSystemKeyspace(row.getString("keyspace_name"))) {
        dropUDType(row.getString("keyspace_name"), row.getString("type_name"));
      }
    }
  }

  protected void dropKeyspaces() throws Exception {
    if (cluster == null) {
      return;
    }
    for (Row row : session.execute("SELECT keyspace_name FROM system_schema.keyspaces")) {
      if (!IsSystemKeyspace(row.getString("keyspace_name"))) {
        dropKeyspace(row.getString("keyspace_name"));
      }
    }
  }

  protected Iterator<Row> runSelect(String select_stmt) {
    ResultSet rs = session.execute(select_stmt);
    Iterator<Row> iter = rs.iterator();
    assertTrue(iter.hasNext());
    return iter;
  }

  protected Iterator<Row> runSelect(String select_stmt, Object... args) {
    return runSelect(String.format(select_stmt, args));
  }

  protected String runInvalidQuery(Statement stmt) {
    try {
      session.execute(stmt);
      fail(String.format("Statement did not fail: %s", stmt));
      return null; // Never happens, but keeps compiler happy
    } catch (QueryValidationException qv) {
      LOG.info("Expected exception", qv);
      return qv.getCause().getMessage();
    }
  }

  protected String runInvalidQuery(String stmt) {
    return runInvalidQuery(new SimpleStatement(stmt));
  }

  protected String runInvalidStmt(Statement stmt, Session s) {
    try {
      s.execute(stmt);
      fail(String.format("Statement did not fail: %s", stmt));
      return null; // Never happens, but keeps compiler happy
    } catch (QueryValidationException qv) {
      LOG.info("Expected exception", qv);
      return qv.getCause().getMessage();
    }
  }

  protected String runValidSelect(String stmt) {
    ResultSet rs = session.execute(stmt);
    String result = "";
    for (Row row : rs) {
      result += row.toString();
    }
    return result;
  }

  protected String runInvalidStmt(Statement stmt) {
    return runInvalidStmt(stmt, session);
  }

  protected String runInvalidStmt(String stmt, Session s) {
    return runInvalidStmt(new SimpleStatement(stmt), s);
  }

  protected String runInvalidStmt(String stmt) {
    return runInvalidStmt(stmt, session);
  }

  protected void runInvalidStmt(String stmt, String errorSubstring) {
    String errorMsg = runInvalidStmt(stmt);
    assertTrue("Error message '" + errorMsg + "' should contain '" + errorSubstring + "'",
               errorMsg.contains(errorSubstring));
  }

  protected void assertNoRow(String select_stmt) {
    ResultSet rs = session.execute(select_stmt);
    Iterator<Row> iter = rs.iterator();
    assertFalse(iter.hasNext());
  }

  protected void assertQuery(Statement stmt, String expectedResult) {
    ResultSet rs = session.execute(stmt);
    String actualResult = "";
    for (Row row : rs) {
      actualResult += row.toString();
    }
    assertEquals(expectedResult, actualResult);
  }

  protected void assertQuery(String stmt, String expectedResult) {
    assertQuery(new SimpleStatement(stmt), expectedResult);
  }

  protected void assertQuery(Statement stmt, String expectedColumns, String expectedResult) {
    ResultSet rs = session.execute(stmt);
    assertEquals(expectedColumns, rs.getColumnDefinitions().toString());
    String actualResult = "";
    for (Row row : rs) {
      actualResult += row.toString();
    }
    assertEquals(expectedResult, actualResult);
  }

  protected void assertQuery(String stmt, String expectedColumns, String expectedResult) {
    assertQuery(new SimpleStatement(stmt), expectedColumns, expectedResult);
  }

  protected void assertQuery(String stmt, Set<String> expectedRows) {
    assertQuery(new SimpleStatement(stmt), expectedRows);
  }

  protected void assertQuery(Statement stmt, Set<String> expectedRows) {
    ResultSet rs = session.execute(stmt);
    Set<String> actualRows = new HashSet<>();
    for (Row row : rs) {
      actualRows.add(row.toString());
    }
    assertEquals(expectedRows, actualRows);
  }

  /**
   * Assert the result of a query when the order of the rows is not enforced.
   * To be used, for instance, when querying over multiple hashes where the order is defined by the
   * hash function not just the values.
   *
   * @param stmt The (select) query to be executed
   * @param expectedRows the expected rows in no particular order
   */
  protected void assertQueryRowsUnordered(String stmt, String... expectedRows) {
    assertQuery(stmt, Arrays.stream(expectedRows).collect(Collectors.toSet()));
  }

  /**
   * Assert the result of a query when the order of the rows is enforced.
   * To be used, for instance, for queries where (range) column ordering (ASC/DESC) is being tested.
   *
   * @param stmt The (select) query to be executed
   * @param expectedRows the rows in the expected order
   */
  protected void assertQueryRowsOrdered(Statement stmt, List<String> expectedRows) {
    ResultSet rs = session.execute(stmt);
    List<String> actualRows = new ArrayList<String>();
    for (Row row : rs) {
      actualRows.add(row.toString());
    }
    assertEquals(expectedRows, actualRows);
  }

  /**
   * Assert the result of a query when the order of the rows is enforced.
   * To be used, for instance, for queries where (range) column ordering (ASC/DESC) is being tested.
   *
   * @param stmt The (select) query to be executed
   * @param expectedRows the rows in the expected order
   */
  protected void assertQueryRowsOrdered(String stmt, String... expectedRows) {
    assertQueryRowsOrdered(new SimpleStatement(stmt),
                           Arrays.stream(expectedRows).collect(Collectors.toList()));
  }

  /** Checks that the query yields an error containing the given (case-insensitive) substring */
  protected void assertQueryError(Statement stmt, String expectedErrorSubstring) {
    String result = runInvalidStmt(stmt);
    assertTrue("Query error '" + result + "' did not contain '" + expectedErrorSubstring + "'",
        result.toLowerCase().contains(expectedErrorSubstring.toLowerCase()));
  }

  protected void assertQueryError(String stmt, String expectedErrorSubstring) {
    assertQueryError(new SimpleStatement(stmt), expectedErrorSubstring);
  }

  // blob type utils
  protected String makeBlobString(ByteBuffer buf) {
    StringBuilder sb = new StringBuilder();
    char[] text_values = "0123456789abcdef".toCharArray();

    sb.append("0x");
    while (buf.hasRemaining()) {
      byte b = buf.get();
      sb.append(text_values[(b & 0xFF) >>> 4]);
      sb.append(text_values[b & 0x0F]);
    }
    return sb.toString();
  }

  ByteBuffer makeByteBuffer(long input) {
    ByteBuffer buffer = ByteBuffer.allocate(Long.BYTES);
    buffer.putLong(input);
    buffer.rewind();
    return buffer;
  }

  public ResultSet execute(String stmt, Session s) throws Exception {
    LOG.info("EXEC CQL: " + stmt);
    final ResultSet result = s.execute(stmt);
    LOG.info("EXEC CQL FINISHED: " + stmt);
    return result;
  }

  public ResultSet execute(String stmt) throws Exception { return execute(stmt, session); }

  public void executeInvalid(String stmt, Session s) throws Exception {
    LOG.info("EXEC INVALID CQL: " + stmt);
    runInvalidStmt(stmt, s);
    LOG.info("EXEC INVALID CQL FINISHED: " + stmt);
  }

  public void executeInvalid(String stmt) throws Exception { executeInvalid(stmt, session); }

  public void createKeyspace(String keyspaceName) throws Exception {
    String createKeyspaceStmt = "CREATE KEYSPACE \"" + keyspaceName + "\";";
    execute(createKeyspaceStmt);
  }

  public void createKeyspaceIfNotExists(String keyspaceName) throws Exception {
    String createKeyspaceStmt = "CREATE KEYSPACE IF NOT EXISTS \"" + keyspaceName + "\";";
    execute(createKeyspaceStmt);
  }

  public void useKeyspace(String keyspaceName) throws Exception {
    String useKeyspaceStmt = "USE \"" + keyspaceName + "\";";
    execute(useKeyspaceStmt);
  }

  public void dropKeyspace(String keyspaceName) throws Exception {
    String deleteKeyspaceStmt = "DROP KEYSPACE \"" + keyspaceName + "\";";
    execute(deleteKeyspaceStmt);
  }

  private String getTableUUID(String keyspaceName, String tableName)  throws Exception {
    return miniCluster.getClient().openTable(keyspaceName, tableName).getTableId();
  }

  public RocksDBMetrics getRocksDBMetric(String tableName) throws Exception {
    return getRocksDBMetricByTableUUID(getTableUUID(DEFAULT_TEST_KEYSPACE, tableName));
  }

  public int getTableCounterMetric(String keyspaceName,
                                   String tableName,
                                   String metricName) throws Exception {
    return getTableCounterMetricByTableUUID(getTableUUID(keyspaceName, tableName), metricName);
  }

  public int getRestartsCount(String tableName) throws Exception {
    return getTableCounterMetricByTableUUID(getTableUUID(DEFAULT_TEST_KEYSPACE, tableName),
                                "restart_read_requests");
  }

  public int getRetriesCount() throws Exception {
    int totalSum = 0;
    for (MiniYBDaemon ts : miniCluster.getTabletServers().values()) {
      totalSum += new Metrics(ts.getLocalhostIP(), ts.getCqlWebPort(), "server")
                  .getHistogram("handler_latency_yb_cqlserver_SQLProcessor_NumRetriesToExecute")
                  .totalSum;
    }
    return totalSum;
  }

}
