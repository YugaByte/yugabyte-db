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

import static com.google.common.base.Preconditions.checkNotNull;
import static org.yb.AssertionWrappers.*;
import static org.yb.util.SanitizerUtil.isASAN;
import static org.yb.util.SanitizerUtil.isTSAN;

import com.google.common.net.HostAndPort;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import org.apache.commons.lang3.StringUtils;
import org.junit.After;
import org.junit.AfterClass;
import org.junit.Before;
import org.postgresql.core.TransactionState;
import org.postgresql.jdbc.PgArray;
import org.postgresql.jdbc.PgConnection;
import org.postgresql.util.PGobject;
import org.postgresql.util.PSQLException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.AsyncYBClient;
import org.yb.client.IsInitDbDoneResponse;
import org.yb.client.TestUtils;
import org.yb.client.YBClient;
import org.yb.minicluster.*;
import org.yb.minicluster.Metrics.YSQLStat;
import org.yb.pgsql.cleaners.ClusterCleaner;
import org.yb.pgsql.cleaners.ConnectionCleaner;
import org.yb.pgsql.cleaners.UserObjectCleaner;
import org.yb.util.EnvAndSysPropertyUtil;
import org.yb.util.SanitizerUtil;
import org.yb.master.Master;

import java.io.File;
import java.net.InetSocketAddress;
import java.net.URL;
import java.net.URLConnection;
import java.sql.*;
import java.util.*;
import java.util.concurrent.ConcurrentSkipListSet;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Function;

public class BasePgSQLTest extends BaseMiniClusterTest {
  private static final Logger LOG = LoggerFactory.getLogger(BasePgSQLTest.class);

  // Postgres settings.
  protected static final String DEFAULT_PG_DATABASE = "yugabyte";
  protected static final String DEFAULT_PG_USER = "yugabyte";
  protected static final String DEFAULT_PG_PASS = "yugabyte";
  public static final String TEST_PG_USER = "yugabyte_test";

  // Non-standard PSQL states defined in yb_pg_errcodes.h
  protected static final String SERIALIZATION_FAILURE_PSQL_STATE = "40001";
  protected static final String SNAPSHOT_TOO_OLD_PSQL_STATE = "72000";

  // Postgres flags.
  private static final String MASTERS_FLAG = "FLAGS_pggate_master_addresses";
  private static final String PG_DATA_FLAG = "PGDATA";
  private static final String YB_ENABLED_IN_PG_ENV_VAR_NAME = "YB_ENABLED_IN_POSTGRES";

  // Metric names.
  protected static final String METRIC_PREFIX = "handler_latency_yb_ysqlserver_SQLProcessor_";
  protected static final String SELECT_STMT_METRIC = METRIC_PREFIX + "SelectStmt";
  protected static final String INSERT_STMT_METRIC = METRIC_PREFIX + "InsertStmt";
  protected static final String DELETE_STMT_METRIC = METRIC_PREFIX + "DeleteStmt";
  protected static final String UPDATE_STMT_METRIC = METRIC_PREFIX + "UpdateStmt";
  protected static final String OTHER_STMT_METRIC = METRIC_PREFIX + "OtherStmts";
  protected static final String TRANSACTIONS_METRIC = METRIC_PREFIX + "Transactions";
  protected static final String AGGREGATE_PUSHDOWNS_METRIC = METRIC_PREFIX + "AggregatePushdowns";

  // CQL and Redis settings.
  protected static boolean startCqlProxy = false;
  protected static boolean startRedisProxy = false;

  protected static Connection connection;

  protected File pgBinDir;

  protected static final int DEFAULT_STATEMENT_TIMEOUT_MS = 30000;

  protected static ConcurrentSkipListSet<Integer> stuckBackendPidsConcMap =
      new ConcurrentSkipListSet<>();

  protected static boolean pgInitialized = false;

  public void runPgRegressTest(String schedule, long maxRuntimeMillis) throws Exception {
    final int tserverIndex = 0;
    PgRegressRunner pgRegress = new PgRegressRunner(schedule,
        getPgHost(tserverIndex), getPgPort(tserverIndex), DEFAULT_PG_USER,
        maxRuntimeMillis);
    pgRegress.setEnvVars(getPgRegressEnvVars());
    pgRegress.start();
    pgRegress.stop();
  }

  public void runPgRegressTest(String schedule) throws Exception {
    // Run test without maximum time.
    runPgRegressTest(schedule, 0);
  }

  private static int getRetryableRpcSingleCallTimeoutMs() {
    if (TestUtils.isReleaseBuild()) {
      return 10000;
    } else if (TestUtils.IS_LINUX) {
      if (SanitizerUtil.isASAN()) {
        return 20000;
      } else if (SanitizerUtil.isTSAN()) {
        return 45000;
      } else {
        // Linux debug builds.
        return 15000;
      }
    } else {
      // We get a lot of timeouts in macOS debug builds.
      return 45000;
    }
  }

  public static void perfAssertLessThan(double time1, double time2) {
    if (TestUtils.isReleaseBuild()) {
      assertLessThan(time1, time2);
    }
  }

  public static void perfAssertEquals(double time1, double time2) {
    if (TestUtils.isReleaseBuild()) {
      assertLessThan(time1, time2 * 1.3);
      assertLessThan(time1 * 1.3, time2);
    }
  }

  protected static int getPerfMaxRuntime(int releaseRuntime,
                                         int debugRuntime,
                                         int asanRuntime,
                                         int tsanRuntime,
                                         int macRuntime) {
    if (TestUtils.isReleaseBuild()) {
      return releaseRuntime;
    } else if (TestUtils.IS_LINUX) {
      if (SanitizerUtil.isASAN()) {
        return asanRuntime;
      } else if (SanitizerUtil.isTSAN()) {
        return tsanRuntime;
      } else {
        // Linux debug builds.
        return debugRuntime;
      }
    } else {
      // We get a lot of timeouts in macOS debug builds.
      return macRuntime;
    }
  }

  protected Map<String, String> getMasterAndTServerFlags() {
    return new TreeMap<>();
  }

  protected Integer getYsqlPrefetchLimit() {
    return null;
  }

  protected Integer getYsqlRequestLimit() {
    return null;
  }

  /**
   * @return flags shared between tablet server and initdb
   */
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = new TreeMap<>();

    if (isTSAN() || isASAN()) {
      flagMap.put("pggate_rpc_timeout_secs", "120");
    }
    flagMap.put("start_cql_proxy", Boolean.toString(startCqlProxy));
    flagMap.put("start_redis_proxy", Boolean.toString(startRedisProxy));

    // Setup flag for postgres test on prefetch-limit when starting tserver.
    if (getYsqlPrefetchLimit() != null) {
      flagMap.put("ysql_prefetch_limit", getYsqlPrefetchLimit().toString());
    }

    if (getYsqlRequestLimit() != null) {
      flagMap.put("ysql_request_limit", getYsqlRequestLimit().toString());
    }

    flagMap.put("ysql_beta_features", "true");
    flagMap.put("ysql_sleep_before_retry_on_txn_conflict", "false");
    flagMap.put("ysql_max_write_restart_attempts", "2");

    return flagMap;
  }

  protected Map<String, String> getMasterFlags() {
    Map<String, String> flagMap = new TreeMap<>();
    flagMap.put("client_read_write_timeout_ms", "120000");
    flagMap.put("memory_limit_hard_bytes", String.valueOf(2L * 1024 * 1024 * 1024));
    return flagMap;
  }

  @Override
  protected int overridableNumShardsPerTServer() {
    return 1;
  }

  @Override
  protected int getReplicationFactor() {
    return 3;
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

    for (Map.Entry<String, String> entry : getMasterAndTServerFlags().entrySet()) {
      String flagStr = "--" + entry.getKey() + "=" + entry.getValue();
      builder.addCommonTServerArgs(flagStr);
      builder.addMasterArgs(flagStr);
    }
    builder.enablePostgres(true);
  }

  @Before
  public void initPostgresBefore() throws Exception {
    if (pgInitialized)
      return;

    LOG.info("Loading PostgreSQL JDBC driver");
    Class.forName("org.postgresql.Driver");

    // Postgres bin directory.
    pgBinDir = new File(TestUtils.getBuildRootDir(), "postgres/bin");

    LOG.info("Waiting for initdb to complete on master");
    TestUtils.waitFor(
        () -> {
          IsInitDbDoneResponse initdbStatusResp = miniCluster.getClient().getIsInitDbDone();
          if (initdbStatusResp.hasError()) {
            throw new RuntimeException(
                "Could not request initdb status: " + initdbStatusResp.getServerError());
          }
          String initdbError = initdbStatusResp.getInitDbError();
          if (initdbError != null && !initdbError.isEmpty()) {
            throw new RuntimeException("initdb failed: " + initdbError);
          }
          return initdbStatusResp.isDone();
        },
        600000);
    LOG.info("initdb has completed successfully on master");

    if (connection != null) {
      LOG.info("Closing previous connection");
      connection.close();
      connection = null;
    }

    // Create test role.
    try (Connection initialConnection = getConnectionBuilder().withUser(DEFAULT_PG_USER).connect();
         Statement statement = initialConnection.createStatement()) {
      statement.execute(
          "CREATE ROLE " + TEST_PG_USER + " SUPERUSER CREATEROLE CREATEDB BYPASSRLS LOGIN");
    }

    connection = getConnectionBuilder().connect();
    pgInitialized = true;
  }

  static ConnectionBuilder getConnectionBuilder() {
    return new ConnectionBuilder(miniCluster);
  }

  public String getPgHost(int tserverIndex) {
    return miniCluster.getPostgresContactPoints().get(tserverIndex).getHostName();
  }

  public int getPgPort(int tserverIndex) {
    return miniCluster.getPostgresContactPoints().get(tserverIndex).getPort();
  }

  protected Map<String, String> getPgRegressEnvVars() {
    Map<String, String> pgRegressEnvVars = new TreeMap<>();
    pgRegressEnvVars.put(MASTERS_FLAG, masterAddresses);
    pgRegressEnvVars.put(YB_ENABLED_IN_PG_ENV_VAR_NAME, "1");

    for (Map.Entry<String, String> entry : System.getenv().entrySet()) {
      String envVarName = entry.getKey();
      if (envVarName.startsWith("postgres_FLAGS_")) {
        String downstreamEnvVarName = envVarName.substring(9);
        LOG.info("Found env var " + envVarName + ", setting " + downstreamEnvVarName + " for " +
                 "pg_regress to " + entry.getValue());
        pgRegressEnvVars.put(downstreamEnvVarName, entry.getValue());
      }
    }

    // A temporary workaround for a failure to look up a user name by uid in an LDAP environment.
    pgRegressEnvVars.put("YB_PG_FALLBACK_SYSTEM_USER_NAME", "yugabyte");

    return pgRegressEnvVars;
  }

  /**
   * Register default post-test cleaners, executed in order.
   * When overridden, new cleaners should be added to the front of the list.
   */
  protected List<ClusterCleaner> getCleaners() {
    List<ClusterCleaner> cleaners = new ArrayList<>();
    cleaners.add(new ConnectionCleaner());
    cleaners.add(new UserObjectCleaner());
    return cleaners;
  }

  @After
  public void cleanUpAfter() throws Exception {
    if (connection == null) {
      LOG.warn("No connection created, skipping cleanup");
      return;
    }

    // If root connection was closed, open a new one for cleaning.
    if (connection.isClosed()) {
      connection = getConnectionBuilder().connect();
    }

    // Run cleaners in order.
    for (ClusterCleaner cleaner : getCleaners()) {
      cleaner.clean(connection);
    }
  }

  @AfterClass
  public static void tearDownAfter() throws Exception {
    // Close the root connection, which is not cleaned up after each test.
    if (connection != null && !connection.isClosed()) {
      connection.close();
    }
    pgInitialized = false;
    LOG.info("Destroying mini-cluster");
    if (miniCluster != null) {
      destroyMiniCluster();
      miniCluster = null;
    }
  }

  /**
   * Commit the current transaction on the given connection, catch and report the exception.
   * @param conn connection to use
   * @param extraMsg an extra part of the error message
   * @return whether commit succeeded
   */
  protected static boolean commitAndCatchException(Connection conn, String extraMsg) {
    extraMsg = extraMsg.trim();
    if (!extraMsg.isEmpty()) {
      extraMsg = " (" + extraMsg + ")";
    }
    try {
      conn.commit();
      return true;
    } catch (SQLException ex) {
      // TODO: validate the exception message.
      LOG.info("Error during commit" + extraMsg + ": " + ex.getMessage());
      return false;
    }
  }

  protected static PgConnection toPgConnection(Connection connection) {
    return (PgConnection) connection;
  }

  protected static TransactionState getPgTxnState(Connection connection) {
    return toPgConnection(connection).getTransactionState();
  }

  protected static int getPgBackendPid(Connection connection) {
    return toPgConnection(connection).getBackendPID();
  }

  protected static class AgregatedValue {
    long count;
    double value;
    long rows;
  }

  protected void resetStatementStat() throws Exception {
    for (MiniYBDaemon ts : miniCluster.getTabletServers().values()) {
      URL url = new URL(String.format("http://%s:%d/statements-reset",
                                      ts.getLocalhostIP(),
                                      ts.getPgsqlWebPort()));
      Scanner scanner = new Scanner(url.openConnection().getInputStream());
      scanner.close();
    }
  }

  protected AgregatedValue getStatementStat(String statName) throws Exception {
    AgregatedValue value = new AgregatedValue();
    for (MiniYBDaemon ts : miniCluster.getTabletServers().values()) {
      URL url = new URL(String.format("http://%s:%d/statements",
                                      ts.getLocalhostIP(),
                                      ts.getPgsqlWebPort()));
      Scanner scanner = new Scanner(url.openConnection().getInputStream());
      JsonParser parser = new JsonParser();
      JsonElement tree = parser.parse(scanner.useDelimiter("\\A").next());
      JsonObject obj = tree.getAsJsonObject();
      YSQLStat ysqlStat = new Metrics(obj, true).getYSQLStat(statName);
      if (ysqlStat != null) {
        value.count += ysqlStat.calls;
        value.value += ysqlStat.total_time;
        value.rows += ysqlStat.rows;
      }
      scanner.close();
    }
    return value;
  }

  protected void verifyStatementStat(Statement stmt, String sql, String statName,
                                     int stmtMetricDelta, boolean validStmt) throws Exception {
    long oldValue = 0;
    if (statName != null) {
      oldValue = getStatementStat(statName).count;
    }

    if (validStmt) {
      stmt.execute(sql);
    } else {
      runInvalidQuery(stmt, sql, "ERROR");
    }

    long newValue = 0;
    if (statName != null) {
      newValue = getStatementStat(statName).count;
    }

    assertEquals(oldValue + stmtMetricDelta, newValue);
  }

  protected void verifyStatementStats(Statement stmt, String sql, String statName,
                                      long numLoops, long oldValue) throws Exception {
    for (int i = 0; i < numLoops; i++) {
      stmt.execute(sql);
    }

    long newValue = getStatementStat(statName).count;
    assertEquals(oldValue + numLoops, newValue);
  }

  protected void verifyStatementStatWithReset(Statement stmt, String sql, String statName,
                                              long numLoopsBeforeReset, long numLoopsAfterReset)
                                                throws Exception {
    long oldValue = getStatementStat(statName).count;
    verifyStatementStats(stmt, sql, statName, numLoopsBeforeReset, oldValue);

    resetStatementStat();

    oldValue = 0;
    verifyStatementStats(stmt, sql, statName, numLoopsAfterReset, oldValue);
  }

  private JsonArray[] getRawMetric(
      Function<MiniYBDaemon, Integer> portFetcher) throws Exception {
    Collection<MiniYBDaemon> servers = miniCluster.getTabletServers().values();
    JsonArray[] result = new JsonArray[servers.size()];
    int index = 0;
    for (MiniYBDaemon ts : servers) {
      URLConnection connection = new URL(String.format("http://%s:%d/metrics",
          ts.getLocalhostIP(),
          portFetcher.apply(ts))).openConnection();
      connection.setUseCaches(false);
      Scanner scanner = new Scanner(connection.getInputStream());
      result[index++] =
          new JsonParser().parse(scanner.useDelimiter("\\A").next()).getAsJsonArray();
      scanner.close();
    }
    return result;
  }

  protected JsonArray[] getRawTSMetric() throws Exception {
    return getRawMetric((ts) -> ts.getWebPort());
  }

  protected JsonArray[] getRawYSQLMetric() throws Exception {
    return getRawMetric((ts) -> ts.getPgsqlWebPort());
  }

  protected AgregatedValue getMetric(String metricName) throws Exception {
    AgregatedValue value = new AgregatedValue();
    for (JsonArray rawMetric : getRawYSQLMetric()) {
      JsonObject obj = rawMetric.get(0).getAsJsonObject();
      assertEquals(obj.get("type").getAsString(), "server");
      assertEquals(obj.get("id").getAsString(), "yb.ysqlserver");
      Metrics.YSQLMetric metric = new Metrics(obj).getYSQLMetric(metricName);
      value.count += metric.count;
      value.value += metric.sum;
      value.rows += metric.rows;
    }
    return value;
  }

  protected Long getTserverMetricCountForTable(String metricName, String tableName)
      throws Exception {
    long count = 0;
    for (JsonArray rawMetric : getRawTSMetric()) {
      for (JsonElement elem : rawMetric.getAsJsonArray()) {
        JsonObject obj = elem.getAsJsonObject();
        if (obj.get("type").getAsString().equals("tablet") &&
            obj.getAsJsonObject("attributes").get("table_name").getAsString().equals(tableName)) {
          for (JsonElement subelem : obj.getAsJsonArray("metrics")) {
            if (!subelem.isJsonObject()) {
              continue;
            }
            JsonObject metric = subelem.getAsJsonObject();
            if (metric.has("name") && metric.get("name").getAsString().equals(metricName)) {
              count += metric.get("value").getAsLong();
            }
          }
        }
      }
    }
    return count;
  }

  protected long getMetricCounter(String metricName) throws Exception {
    return getMetric(metricName).count;
  }

  private interface MetricFetcher {
    AgregatedValue fetch(String name) throws Exception;
  }

  private static abstract class QueryExecutionMetricChecker {
    private MetricFetcher fetcher;
    private String metricName;
    private AgregatedValue oldValue;

    public QueryExecutionMetricChecker(String metricName, MetricFetcher fetcher) {
      this.fetcher = fetcher;
      this.metricName = metricName;
    }

    public void beforeQueryExecution() throws Exception {
      oldValue = fetcher.fetch(metricName);
    }

    public void afterQueryExecution(String query) throws Exception {
      check(query, metricName, oldValue, fetcher.fetch(metricName));
    }

    protected abstract void check(
      String query, String metricName, AgregatedValue oldValue, AgregatedValue newValue);
  }

  private class MetricCountChecker extends QueryExecutionMetricChecker {
    private long countDelta;

    public MetricCountChecker(String name, MetricFetcher fetcher, long countDelta) {
      super(name, fetcher);
      this.countDelta = countDelta;
    }

    @Override
    public void check(
      String query, String metric, AgregatedValue oldValue, AgregatedValue newValue) {
      assertEquals(
        String.format("'%s' count delta assertion failed for query '%s'", metric, query),
        countDelta, newValue.count - oldValue.count);
    }
  }

  private class MetricRowsChecker extends MetricCountChecker {
    private long rowsDelta;

    public MetricRowsChecker(String name, MetricFetcher fetcher, long countDelta, long rowsDelta) {
      super(name, fetcher, countDelta);
      this.rowsDelta = rowsDelta;
    }

    @Override
    public void check(
      String query, String metric, AgregatedValue oldValue, AgregatedValue newValue) {
      super.check(query, metric, oldValue, newValue);
      assertEquals(
        String.format("'%s' row count delta assertion failed for query '%s'", metric, query),
        rowsDelta, newValue.rows - oldValue.rows);
    }
  }

  /** Time execution of a query. */
  private long verifyQuery(Statement statement,
                           String query,
                           boolean validStmt,
                           QueryExecutionMetricChecker... checkers) throws Exception {
    for (QueryExecutionMetricChecker checker : checkers) {
      checker.beforeQueryExecution();
    }
    final long startTimeMillis = System.currentTimeMillis();
    if (validStmt) {
      statement.execute(query);
    } else {
      runInvalidQuery(statement, query, "ERROR");
    }
    // Check the elapsed time.
    final long result = System.currentTimeMillis() - startTimeMillis;
    for (QueryExecutionMetricChecker checker : checkers) {
      checker.afterQueryExecution(query);
    }
    return result;
  }

  /** Time execution of a query. */
  protected long verifyStatementMetric(
    Statement statement, String query, String metricName,
    int queryMetricDelta, int txnMetricDelta, boolean validStmt) throws Exception {
    return verifyQuery(
      statement, query, validStmt,
      new MetricCountChecker(TRANSACTIONS_METRIC, this::getMetric, txnMetricDelta),
      new MetricCountChecker(metricName, this::getMetric, queryMetricDelta));
  }

  protected void verifyStatementTxnMetric(
    Statement statement, String query, int txnMetricDelta) throws Exception {
    verifyQuery(
      statement, query,true,
      new MetricCountChecker(TRANSACTIONS_METRIC, this::getMetric, txnMetricDelta));
  }

  protected void verifyStatementMetricRows(
    Statement statement, String query, String metricName,
    int countDelta, int rowsDelta) throws Exception {
    verifyQuery(statement, query, true,
      new MetricRowsChecker(metricName, this::getMetric, countDelta, rowsDelta));
  }

  protected void executeWithTimeout(Statement statement, String sql)
      throws SQLException, TimeoutException, InterruptedException {
    // Maintain our map saying how many statements are being run by each backend pid.
    // Later we can determine (possibly) stuck backends based on this.
    final int backendPid = getPgBackendPid(statement.getConnection());

    AtomicReference<SQLException> sqlExceptionWrapper = new AtomicReference<>();
    boolean timedOut = false;
    try {
      String taskDescription = "SQL statement (PG backend pid: " + backendPid + "): " + sql;
      runWithTimeout(DEFAULT_STATEMENT_TIMEOUT_MS, taskDescription, () -> {
        try {
          statement.execute(sql);
        } catch (SQLException e) {
          sqlExceptionWrapper.set(e);
        }
      });
    } catch (TimeoutException ex) {
      // Record that this backend is possibly "stuck" so we can force a core dump and examine it.
      stuckBackendPidsConcMap.add(backendPid);
      timedOut = true;
      throw ex;
    } finally {
      // Make sure we propagate the SQLException. But TimeoutException takes precedence.
      if (!timedOut && sqlExceptionWrapper.get() != null) {
        throw sqlExceptionWrapper.get();
      }
    }
  }

  public class PgTxnState {
    public PgTxnState(Connection connection, String connectionName) {
      this.connection = connection;
      this.connectionName = connectionName;
    }

    boolean isFinished() {
      return stmtExecuted != null &&
          beforeCommitState != null &&
          afterCommitState != null &&
          committed != null;
    }

    public boolean isSuccess() {
      return isFinished() &&
          stmtExecuted &&
          TransactionState.OPEN == beforeCommitState &&
          committed &&
          TransactionState.IDLE == afterCommitState;
    }

    public boolean isFailure() {
      if (!isFinished()) {
        return false;
      }

      // We have two cases:
      // 1. If stmt execution succeeded but commit failed.
      // 2. If stmt exec failed. Then txn should be in failed state and commit should succeed (but
      //    effectively do a rollback/abort).
      if (stmtExecuted) {
        return TransactionState.OPEN == beforeCommitState &&
            !committed &&
            TransactionState.IDLE == afterCommitState;

      } else {
        return TransactionState.FAILED == beforeCommitState &&
            committed &&
            TransactionState.IDLE == afterCommitState;
      }
    }

    @Override
    public String toString() {
      StringBuilder sb = new StringBuilder();
      sb.append("PgTxnState: ").append(connectionName).append("\n");
      sb.append("{\n");
      sb.append("  stmtExecuted: ").append(String.valueOf(stmtExecuted)).append("\n");
      sb.append("  beforeCommitState: ").append(String.valueOf(beforeCommitState)).append("\n");
      sb.append("  committed: ").append(String.valueOf(committed)).append("\n");
      sb.append("  afterCommitState: ").append(String.valueOf(afterCommitState)).append("\n");
      sb.append("}\n");
      return sb.toString();
    }

    private Statement getStatement() throws SQLException {
      if (statement != null) {
        return statement;
      }
      return connection.createStatement();
    }

    private String connectionName;
    private Connection connection;
    private Statement statement = null;

    private Boolean stmtExecuted = null;
    private TransactionState beforeCommitState = null;
    private Boolean committed = null;
    private TransactionState afterCommitState = null;
  }


  protected void executeWithTxnState(PgTxnState txnState, String sql) throws Exception {
    boolean previousStmtFailed = Boolean.FALSE.equals(txnState.stmtExecuted);
    txnState.stmtExecuted = false;
    try {
      executeWithTimeout(txnState.getStatement(), sql);
      txnState.stmtExecuted = !previousStmtFailed;
    } catch (PSQLException ex) {
      // TODO: validate the exception message.
      // Not reporting a stack trace here on purpose, because this will happen a lot in a test.
      LOG.info("Error while inserting on the second connection:" + ex.getMessage());
    }
  }

  protected void commitWithTxnState(PgTxnState txnState) {
    txnState.beforeCommitState = getPgTxnState(txnState.connection);
    txnState.committed = commitAndCatchException(txnState.connection, txnState.connectionName);
    txnState.afterCommitState = getPgTxnState(txnState.connection);
  }

  //------------------------------------------------------------------------------------------------
  // Test Utilities

  protected static class Row implements Comparable<Row>, Cloneable {
    static Row fromResultSet(ResultSet rs) throws SQLException {
      Object[] elems = new Object[rs.getMetaData().getColumnCount()];
      for (int i = 0; i < elems.length; i++) {
        elems[i] = rs.getObject(i + 1); // Column index starts from 1.
      }
      return new Row(elems);
    }

    ArrayList<Object> elems = new ArrayList<>();

    Row(Object... elems) {
      Collections.addAll(this.elems, elems);
    }

    Object get(int index) {
      return elems.get(index);
    }

    Boolean getBoolean(int index) {
      return (Boolean) elems.get(index);
    }

    Integer getInt(int index) {
      return (Integer) elems.get(index);
    }

    Long getLong(int index) {
      return (Long) elems.get(index);
    }

    Double getDouble(int index) {
      return (Double) elems.get(index);
    }

    String getString(int index) {
      return (String) elems.get(index);
    }

    @Override
    public boolean equals(Object obj) {
      if (obj == this) {
        return true;
      }
      if (!(obj instanceof Row)) {
        return false;
      }
      Row other = (Row)obj;
      return compareTo(other) == 0;
    }

    @Override
    public int compareTo(Row that) {
      // In our test, if selected Row has different number of columns from expected row, something
      // must be very wrong. Stop the test here.
      assertEquals("Row width mismatch between " + this + " and " + that,
          this.elems.size(), that.elems.size());
      return compare(this.elems, that.elems);
    }

    @Override
    public int hashCode() {
      return elems.hashCode();
    }

    @Override
    public String toString() {
      StringBuilder sb = new StringBuilder();
      sb.append("Row[");
      for (int i = 0; i < elems.size(); i++) {
        if (i > 0) sb.append(',');
        if (elems.get(i) == null) {
          sb.append("null");
        } else {
          sb.append(elems.get(i).getClass().getName() + "::");
          sb.append(elems.get(i).toString());
        }
      }
      sb.append(']');
      return sb.toString();
    }

    @Override
    public Row clone() throws CloneNotSupportedException {
      Row clone = (Row)super.clone();
      clone.elems = new ArrayList<>(elems.size());
      for (Object e : elems) {
        clone.elems.add(e);
      }
      return clone;
    }

    //
    // Helpers
    //

    /**
     * Compare two objects if possible. Is able to compare:
     * <ul>
     * <li>Primitives
     * <li>Comparables (including {@code String}) - but cannot handle the case of two unrelated
     * Comparables
     * <li>{@code PGobject}s wrapping {@code Comparable}s
     * <li>Arrays, {@code PgArray}s or lists of the above types
     * </ul>
     */
    @SuppressWarnings({ "unchecked", "rawtypes" })
    private static int compare(Object o1, Object o2) {
      if (o1 == null || o2 == null) {
        if (o1 != o2) {
          return o1 == null ? -1 : 1;
        }
        return 0;
      } else {
        Object promoted1 = promoteType(o1);
        Object promoted2 = promoteType(o2);
        if (promoted1 instanceof Long && promoted2 instanceof Long) {
          return ((Long) promoted1).compareTo((Long) promoted2);
        } else if (promoted1 instanceof Double && promoted2 instanceof Double) {
          return ((Double) promoted1).compareTo((Double) promoted2);
        } else if (promoted1 instanceof Number && promoted2 instanceof Number) {
          return Double.compare(
              ((Number) promoted1).doubleValue(),
              ((Number) promoted2).doubleValue());
        } else if (promoted1 instanceof Comparable && promoted2 instanceof Comparable) {
          // This is unsafe but we dont expect arbitrary types here.
          return ((Comparable) promoted1).compareTo((Comparable) promoted2);
        } else if (promoted1 instanceof List && promoted2 instanceof List) {
          List list1 = (List) promoted1;
          List list2 = (List) promoted2;
          if (list1.size() != list2.size()) {
            return Integer.compare(list1.size(), list2.size());
          }
          for (int i = 0; i < list1.size(); ++i) {
            int comparisonResult = compare(list1.get(i), list2.get(i));
            if (comparisonResult != 0) {
              return comparisonResult;
            }
          }
          return 0;
        } else {
          throw new IllegalArgumentException("Cannot compare "
              + o1 + " (of class " + o1.getClass().getCanonicalName() + ") with "
              + o2 + " (of class " + o1.getClass().getCanonicalName() + ")");
        }
      }
    }

    /** Converts the value to a widest one of the same type for comparison */
    @SuppressWarnings({ "rawtypes", "unchecked" })
    private static Object promoteType(Object v) {
      if (v instanceof Byte || v instanceof Short || v instanceof Integer) {
        return ((Number)v).longValue();
      } else if (v instanceof Float) {
        return ((Float)v).doubleValue();
      } else if (v instanceof Comparable) {
        return v;
      } else if (v instanceof List) {
        return v;
      } else if (v instanceof PGobject) {
        return promoteType(((PGobject) v).getValue()); // For PG_LSN type.
      } else if (v instanceof PgArray) {
        try {
          return promoteType(((PgArray) v).getArray());
        } catch (SQLException ex) {
          throw new RuntimeException("SQL exception during type promotion", ex);
        }
      } else if (v.getClass().isArray()) {
        List list = new ArrayList<>();
        // Unfortunately there's no easy way to automate that, we have to enumerate all array types
        // explicitly.
        if (v instanceof byte[]) {
          for (byte ve : (byte[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof short[]) {
          for (short ve : (short[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof int[]) {
          for (int ve : (int[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof long[]) {
          for (long ve : (long[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof float[]) {
          for (float ve : (float[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof double[]) {
          for (double ve : (double[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof boolean[]) {
          for (boolean ve : (boolean[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof char[]) {
          for (char ve : (char[]) v) {
            list.add(promoteType(ve));
          }
        } else if (v instanceof Object[]) {
          for (Object ve : (Object[]) v) {
            list.add(promoteType(ve));
          }
        }
        return list;
      } else {
        throw new IllegalArgumentException(v + " (of class " + v.getClass().getSimpleName() + ")"
            + " cannot be promoted for comparison!");
      }
    }
  }

  protected Set<Row> getRowSet(ResultSet rs) throws SQLException {
    Set<Row> rows = new HashSet<>();
    while (rs.next()) {
      rows.add(Row.fromResultSet(rs));
    }
    return rows;
  }

  protected Row getSingleRow(ResultSet rs) throws SQLException {
    assertTrue("Result set has no rows", rs.next());
    Row row = Row.fromResultSet(rs);
    assertFalse("Result set has more than one row", rs.next());
    return row;
  }

  protected List<Row> getRowList(ResultSet rs) throws SQLException {
    List<Row> rows = new ArrayList<>();
    while (rs.next()) {
      rows.add(Row.fromResultSet(rs));
    }
    return rows;
  }

  protected List<Row> getSortedRowList(ResultSet rs) throws SQLException {
    // Sort all rows and return.
    List<Row> rows = getRowList(rs);
    Collections.sort(rows);
    return rows;
  }

  protected void assertQuery(Statement stmt, String query, Row... expectedRows)
      throws SQLException {
    List<Row> actualRows = getRowList(stmt.executeQuery(query));
    assertEquals(
        "Expected " + expectedRows.length + " rows, got " + actualRows.size() + ": " + actualRows,
        expectedRows.length, actualRows.size());
    assertArrayEquals(expectedRows, actualRows.toArray(new Row[0]));
  }

  protected void assertNoRows(Statement stmt, String query) throws SQLException {
    List<Row> actualRows = getRowList(stmt.executeQuery(query));
    assertTrue("Expected no results, got " + actualRows, actualRows.isEmpty());
  }

  protected void assertNextRow(ResultSet rs, Object... values) throws SQLException {
    assertTrue(rs.next());
    Row expected = new Row(values);
    Row actual = Row.fromResultSet(rs);
    assertEquals(expected, actual);
  }

  protected void assertOneRow(Statement statement,
                              String query,
                              Object... values) throws SQLException {
    try (ResultSet rs = statement.executeQuery(query)) {
      assertNextRow(rs, values);
      assertFalse(rs.next());
    }
  }

  protected void assertRowSet(Statement statement,
                              String query,
                              Set<Row> expectedRows) throws SQLException {
    try (ResultSet rs = statement.executeQuery(query)) {
      assertEquals(expectedRows, getRowSet(rs));
    }
  }

  protected void assertRowList(Statement statement,
                               String query,
                               List<Row> expectedRows) throws SQLException {
    try (ResultSet rs = statement.executeQuery(query)) {
      assertEquals(expectedRows, getRowList(rs));
    }
  }

  private boolean doesQueryPlanContainsSubstring(Statement stmt, String query, String substring)
      throws SQLException {
    try (ResultSet rs = stmt.executeQuery("EXPLAIN " + query)) {
      assert (rs.getMetaData().getColumnCount() == 1); // Expecting one string column.
      while (rs.next()) {
        if (rs.getString(1).contains(substring)) {
          return true;
        }
      }
      return false;
    }
  }

  /** Whether or not this select query uses Index Scan with a given index. */
  protected boolean isIndexScan(Statement stmt, String query, String index)
      throws SQLException {
    return doesQueryPlanContainsSubstring(stmt, query, "Index Scan using " + index);
  }

  /** Whether or not this select query uses Index Only Scan with a given index. */
  protected boolean isIndexOnlyScan(Statement stmt, String query, String index)
      throws SQLException {
    return doesQueryPlanContainsSubstring(stmt, query, "Index Only Scan using " + index);
  }

  /**
   * Whether or not this select query requires filtering by Postgres (i.e. not all
   * conditions can be pushed down to YugaByte).
   */
  protected boolean doesNeedPgFiltering(Statement stmt, String query) throws SQLException {
    return doesQueryPlanContainsSubstring(stmt, query, "Filter:");
  }

  protected void createSimpleTableWithSingleColumnKey(String tableName) throws SQLException {
    try (Statement statement = connection.createStatement()) {
      String sql =
          "CREATE TABLE " + tableName + "(h bigint PRIMARY KEY, r float, vi int, vs text)";
      LOG.info("Creating table " + tableName + ", SQL statement: " + sql);
      statement.execute(sql);
      LOG.info("Table creation finished: " + tableName);
    }
  }

  protected void createSimpleTable(String tableName) throws SQLException {
    try (Statement statement = connection.createStatement()) {
      createSimpleTable(statement, tableName);
    }
  }

  protected void createSimpleTable(Statement statement, String tableName) throws SQLException {
    String sql =
        "CREATE TABLE " + tableName + "(h bigint, r float, vi int, vs text, PRIMARY KEY (h, r))";
    LOG.info("Creating table " + tableName + ", SQL statement: " + sql);
    statement.execute(sql);
    LOG.info("Table creation finished: " + tableName);
  }

  /**
   * @param statement The statement used to execute the query.
   * @param query The query string.
   * @param errorSubstring A (case-insensitive) substring of the expected error message.
   */
  protected void runInvalidQuery(Statement statement, String query, String errorSubstring) {
    try {
      statement.execute(query);
      fail(String.format("Statement did not fail: %s", query));
    } catch (SQLException e) {
      if (StringUtils.containsIgnoreCase(e.getMessage(), errorSubstring)) {
        LOG.info("Expected exception", e);
      } else {
        fail(String.format("Unexpected Error Message. Got: '%s', Expected to contain: '%s'",
                           e.getMessage(), errorSubstring));
      }
    }
  }

  /**
   * Verify that a (write) query succeeds with a warning matching the given substring.
   * @param statement The statement used to execute the query.
   * @param query The query string.
   * @param warningSubstring A (case-insensitive) substring of the expected warning message.
   */
  protected void verifyStatementWarning(Statement statement,
                                        String query,
                                        String warningSubstring) throws SQLException {
    statement.execute(query);
    SQLWarning warning = statement.getWarnings();
    assertNotEquals("Expected (at least) one warning", null, warning);
    assertTrue(String.format("Unexpected Warning Message. Got: '%s', expected to contain : '%s",
                             warning.getMessage(), warningSubstring),
               StringUtils.containsIgnoreCase(warning.getMessage(), warningSubstring));
    assertEquals("Expected (at most) one warning", null, warning.getNextWarning());
  }

  protected String getSimpleTableCreationStatement(
      String tableName,
      String valueColumnName,
      PartitioningMode partitioningMode) {
    String firstColumnIndexMode;
    if (partitioningMode == PartitioningMode.HASH) {
      firstColumnIndexMode = "HASH";
    } else {
      firstColumnIndexMode = "ASC";
    }
    return "CREATE TABLE " + tableName + "(h int, r int, " + valueColumnName + " int, " +
        "PRIMARY KEY (h " + firstColumnIndexMode + ", r))";
  }

  protected void createSimpleTable(
      String tableName,
      String valueColumnName,
      PartitioningMode partitioningMode) throws SQLException {
    try (Statement statement = connection.createStatement()) {
      String sql = getSimpleTableCreationStatement(tableName, valueColumnName, partitioningMode);
      LOG.info("Creating table " + tableName + ", SQL statement: " + sql);
      statement.execute(sql);
      LOG.info("Table creation finished: " + tableName);
    }
  }

  protected void createSimpleTable(String tableName, String valueColumnName) throws SQLException {
    createSimpleTable(tableName, valueColumnName, PartitioningMode.HASH);
  }

  protected List<Row> setupSimpleTable(String tableName) throws SQLException {
    List<Row> allRows = new ArrayList<>();
    try (Statement statement = connection.createStatement()) {
      createSimpleTable(statement, tableName);
      String insertTemplate = "INSERT INTO %s(h, r, vi, vs) VALUES (%d, %f, %d, '%s')";

      for (int h = 0; h < 10; h++) {
        for (int r = 0; r < 10; r++) {
          statement.execute(String.format(insertTemplate, tableName,
                                          h, r + 0.5, h * 10 + r, "v" + h + r));
          allRows.add(new Row((long) h,
                              r + 0.5,
                              h * 10 + r,
                              "v" + h + r));
        }
      }
    }

    // Sort inserted rows and return.
    Collections.sort(allRows);
    return allRows;
  }

  @Override
  public int getTestMethodTimeoutSec() {
    // initdb takes a really long time on macOS in debug mode.
    return 1200;
  }

  void waitForTServerHeartbeat() throws InterruptedException {
    // Wait an extra heartbeat interval to avoid race conditions due to deviations
    // in the real heartbeat frequency (due to latency, scheduling, etc.).
    Thread.sleep(MiniYBCluster.TSERVER_HEARTBEAT_INTERVAL_MS * 2);
  }

  /** Run a query and check row-count. */
  public int runQueryWithRowCount(Statement stmt, String query, int expectedRowCount)
      throws Exception {
    // Query and check row count.
    int rowCount = 0;
    try (ResultSet rs = stmt.executeQuery(query)) {
      while (rs.next()) {
        rowCount++;
      }
    }
    if (expectedRowCount >= 0) {
      // Caller wants to assert row-count.
      assertEquals(expectedRowCount, rowCount);
    } else {
      LOG.info(String.format("Exec query: row count = %d", rowCount));
    }

    return rowCount;
  }

  /** Run a query and check row-count. */
  private void runQueryWithRowCount(PreparedStatement pstmt, int expectedRowCount)
      throws Exception {
    // Query and check row count.
    int rowCount = 0;
    try (ResultSet rs = pstmt.executeQuery()) {
      while (rs.next()) {
        rowCount++;
      }
    }
    if (expectedRowCount >= 0) {
      // Caller wants to assert row-count.
      assertEquals(expectedRowCount, rowCount);
    } else {
      LOG.info(String.format("Exec query: row count = %d", rowCount));
    }
  }

  /** Time execution of a query. */
  protected long timeQueryWithRowCount(Statement stmt,
                                       String query,
                                       int expectedRowCount,
                                       int numberOfRuns) throws Exception {
    LOG.info(String.format("Exec statement: %s", stmt));

    // Not timing the first query run as its result is not predictable.
    runQueryWithRowCount(stmt, query, expectedRowCount);

    // Seek average run-time for a few different run.
    final long startTimeMillis = System.currentTimeMillis();
    for (int qrun = 0; qrun < numberOfRuns; qrun++) {
      runQueryWithRowCount(stmt, query, expectedRowCount);
    }

    // Check the elapsed time.
    long result = System.currentTimeMillis() - startTimeMillis;
    LOG.info(String.format("Ran query %d times. Total elapsed time = %d msecs",
        numberOfRuns, result));
    return result;
  }

  /** Time execution of a query. */
  protected long timeQueryWithRowCount(
      PreparedStatement pstmt,
      int expectedRowCount,
      int numberOfRuns) throws Exception {
    LOG.info("Exec prepared statement");

    // Not timing the first query run as its result is not predictable.
    runQueryWithRowCount(pstmt, expectedRowCount);

    // Seek average run-time for a few different run.
    final long startTimeMillis = System.currentTimeMillis();
    for (int qrun = 0; qrun < numberOfRuns; qrun++) {
      runQueryWithRowCount(pstmt, expectedRowCount);
    }

    // Check the elapsed time.
    long result = System.currentTimeMillis() - startTimeMillis;
    LOG.info(String.format("Ran statement %d times. Total elapsed time = %d msecs",
        numberOfRuns, result));
    return result;
  }

  /** Time execution of a statement. */
  protected long timeStatement(String sql, int numberOfRuns)
      throws Exception {
    LOG.info(String.format("Exec statement: %s", sql));

    // Not timing the first query run as its result is not predictable.
    try (Statement statement = connection.createStatement()) {
      statement.executeUpdate(sql);
    }

    // Seek average run-time for a few different run.
    final long startTimeMillis = System.currentTimeMillis();
    try (Statement statement = connection.createStatement()) {
      for (int qrun = 0; qrun < numberOfRuns; qrun++) {
        statement.executeUpdate(sql);
      }
    }

    // Check the elapsed time.
    long result = System.currentTimeMillis() - startTimeMillis;
    LOG.info(String.format("Ran statement %d times. Total elapsed time = %d msecs",
        numberOfRuns, result));
    return result;
  }

  /** Time execution of a statement. */
  protected long timeStatement(PreparedStatement pstmt, int numberOfRuns)
      throws Exception {
    LOG.info("Exec prepared statement");

    // Not timing the first query run as its result is not predictable.
    pstmt.executeUpdate();

    // Seek average run-time for a few different run.
    final long startTimeMillis = System.currentTimeMillis();
    for (int qrun = 0; qrun < numberOfRuns; qrun++) {
      pstmt.executeUpdate();
    }

    // Check the elapsed time.
    long result = System.currentTimeMillis() - startTimeMillis;
    LOG.info(String.format("Ran statement %d times. Total elapsed time = %d msecs",
        numberOfRuns, result));
    return result;
  }

  /** Time execution of a query. */
  protected long assertQueryRuntimeWithRowCount(
      Statement stmt,
      String query,
      int expectedRowCount,
      int numberOfRuns,
      long maxTotalMillis) throws Exception {
    long elapsedMillis = timeQueryWithRowCount(stmt, query, expectedRowCount, numberOfRuns);
    assertTrue(
        String.format("Query '%s' took %d ms! Expected %d ms at most", stmt, elapsedMillis,
            maxTotalMillis),
        elapsedMillis <= maxTotalMillis);
    return elapsedMillis;
  }

  /** Time execution of a query. */
  protected long assertQueryRuntimeWithRowCount(
      PreparedStatement pstmt,
      int expectedRowCount,
      int numberOfRuns,
      long maxTotalMillis) throws Exception {
    long elapsedMillis = timeQueryWithRowCount(pstmt, expectedRowCount, numberOfRuns);
    assertTrue(
        String.format("Query took %d ms! Expected %d ms at most", elapsedMillis, maxTotalMillis),
        elapsedMillis <= maxTotalMillis);
    return elapsedMillis;
  }

  /** Time execution of a statement. */
  protected long assertStatementRuntime(
      String sql,
      int numberOfRuns,
      long maxTotalMillis) throws Exception {
    long elapsedMillis = timeStatement(sql, numberOfRuns);
    assertTrue(
        String.format("Statement '%s' took %d ms! Expected %d ms at most", sql, elapsedMillis,
            maxTotalMillis),
        elapsedMillis <= maxTotalMillis);
    return elapsedMillis;
  }

  /** Time execution of a statement. */
  protected long assertStatementRuntime(
      PreparedStatement pstmt,
      int numberOfRuns,
      long maxTotalMillis) throws Exception {
    long elapsedMillis = timeStatement(pstmt, numberOfRuns);
    assertTrue(
        String.format("Statement took %d ms! Expected %d ms at most", elapsedMillis,
            maxTotalMillis),
        elapsedMillis <= maxTotalMillis);
    return elapsedMillis;
  }

  /** UUID of the first table with specified name. **/
  private String getTableUUID(String tableName)  throws Exception {
    for (Master.ListTablesResponsePB.TableInfo table :
        miniCluster.getClient().getTablesList().getTableInfoList()) {
      if (table.getName().equals(tableName)) {
        return table.getId().toStringUtf8();
      }
    }
    throw new Exception(String.format("YSQL table ''%s' not found", tableName));
  }

  protected RocksDBMetrics getRocksDBMetric(String tableName) throws Exception {
    return getRocksDBMetricByTableUUID(getTableUUID(tableName));
  }

  protected int getTableCounterMetric(String tableName,
                                      String metricName) throws Exception {
    return getTableCounterMetricByTableUUID(getTableUUID(tableName), metricName);
  }

  protected String getExplainAnalyzeOutput(Statement stmt, String query) throws Exception {
    try (ResultSet rs = stmt.executeQuery("EXPLAIN ANALYZE " + query)) {
      StringBuilder sb = new StringBuilder();
      while (rs.next()) {
        sb.append(rs.getString(1)).append("\n");
      }
      return sb.toString().trim();
    }
  }

  /** Immutable connection builder */
  private void runProcess(String... args) throws Exception {
    assertEquals(0, new ProcessBuilder(args).start().waitFor());
  }

  protected HostAndPort getMasterLeaderAddress() {
    return miniCluster.getClient().getLeaderMasterHostAndPort();
  }

  protected void setServerFlag(HostAndPort server, String flag, String value) throws Exception {
    runProcess(TestUtils.findBinary("yb-ts-cli"),
               "--server_address",
               server.toString(),
               "set_flag",
               "-force",
               flag,
               value);
  }

  public static class ConnectionBuilder implements Cloneable {
    private static final int MAX_CONNECTION_ATTEMPTS = 15;
    private static final int INITIAL_CONNECTION_DELAY_MS = 500;

    private final MiniYBCluster miniCluster;

    private int tserverIndex = 0;
    private String database = DEFAULT_PG_DATABASE;
    private String user = TEST_PG_USER;
    private String password = null;
    private String preferQueryMode = null;
    private IsolationLevel isolationLevel = IsolationLevel.DEFAULT;
    private AutoCommit autoCommit = AutoCommit.DEFAULT;

    ConnectionBuilder(MiniYBCluster miniCluster) {
      this.miniCluster = checkNotNull(miniCluster);
    }

    ConnectionBuilder withTServer(int tserverIndex) {
      ConnectionBuilder copy = clone();
      copy.tserverIndex = tserverIndex;
      return copy;
    }

    ConnectionBuilder withDatabase(String database) {
      ConnectionBuilder copy = clone();
      copy.database = database;
      return copy;
    }

    ConnectionBuilder withUser(String user) {
      ConnectionBuilder copy = clone();
      copy.user = user;
      return copy;
    }

    ConnectionBuilder withPassword(String password) {
      ConnectionBuilder copy = clone();
      copy.password = password;
      return copy;
    }

    ConnectionBuilder withIsolationLevel(IsolationLevel isolationLevel) {
      ConnectionBuilder copy = clone();
      copy.isolationLevel = isolationLevel;
      return copy;
    }

    ConnectionBuilder withAutoCommit(AutoCommit autoCommit) {
      ConnectionBuilder copy = clone();
      copy.autoCommit = autoCommit;
      return copy;
    }

    ConnectionBuilder withPreferQueryMode(String preferQueryMode) {
      ConnectionBuilder copy = clone();
      copy.preferQueryMode = preferQueryMode;
      return copy;
    }

    @Override
    protected ConnectionBuilder clone() {
      try {
        return (ConnectionBuilder) super.clone();
      } catch (CloneNotSupportedException ex) {
        throw new RuntimeException("This can't happen, but to keep compiler happy", ex);
      }
    }

    Connection connect() throws Exception {
      final InetSocketAddress postgresAddress = miniCluster.getPostgresContactPoints()
          .get(tserverIndex);
      String url = String.format(
          "jdbc:postgresql://%s:%d/%s",
          postgresAddress.getHostName(),
          postgresAddress.getPort(),
          database
      );

      Properties props = new Properties();
      props.setProperty("user", user);
      if (password != null) {
        props.setProperty("password", password);
      }
      if (preferQueryMode != null) {
        props.setProperty("preferQueryMode", preferQueryMode);
      }
      if (EnvAndSysPropertyUtil.isEnvVarOrSystemPropertyTrue("YB_PG_JDBC_TRACE_LOGGING")) {
        props.setProperty("loggerLevel", "TRACE");
      }

      int delayMs = INITIAL_CONNECTION_DELAY_MS;
      for (int attempt = 1; attempt <= MAX_CONNECTION_ATTEMPTS; ++attempt) {
        Connection connection = null;
        try {
          connection = checkNotNull(DriverManager.getConnection(url, props));

          if (isolationLevel != null) {
            connection.setTransactionIsolation(isolationLevel.pgIsolationLevel);
          }
          if (autoCommit != null) {
            connection.setAutoCommit(autoCommit.enabled);
          }

          ConnectionCleaner.register(connection);
          return connection;
        } catch (SQLException sqlEx) {
          // Close the connection now if we opened it, instead of waiting until the end of the test.
          if (connection != null) {
            try {
              connection.close();
            } catch (SQLException closingError) {
              LOG.error("Failure to close connection during failure cleanup before a retry:",
                  closingError);
              LOG.error("When handling this exception when opening/setting up connection:", sqlEx);
            }
          }

          boolean retry = false;

          if (attempt < MAX_CONNECTION_ATTEMPTS) {
            if (sqlEx.getMessage().contains("FATAL: the database system is starting up")
                || sqlEx.getMessage().contains("refused. Check that the hostname and port are " +
                "correct and that the postmaster is accepting")) {
              retry = true;

              LOG.info("Postgres is still starting up, waiting for " + delayMs + " ms. " +
                  "Got message: " + sqlEx.getMessage());
            } else if (sqlEx.getMessage().contains("the database system is in recovery mode")) {
              retry = true;

              LOG.info("Postgres is in recovery mode, waiting for " + delayMs + " ms. " +
                  "Got message: " + sqlEx.getMessage());
            }
          }

          if (retry) {
            Thread.sleep(delayMs);
            delayMs = Math.min(delayMs + 500, 10000);
          } else {
            LOG.error("Exception while trying to create connection (after " + attempt +
                " attempts): " + sqlEx.getMessage());
            throw sqlEx;
          }
        }
      }
      throw new IllegalStateException("Should not be able to reach here");
    }
  }
}
