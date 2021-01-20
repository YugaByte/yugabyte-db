/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. See accompanying LICENSE file.
 *
 * Portions Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 */
package org.yb.minicluster;

import com.google.common.base.Joiner;
import com.google.common.base.Preconditions;
import com.google.common.base.Stopwatch;
import com.google.common.collect.Lists;
import com.google.common.net.HostAndPort;
import org.apache.commons.io.FileUtils;
import org.yb.client.BaseYBClientTest;
import org.yb.client.TestUtils;
import org.yb.client.YBClient;
import org.yb.util.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.*;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentSkipListSet;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import static java.util.concurrent.TimeUnit.MILLISECONDS;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

/**
 * Utility class to start and manipulate YB clusters. Relies on being IN the source code with
 * both the yb-master and yb-tserver binaries already compiled. {@link BaseYBClientTest} should be
 * extended instead of directly using this class in almost all cases.
 */
public class MiniYBCluster implements AutoCloseable {

  private static final Logger LOG = LoggerFactory.getLogger(MiniYBCluster.class);

  // CQL port needs to be same for all nodes, since CQL clients use a configured port to generate
  // a host:port pair for each node given just the host.
  private static final int CQL_PORT = 9042;

  private static final int REDIS_PORT = 6379;

  // When picking an IP address for a tablet server to use, we check that we can bind to these
  // ports.
  private static final int[] TSERVER_CLIENT_FIXED_API_PORTS = new int[] { CQL_PORT,
      REDIS_PORT};

  // How often to push node list refresh events to CQL clients (in seconds)
  public static int CQL_NODE_LIST_REFRESH_SECS = 5;

  public static final int TSERVER_HEARTBEAT_INTERVAL_MS = 500;

  public static final int CATALOG_MANAGER_BG_TASK_WAIT_MS = 500;

  private static final String TSERVER_MASTER_ADDRESSES_FLAG = "--tserver_master_addrs";

  private static final String TSERVER_MASTER_ADDRESSES_FLAG_REGEX =
      TSERVER_MASTER_ADDRESSES_FLAG + ".*";

  // High threshold to avoid excessive slow query log.
  private static final int RPC_SLOW_QUERY_THRESHOLD = 10000000;

  private static final int YB_CLIENT_ADMIN_OPERATION_TIMEOUT_SEC = 120;

  // Timeout for waiting process to terminate.
  private static final long PROCESS_TERMINATE_TIMEOUT_MS =
      (long) (180 * 1000 * SanitizerUtil.getTimeoutMultiplier());

  // List of threads that print log messages.
  private final List<LogPrinter> logPrinters = new ArrayList<>();

  // Map of host/port pairs to master servers.
  private final Map<HostAndPort, MiniYBDaemon> masterProcesses = new ConcurrentHashMap<>();

  // Map of host/port pairs to tablet servers.
  private final Map<HostAndPort, MiniYBDaemon> tserverProcesses = new ConcurrentHashMap<>();

  private final List<String> pathsToDelete = new ArrayList<>();
  private final List<HostAndPort> masterHostPorts = new ArrayList<>();
  private final List<InetSocketAddress> cqlContactPoints = new ArrayList<>();
  private final List<InetSocketAddress> redisContactPoints = new ArrayList<>();
  private final List<InetSocketAddress> pgsqlContactPoints = new ArrayList<>();

  // Client we can use for common operations.
  private YBClient syncClient;

  private final MiniYBClusterParameters clusterParameters;

  private String masterAddresses;

  // We pass the Java test class name as a command line option to YB daemons so that we know what
  // test invoked them if they get stuck.
  private final String testClassName;

  /**
   * This is used to prevent trying to launch two daemons on the same loopback IP. However, this
   * only works with the same test process.
   */
  private static final ConcurrentSkipListSet<String> usedBindIPs =
      new ConcurrentSkipListSet<>();

  // These are used to assign master/tserver indexes used in the logs (the "m1", "ts2", etc.
  // prefixes).
  private AtomicInteger nextMasterIndex = new AtomicInteger(0);
  private AtomicInteger nextTServerIndex = new AtomicInteger(0);

  /**
   * Hard memory limit for YB daemons. This should be consistent with the memory limit set for C++
   * based mini clusters in external_mini_cluster.cc.
   */
  private static final long DAEMON_MEMORY_LIMIT_HARD_BYTES_NON_TSAN = 1024 * 1024 * 1024;
  private static final long DAEMON_MEMORY_LIMIT_HARD_BYTES_TSAN = 512 * 1024 * 1024;

  private String certFile = null;

  /**
   * Not to be invoked directly, but through a {@link MiniYBClusterBuilder}.
   */
  MiniYBCluster(MiniYBClusterParameters clusterParameters,
                List<String> masterArgs,
                List<List<String>> tserverArgs,
                List<String> commonTServerArgs,
                Map<String, String> tserverEnvVars,
                String testClassName,
                String certFile) throws Exception {
    this.clusterParameters = clusterParameters;
    this.testClassName = testClassName;
    this.certFile = certFile;
    if (clusterParameters.pgTransactionsEnabled && !clusterParameters.startPgSqlProxy) {
      throw new AssertionError(
          "Attempting to enable PostgreSQL transactions without enabling PostgreSQL API");
    }

    startCluster(
        clusterParameters.numMasters, clusterParameters.numTservers, masterArgs, tserverArgs,
        commonTServerArgs, tserverEnvVars);
    startSyncClient();
  }

  public void startSyncClient() throws Exception {
    startSyncClient(true);
  }

  public void startSyncClient(boolean waitForMasterLeader) throws Exception {
    syncClient = new YBClient.YBClientBuilder(getMasterAddresses())
        .defaultAdminOperationTimeoutMs(clusterParameters.defaultTimeoutMs)
        .defaultOperationTimeoutMs(clusterParameters.defaultTimeoutMs)
        .sslCertFile(certFile)
        .build();

    if (waitForMasterLeader) {
      syncClient.waitForMasterLeader(clusterParameters.defaultTimeoutMs);
    }
  }

  /**
   * Should only be used to get current cluster parameters.
   * WARNING: do not modify fields of returned value, since this is not supported and can have
   * unwanted effect.
   */
  public MiniYBClusterParameters getClusterParameters() {
    return clusterParameters;
  }

  private static void addFlagsFromEnv(List<String> dest, String envVarName) {
    final String extraFlagsFromEnv = System.getenv(envVarName);
    if (extraFlagsFromEnv != null) {
      // TODO: this has an issue with handling quoted arguments with embedded spaces.
      for (String flag : extraFlagsFromEnv.split("\\s+")) {
        dest.add(flag);
      }
    }
  }

  /** Common flags for both master and tserver processes */
  private List<String> getCommonDaemonFlags() {
    final List<String> commonFlags = Lists.newArrayList(
        // Ensure that logging goes to the test output and doesn't get buffered.
        "--logtostderr",
        "--logbuflevel=-1",
        "--webserver_doc_root=" + TestUtils.getWebserverDocRoot());
    addFlagsFromEnv(commonFlags, "YB_EXTRA_DAEMON_FLAGS");
    if (testClassName != null) {
      commonFlags.add("--yb_test_name=" + testClassName);
    }

    final long memoryLimit = SanitizerUtil.nonTsanVsTsan(
        DAEMON_MEMORY_LIMIT_HARD_BYTES_NON_TSAN,
        DAEMON_MEMORY_LIMIT_HARD_BYTES_TSAN);
    commonFlags.add("--memory_limit_hard_bytes=" + memoryLimit);

    // YB_TEST_INVOCATION_ID is a special environment variable that we use to force-kill all
    // processes even if MiniYBCluster fails to kill them.
    String testInvocationId = System.getenv("YB_TEST_INVOCATION_ID");
    if (testInvocationId != null) {
      // We use --metric_node_name=... to include a unique "test invocation id" into the command
      // line so we can kill any stray processes later. --metric_node_name is normally how we pass
      // the Universe ID to the cluster. We could use any other flag that is present in yb-master
      // and yb-tserver for this.
      commonFlags.add("--metric_node_name=" + testInvocationId);
    }

    commonFlags.add("--yb_num_shards_per_tserver=" + clusterParameters.numShardsPerTServer);
    commonFlags.add("--ysql_num_shards_per_tserver=" + clusterParameters.numShardsPerTServer);

    if (clusterParameters.replicationFactor > 0) {
      commonFlags.add("--replication_factor=" + clusterParameters.replicationFactor);
    }

    if (clusterParameters.startPgSqlProxy) {
      commonFlags.add("--enable_ysql=true");
    } else {
      commonFlags.add("--enable_ysql=false");
    }

    return commonFlags;
  }

  /**
   * Wait up to this instance's "default timeout" for an expected count of TS to
   * connect to the master.
   *
   * @param expected How many TS are expected
   * @return true if there are at least as many TS as expected, otherwise false
   */
  public boolean waitForTabletServers(int expected) throws Exception {
    int count = 0;
    Stopwatch stopwatch = Stopwatch.createStarted();
    while (count < expected &&
        stopwatch.elapsed(MILLISECONDS) < clusterParameters.defaultTimeoutMs) {
      Thread.sleep(200);
      count = syncClient.listTabletServers().getTabletServersCount();
    }
    boolean success = count >= expected;
    if (!success) {
      LOG.error("Waited for " + clusterParameters.defaultTimeoutMs + " ms for " + expected +
                " tablet servers to be online. Only found " + count + " tablet servers.");
    }
    return success;
  }

  /**
   * @return the string representation of a random localhost IP.
   */
  private String getRandomBindAddressOnLinux() throws IllegalArgumentException {
    assert(TestUtils.IS_LINUX);
    // On Linux we can use 127.x.y.z, so let's just pick a random address.
    final StringBuilder randomLoopbackIp = new StringBuilder("127");
    final Random rng = RandomNumberUtil.getRandomGenerator();
    for (int i = 0; i < 3; ++i) {
      // Do not use 0 or 255 for IP components.
      randomLoopbackIp.append("." + (1 + rng.nextInt(254)));
    }
    return randomLoopbackIp.toString();
  }

  private boolean canUseBindIP(String bindIP, boolean logException) throws IOException {
    if (usedBindIPs.contains(bindIP)) {
      // We are using this bind IP for a daemon already.
      return false;
    }

    final InetAddress bindIp = InetAddress.getByName(bindIP);
    for (int clientApiPort : TSERVER_CLIENT_FIXED_API_PORTS) {
      if (!TestUtils.isPortFree(bindIp, clientApiPort, logException)) {
        // One of the ports we need to be free is not free, reject this IP address.
        return false;
      }
    }

    // All ports we care about are free, let's try to use this IP address.
    return usedBindIPs.add(bindIP);
  }

  private String getDaemonBindAddress(MiniYBDaemonType daemonType) throws IOException {
    if (TestUtils.IS_LINUX && !clusterParameters.useIpWithCertificate) {
      return pickFreeRandomBindIpOnLinux(daemonType);
    }

    return pickFreeBindIpOnMacOrWithCertificate(daemonType);
  }

  private String getMasterBindAddress() throws IOException {
    return getDaemonBindAddress(MiniYBDaemonType.MASTER);
  }

  private String getTabletServerBindAddress() throws IOException {
    return getDaemonBindAddress(MiniYBDaemonType.TSERVER);
  }

  private String pickFreeRandomBindIpOnLinux(MiniYBDaemonType daemonType) throws IOException {
    final int MAX_NUM_ATTEMPTS = 1000;
    for (int i = 1; i <= MAX_NUM_ATTEMPTS; ++i) {
      String randomBindAddress = getRandomBindAddressOnLinux();
      if (canUseBindIP(randomBindAddress, i == MAX_NUM_ATTEMPTS)) {
        return randomBindAddress;
      }
    }
    throw new IOException("Could not find a loopback IP of the form 127.x.y.z for a " +
        daemonType.humanReadableName() + " in " + MAX_NUM_ATTEMPTS + " attempts");
  }

  private String getLoopbackIpWithLastTwoBytes(int nextToLastByte, int lastByte) {
    return "127.0." + nextToLastByte + "." + lastByte;
  }

  private final int MIN_LAST_IP_BYTE = 2;
  private final int MAX_LAST_IP_BYTE = 254;

  private String pickFreeBindIpOnMacOrWithCertificate(
      MiniYBDaemonType daemonType) throws IOException {
    List<String> bindIps = new ArrayList<>();

    final int nextToLastByteMin = 0;
    // If we need an IP with a certificate, use 127.0.0.*, otherwise use 127.0.x.y with a small
    // range of x.
    final int nextToLastByteMax = clusterParameters.useIpWithCertificate ? 0 : 3;

    if (TestUtils.IS_LINUX) {
      // We only use even last bytes of the loopback IP in case we are testing TLS encryption.
      final int lastIpByteStep = clusterParameters.useIpWithCertificate ? 2 : 1;
      for (int nextToLastByte = nextToLastByteMin;
           nextToLastByte <= nextToLastByteMax;
           ++nextToLastByte) {
        for (int lastIpByte = MIN_LAST_IP_BYTE;
             lastIpByte <= MAX_LAST_IP_BYTE;
             lastIpByte += lastIpByteStep) {
          String bindIp = getLoopbackIpWithLastTwoBytes(nextToLastByte, lastIpByte);
          if (!usedBindIPs.contains(bindIp)) {
            bindIps.add(bindIp);
          }
        }
      }
    } else {
      List<String> loopbackIps  = BindIpUtil.getLoopbackIPs();
      if (clusterParameters.useIpWithCertificate) {
        // macOS, but we need a 127.0.0.x, where x is even.
        for (String loopbackIp : loopbackIps) {
          if (loopbackIp.startsWith("127.0.0.")) {
            String[] components = loopbackIp.split("[.]");
            int lastIpByte = Integer.valueOf(components[components.length - 1]);
            if (lastIpByte >= MIN_LAST_IP_BYTE &&
                lastIpByte <= MAX_LAST_IP_BYTE &&
                lastIpByte % 2 == 0) {
              bindIps.add(loopbackIp);
            }
          }
        }
      } else {
        // macOS, no requirement that there is a certificate.
        bindIps = loopbackIps;
      }
    }

    Collections.shuffle(bindIps, RandomNumberUtil.getRandomGenerator());

    for (int i = bindIps.size() - 1; i >= 0; --i) {
      String bindAddress = bindIps.get(i);
      if (canUseBindIP(bindAddress, i == 0)) {
        return bindAddress;
      }
    }

    Collections.sort(bindIps);
    throw new IOException(String.format(
        "Cannot find a loopback IP of the form 127.0.x.y to launch a %s on. " +
            "Considered options: %s.",
        daemonType.humanReadableName(),
        bindIps));
  }

  /**
   * Starts a YB cluster composed of the provided masters and tablet servers.
   *
   * @param numMasters how many masters to start
   * @param numTservers how many tablet servers to start
   * @param masterArgs extra master arguments
   * @param perTServerArgs per-tablet server arguments
   */
  private void startCluster(int numMasters,
                            int numTservers,
                            List<String> masterArgs,
                            List<List<String>> perTServerArgs,
                            List<String> commonTServerArgs,
                            Map<String, String> tserverEnvVars) throws Exception {
    Preconditions.checkArgument(numMasters > 0, "Need at least one master");
    Preconditions.checkArgument(numTservers > 0, "Need at least one tablet server");
    Preconditions.checkNotNull(perTServerArgs);
    if (perTServerArgs.size() != numTservers) {
      throw new AssertionError("numTservers=" + numTservers + " but (perTServerArgs has " +
          perTServerArgs.size() + " elements");
    }
    // The following props are set via yb-client's pom.
    String baseDirPath = TestUtils.getBaseTmpDir();

    for (String envVarName :
        new String[]{"ASAN_OPTIONS", "TSAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS",
                     "ASAN_SYMBOLIZER_PATH"}) {
      String envVarValue = System.getenv(envVarName);
      LOG.info("Environment variable " + envVarName + ": " +
               (envVarValue == null ? "not set" : envVarValue));
    }
    LOG.info("Starting {} masters...", numMasters);
    startMasters(numMasters, baseDirPath, masterArgs);

    LOG.info("Starting {} tablet servers...", numTservers);
    startTabletServers(numTservers, perTServerArgs, commonTServerArgs, tserverEnvVars);
  }

  private void startTabletServers(
      int numTservers,
      List<List<String>> tserverArgs,
      List<String> commonTServerArgs,
      Map<String, String> tserverEnvVars) throws Exception {
    LOG.info("startTabletServers: numTServers=" + numTservers +
        ", tserverArgs=" + tserverArgs +
        ", commonTServerArgs=" + commonTServerArgs);

    for (int i = 0; i < numTservers; i++) {
      List<String> concatenatedArgs = new ArrayList<>();
      if (tserverArgs.get(i) != null) {
        concatenatedArgs.addAll(tserverArgs.get(i));
      }
      concatenatedArgs.addAll(commonTServerArgs);
      startTServer(concatenatedArgs, tserverEnvVars);
    }

    long tserverStartupDeadlineMs = System.currentTimeMillis() + 60000;
    for (MiniYBDaemon tserverProcess : tserverProcesses.values()) {
      tserverProcess.waitForServerStartLogMessage(tserverStartupDeadlineMs);
    }
  }

  /**
   * Update the master addresses for MiniYBCluster and also for the flagsfile so that tservers
   * pick it up.
   */
  private void updateMasterAddresses() throws IOException {
    masterAddresses = NetUtil.hostsAndPortsToString(masterHostPorts);
    Path flagsFile = Paths.get (TestUtils.getFlagsPath());
    String content = new String(Files.readAllBytes(flagsFile));
    LOG.info("Retrieved flags file content: " + content);
    String tserverMasterAddressesFlag = String.format("%s=%s", TSERVER_MASTER_ADDRESSES_FLAG,
      masterAddresses);
    if (content.contains(TSERVER_MASTER_ADDRESSES_FLAG)) {
      content = content.replaceAll(TSERVER_MASTER_ADDRESSES_FLAG_REGEX, tserverMasterAddressesFlag);
    } else {
      content += tserverMasterAddressesFlag + "\n";
    }
    Files.write(flagsFile, content.getBytes());
    LOG.info("Wrote flags file content: " + content);
  }

  public void startTServer(List<String> tserverArgs) throws Exception {
    startTServer(tserverArgs, null, null, null);
  }

  public void startTServer(List<String> tserverArgs,
                           Map<String, String> tserverEnvVars) throws Exception {
    startTServer(tserverArgs, null, null, tserverEnvVars);
  }

  public void startTServer(List<String> tserverArgs, String tserverBindAddress,
                           Integer tserverRpcPort) throws Exception {
    startTServer(tserverArgs, tserverBindAddress, tserverRpcPort, null);
  }

  public void startTServer(List<String> tserverArgs, String tserverBindAddress,
                           Integer tserverRpcPort,
                           Map<String, String> tserverEnvVars) throws Exception {
    LOG.info("Starting a tablet server: " +
        "tserverArgs=" + tserverArgs +
        ", tserverBindAddress=" + tserverBindAddress +
        ", tserverRpcPort=" + tserverRpcPort);
    String baseDirPath = TestUtils.getBaseTmpDir();
    long now = System.currentTimeMillis();
    if (tserverBindAddress == null) {
      tserverBindAddress = getTabletServerBindAddress();
    }

    final int rpcPort = (tserverRpcPort == null) ? TestUtils.findFreePort(tserverBindAddress) :
        tserverRpcPort;
    final int webPort = TestUtils.findFreePort(tserverBindAddress);
    final int redisWebPort = TestUtils.findFreePort(tserverBindAddress);
    final int cqlWebPort = TestUtils.findFreePort(tserverBindAddress);
    final int postgresPort = TestUtils.findFreePort(tserverBindAddress);
    final int pgsqlWebPort = TestUtils.findFreePort(tserverBindAddress);

    // TODO: use a random port here as well.
    final int redisPort = REDIS_PORT;

    String dataDirPath = baseDirPath + "/ts-" + tserverBindAddress + "-" + rpcPort + "-" + now;
    String flagsPath = TestUtils.getFlagsPath();
    final List<String> tsCmdLine = Lists.newArrayList(
        TestUtils.findBinary("yb-tserver"),
        "--flagfile=" + flagsPath,
        "--fs_data_dirs=" + dataDirPath,
        "--tserver_master_addrs=" + masterAddresses,
        "--webserver_interface=" + tserverBindAddress,
        "--local_ip_for_outbound_sockets=" + tserverBindAddress,
        "--rpc_bind_addresses=" + tserverBindAddress + ":" + rpcPort,
        "--webserver_port=" + webPort,
        "--redis_proxy_bind_address=" + tserverBindAddress + ":" + redisPort,
        "--redis_proxy_webserver_port=" + redisWebPort,
        "--cql_proxy_bind_address=" + tserverBindAddress + ":" + CQL_PORT,
        "--cql_nodelist_refresh_interval_secs=" + CQL_NODE_LIST_REFRESH_SECS,
        "--heartbeat_interval_ms=" + TSERVER_HEARTBEAT_INTERVAL_MS,
        "--rpc_slow_query_threshold_ms=" + RPC_SLOW_QUERY_THRESHOLD,
        "--cql_proxy_webserver_port=" + cqlWebPort,
        "--pgsql_proxy_webserver_port=" + pgsqlWebPort,
        "--yb_client_admin_operation_timeout_sec=" + YB_CLIENT_ADMIN_OPERATION_TIMEOUT_SEC,
        "--callhome_enabled=false",
        "--TEST_process_info_dir=" + getProcessInfoDir());
    addFlagsFromEnv(tsCmdLine, "YB_EXTRA_TSERVER_FLAGS");

    if (clusterParameters.startPgSqlProxy) {
      tsCmdLine.addAll(Lists.newArrayList(
          "--pgsql_proxy_bind_address=" + tserverBindAddress + ":" + postgresPort
      ));
      if (clusterParameters.pgTransactionsEnabled) {
        tsCmdLine.add("--pg_transactions_enabled");
      }
    }

    if (tserverArgs != null) {
      for (String arg : tserverArgs) {
        tsCmdLine.add(arg);
      }
    }

    final MiniYBDaemon daemon = configureAndStartProcess(MiniYBDaemonType.TSERVER,
        tsCmdLine.toArray(new String[tsCmdLine.size()]),
        tserverBindAddress, rpcPort, webPort, pgsqlWebPort,
        cqlWebPort, redisWebPort, dataDirPath, tserverEnvVars);
    tserverProcesses.put(HostAndPort.fromParts(tserverBindAddress, rpcPort), daemon);
    cqlContactPoints.add(new InetSocketAddress(tserverBindAddress, CQL_PORT));
    redisContactPoints.add(new InetSocketAddress(tserverBindAddress, redisPort));
    pgsqlContactPoints.add(new InetSocketAddress(tserverBindAddress, postgresPort));

    if (flagsPath.startsWith(baseDirPath)) {
      // We made a temporary copy of the flags; delete them later.
      pathsToDelete.add(flagsPath);
    }
    pathsToDelete.add(dataDirPath);
  }

  /**
   * Returns the common options among regular masters and shell masters.
   * @return a list of command line options
   */
  private List<String> getCommonMasterCmdLine(String flagsPath, String dataDirPath,
                                              String masterBindAddress, int masterRpcPort,
                                              int masterWebPort) throws Exception {
    List<String> masterCmdLine = Lists.newArrayList(
      TestUtils.findBinary("yb-master"),
      "--flagfile=" + flagsPath,
      "--fs_wal_dirs=" + dataDirPath,
      "--fs_data_dirs=" + dataDirPath,
      "--webserver_interface=" + masterBindAddress,
      "--local_ip_for_outbound_sockets=" + masterBindAddress,
      "--rpc_bind_addresses=" + masterBindAddress + ":" + masterRpcPort,
      "--catalog_manager_bg_task_wait_ms=" + CATALOG_MANAGER_BG_TASK_WAIT_MS,
      "--rpc_slow_query_threshold_ms=" + RPC_SLOW_QUERY_THRESHOLD,
      "--webserver_port=" + masterWebPort,
      "--callhome_enabled=false",
      "--TEST_process_info_dir=" + getProcessInfoDir());
    if (clusterParameters.tserverHeartbeatTimeoutMsOpt.isPresent()) {
      masterCmdLine.add(
          "--tserver_unresponsive_timeout_ms=" +
          clusterParameters.tserverHeartbeatTimeoutMsOpt.get());
    }
    if (clusterParameters.yqlSystemPartitionsVtableRefreshSecsOpt.isPresent()) {
      masterCmdLine.add(
          "--partitions_vtable_cache_refresh_secs=" +
          clusterParameters.yqlSystemPartitionsVtableRefreshSecsOpt.get());
    }
    addFlagsFromEnv(masterCmdLine, "YB_EXTRA_MASTER_FLAGS");
    return masterCmdLine;
  }

  public HostAndPort startShellMaster() throws Exception {
    return startShellMaster(new TreeMap<String, String>());
  }

  /**
   * Start a new master server in 'shell' mode. Finds free web and RPC ports and then
   * starts the master on those ports, finally populates the 'masters' map.
   * @param extraArgs extra flags to pass to the master process.
   *
   * @return the host and port for a newly created master.
   * @throws Exception if we are unable to start the master.
   */
  public HostAndPort startShellMaster(Map<String, String> extraArgs) throws Exception {
    final String baseDirPath = TestUtils.getBaseTmpDir();
    final String masterBindAddress = getMasterBindAddress();
    final int rpcPort = TestUtils.findFreePort(masterBindAddress);
    final int webPort = TestUtils.findFreePort(masterBindAddress);
    LOG.info("Starting shell master on {}, port {}.", masterBindAddress, rpcPort);
    long now = System.currentTimeMillis();
    final String dataDirPath =
        baseDirPath + "/master-" + masterBindAddress + "-" + rpcPort + "-" + now;
    final String flagsPath = TestUtils.getFlagsPath();
    List<String> masterCmdLine = getCommonMasterCmdLine(flagsPath, dataDirPath,
      masterBindAddress, rpcPort, webPort);
    for (Map.Entry<String, String> entry : extraArgs.entrySet()) {
      masterCmdLine.add("--" + entry.getKey() + "=" + entry.getValue());
    }

    final MiniYBDaemon daemon = configureAndStartProcess(
        MiniYBDaemonType.MASTER, masterCmdLine.toArray(new String[masterCmdLine.size()]),
        masterBindAddress, rpcPort, webPort, -1, -1, -1, dataDirPath, null);

    final HostAndPort masterHostPort = HostAndPort.fromParts(masterBindAddress, rpcPort);
    masterHostPorts.add(masterHostPort);
    updateMasterAddresses();
    masterProcesses.put(masterHostPort, daemon);

    if (flagsPath.startsWith(baseDirPath)) {
      // We made a temporary copy of the flags; delete them later.
      pathsToDelete.add(flagsPath);
    }
    pathsToDelete.add(dataDirPath);

    TestUtils.logAndSleepMs(5000, "let the shell master get initialized and running");

    return masterHostPort;
  }

  private static class MasterHostPortAllocation {
    final String bindAddress;
    final int rpcPort;
    final int webPort;

    public MasterHostPortAllocation(String bindAddress, int rpcPort, int webPort) {
      this.bindAddress = bindAddress;
      this.rpcPort = rpcPort;
      this.webPort = webPort;
    }
  }

  /**
   * Start the specified number of master servers with ports starting from a specified
   * number. Finds free web and RPC ports up front for all of the masters first, then
   * starts them on those ports, populating 'masters' map.
   *
   * @param numMasters number of masters to start
   * @param baseDirPath  the base directory where the mini cluster stores its data
   * @param extraMasterArgs common command-line arguments to pass to all masters
   * @throws Exception if we are unable to start the masters
   */
  private void startMasters(
      int numMasters,
      String baseDirPath,
      List<String> extraMasterArgs) throws Exception {
    assert(masterHostPorts.isEmpty());

    // Get the list of web and RPC ports to use for the master consensus configuration:
    // request NUM_MASTERS * 2 free ports as we want to also reserve the web
    // ports for the consensus configuration.
    final List<MasterHostPortAllocation> masterHostPortAlloc = new ArrayList<>();
    for (int i = 0; i < numMasters; ++i) {
      final String masterBindAddress = getMasterBindAddress();
      final int rpcPort = TestUtils.findFreePort(masterBindAddress);
      final int webPort = TestUtils.findFreePort(masterBindAddress);
      masterHostPortAlloc.add(
          new MasterHostPortAllocation(masterBindAddress, rpcPort, webPort));
      masterHostPorts.add(HostAndPort.fromParts(masterBindAddress, rpcPort));
    }

    updateMasterAddresses();
    for (MasterHostPortAllocation masterAlloc : masterHostPortAlloc) {
      final String masterBindAddress = masterAlloc.bindAddress;
      final int masterRpcPort = masterAlloc.rpcPort;
      final long now = System.currentTimeMillis();
      String dataDirPath =
          baseDirPath + "/master-" + masterBindAddress + "-" + masterRpcPort + "-" + now;
      String flagsPath = TestUtils.getFlagsPath();
      final int masterWebPort = masterAlloc.webPort;
      List<String> masterCmdLine = getCommonMasterCmdLine(flagsPath, dataDirPath,
        masterBindAddress, masterRpcPort, masterWebPort);
      masterCmdLine.add("--master_addresses=" + masterAddresses);
      if (extraMasterArgs != null) {
        masterCmdLine.addAll(extraMasterArgs);
      }
      if (clusterParameters.startPgSqlProxy) {
        masterCmdLine.add("--master_auto_run_initdb");
      }
      final HostAndPort masterHostAndPort = HostAndPort.fromParts(masterBindAddress, masterRpcPort);
      masterProcesses.put(masterHostAndPort,
          configureAndStartProcess(
              MiniYBDaemonType.MASTER,
              masterCmdLine.toArray(new String[masterCmdLine.size()]),
              masterBindAddress, masterRpcPort, masterWebPort, -1, -1, -1, dataDirPath, null));

      if (flagsPath.startsWith(baseDirPath)) {
        // We made a temporary copy of the flags; delete them later.
        pathsToDelete.add(flagsPath);
      }
      pathsToDelete.add(dataDirPath);
    }

    long startupDeadlineMs = System.currentTimeMillis() + 120000;
    for (MiniYBDaemon masterProcess : masterProcesses.values()) {
      masterProcess.waitForServerStartLogMessage(startupDeadlineMs);
    }
  }

  /**
   * Starts a process using the provided command and configures it to be daemon,
   * redirects the stderr to stdout, and starts a thread that will read from the process' input
   * stream and redirect that to LOG.
   *
   * @param type Daemon type
   * @param command Process and options
   * @return The started process
   * @throws Exception Exception if an error prevents us from starting the process,
   *                   or if we were able to start the process but noticed that it was then killed
   *                   (in which case we'll log the exit value).
   */
  private MiniYBDaemon configureAndStartProcess(MiniYBDaemonType type,
                                                String[] command,
                                                String bindIp,
                                                int rpcPort,
                                                int webPort,
                                                int pgsqlWebPort,
                                                int cqlWebPort,
                                                int redisWebPort,
                                                String dataDirPath,
                                                Map<String, String> environment) throws Exception {
    command[0] = FileSystems.getDefault().getPath(command[0]).normalize().toString();
    final int indexForLog =
        type == MiniYBDaemonType.MASTER ? nextMasterIndex.incrementAndGet()
            : nextTServerIndex.incrementAndGet();

    {
      List<String> args = new ArrayList<>();
      args.addAll(Arrays.asList(command));
      String fatalDetailsPathPrefix = System.getenv("YB_FATAL_DETAILS_PATH_PREFIX");
      if (fatalDetailsPathPrefix == null) {
        fatalDetailsPathPrefix =
            TestUtils.getTestReportFilePrefix() + "fatal_failure_details";
      }
      fatalDetailsPathPrefix += "." + type.shortStr() + "-" + indexForLog + "." + bindIp + "-" +
          "port" + rpcPort;
      args.add("--fatal_details_path_prefix=" + fatalDetailsPathPrefix);
      args.addAll(getCommonDaemonFlags());
      command = args.toArray(command);
    }

    ProcessBuilder procBuilder = new ProcessBuilder(command).redirectErrorStream(true);
    String envString = "{}";
    if (environment != null) {
      procBuilder.environment().putAll(environment);
      envString = environment.toString();
    }
    LOG.info("Starting process: {} with environment {}", Joiner.on(" ").join(command), envString);
    Process proc = procBuilder.start();
    final MiniYBDaemon daemon =
        new MiniYBDaemon(type, indexForLog, command, proc, bindIp, rpcPort, webPort,
                         pgsqlWebPort, cqlWebPort, redisWebPort, dataDirPath);
    logPrinters.add(daemon.getLogPrinter());

    Thread.sleep(300);
    try {
      int ev = proc.exitValue();
      throw new Exception("We tried starting a process (" + command[0] + ") but it exited with " +
          "value=" + ev + (daemon.getLogPrinter().getError() == null ?
                           "" : ", error: " + daemon.getLogPrinter().getError()));
    } catch (IllegalThreadStateException ex) {
      // This means the process is still alive, it's like reverse psychology.
    }

    LOG.info("Started " + command[0] + " as pid " + ProcessUtil.pidOfProcess(proc));

    return daemon;
  }

  private MiniYBDaemon restart(MiniYBDaemon daemon) throws Exception {
    String[] command = daemon.getCommandLine();
    LOG.info("Restarting process: {}", Joiner.on(" ").join(command));
    daemon = daemon.restart();
    logPrinters.add(daemon.getLogPrinter());

    Process proc = daemon.getProcess();
    Thread.sleep(300);
    try {
      int ev = proc.exitValue();
      throw new Exception("We tried starting a process (" + command[0] + ") but it exited with " +
          "value=" + ev);
    } catch (IllegalThreadStateException ex) {
      // This means the process is still alive, it's like reverse psychology.
    }

    LOG.info("Restarted " + command[0] + " as pid " + ProcessUtil.pidOfProcess(proc));

    return daemon;
  }

  public void restart() throws Exception {
    restart(true /* waitForMasterLeader */);
  }

  /**
   * Restart the cluster
   * @param waitForMasterLeader should sync client wait for master leader.
   */
  public void restart(boolean waitForMasterLeader) throws Exception {
    List<MiniYBDaemon> masters = new ArrayList<>(masterProcesses.values());
    List<MiniYBDaemon> tservers = new ArrayList<>(tserverProcesses.values());

    LOG.info("Shutting down mini cluster");
    shutdownDaemons();

    LOG.info("Restarting mini cluster");
    for (MiniYBDaemon master : masters) {
      master = restart(master);
      masterProcesses.put(master.getHostAndPort(), master);
    }
    for (MiniYBDaemon tserver : tservers) {
      tserver = restart(tserver);
      tserverProcesses.put(tserver.getHostAndPort(), tserver);
    }

    startSyncClient(waitForMasterLeader);

    LOG.info("Restarted mini cluster");
  }

  private void processCoreFile(MiniYBDaemon daemon) throws Exception {
    CoreFileUtil.processCoreFile(
        daemon.getPid(), daemon.getCommandLine()[0], daemon.toString(),
        /* coreFileDir */ null,
        CoreFileUtil.CoreFileMatchMode.EXACT_PID);
  }

  private void destroyDaemon(MiniYBDaemon daemon) throws Exception {
    LOG.warn("Destroying " + daemon + ", IsAlive: " + daemon.getProcess().isAlive());
    daemon.terminate();
    processCoreFile(daemon);
  }

  private void destroyDaemonAndWait(MiniYBDaemon daemon) throws Exception {
    destroyDaemon(daemon);
    daemon.getProcess().waitFor();
  }

  /**
   * Kills the TS listening on the provided port. Doesn't do anything if the TS was already killed.
   * @param hostPort host and port on which the tablet server is listening on
   * @throws InterruptedException
   */
  public void killTabletServerOnHostPort(HostAndPort hostPort) throws Exception {
    final MiniYBDaemon ts = tserverProcesses.remove(hostPort);
    if (ts == null) {
      // The TS is already dead, good.
      return;
    }
    assert(cqlContactPoints.remove(new InetSocketAddress(hostPort.getHost(), CQL_PORT)));
    // TODO: bug, we're using the multiple Hostnames with different ports for testing
    assertTrue(
        redisContactPoints.removeIf((InetSocketAddress addr) ->
            addr.getHostName().equals(hostPort.getHost())));
    assertTrue(
        pgsqlContactPoints.removeIf((InetSocketAddress addr) ->
            addr.getHostName().equals(hostPort.getHost())));
    destroyDaemonAndWait(ts);
    usedBindIPs.remove(hostPort.getHost());
  }

  public Map<HostAndPort, MiniYBDaemon> getTabletServers() {
    return tserverProcesses;
  }

  public Map<HostAndPort, MiniYBDaemon> getMasters() {
    return masterProcesses;
  }

  /**
   * Kills the master listening on the provided host/port combination. Doesn't do anything if the
   * master has already been killed.
   * @param hostAndPort host/port the master is listening on
   * @throws InterruptedException
   */
  public void killMasterOnHostPort(HostAndPort hostAndPort) throws Exception {
    MiniYBDaemon master = masterProcesses.remove(hostAndPort);
    if (master == null) {
      // The master is already dead, good.
      return;
    }
    assert(masterHostPorts.remove(hostAndPort));
    updateMasterAddresses();
    destroyDaemonAndWait(master);
  }

  /**
   * See {@link #shutdown()}.
   * @throws Exception never thrown, exceptions are logged
   */
  @Override
  public void close() throws Exception {
    shutdown();
  }

  /**
   * Destroys the given list of YB daemons. Returns a list of processes in case the caller wants
   * to wait for all of them to shut down.
   */
  private List<Process> destroyDaemons(Collection<MiniYBDaemon> daemons) throws Exception {
    List<Process> processes = new ArrayList<>();
    for (Iterator<MiniYBDaemon> iter = daemons.iterator(); iter.hasNext(); ) {
      final MiniYBDaemon daemon = iter.next();
      destroyDaemon(daemon);
      processes.add(daemon.getProcess());
      iter.remove();
    }
    return processes;
  }

  /**
   * Stops all the processes and deletes the folders used to store data and the flagfile.
   */
  public void shutdown() throws Exception {
    LOG.info("Shutting down mini cluster");
    shutdownDaemons();
    String processInfoDir = getProcessInfoDir();
    processCoreFiles(processInfoDir);
    pathsToDelete.add(processInfoDir);
    for (String path : pathsToDelete) {
      try {
        File f = new File(path);
        LOG.info("Deleting path: " + path);
        if (f.isDirectory()) {
          FileUtils.deleteDirectory(f);
        } else {
          f.delete();
        }
      } catch (Exception e) {
        LOG.warn("Could not delete path {}", path, e);
      }
    }
    LOG.info("Mini cluster shutdown finished");
  }

  private void processCoreFiles(String folder) {
    File[] files = (new File(folder)).listFiles();
    for (File file : files == null ? new File[]{} : files) {
      String fileName = file.getAbsolutePath();
      try {
        String exeFile = new String(Files.readAllBytes(Paths.get(fileName)));
        int pid = Integer.parseInt(file.getName());
        CoreFileUtil.processCoreFile(
            pid, exeFile, exeFile, null /* coreFileDir */,
            CoreFileUtil.CoreFileMatchMode.EXACT_PID);
      } catch (Exception e) {
        LOG.warn("Failed to analyze PID from '{}' file", fileName, e);
      }
    }
  }

  private String getProcessInfoDir() {
    Path path = Paths.get(TestUtils.getBaseTmpDir()).resolve("process_info");
    path.toFile().mkdirs();
    return path.toAbsolutePath().toString();
  }

  private void shutdownDaemons() throws Exception {
    List<Process> processes = new ArrayList<>();
    List<MiniYBDaemon> allDaemons = new ArrayList<>();
    allDaemons.addAll(masterProcesses.values());
    allDaemons.addAll(tserverProcesses.values());
    processes.addAll(destroyDaemons(masterProcesses.values()));
    processes.addAll(destroyDaemons(tserverProcesses.values()));
    LOG.info(
        "Waiting for " + processes.size() + " processes to terminate...");
    final long deadlineMs = System.currentTimeMillis() + PROCESS_TERMINATE_TIMEOUT_MS;
    for (Process process : processes) {
      final long timeLeftMs = deadlineMs - System.currentTimeMillis();
      if (timeLeftMs > 0) {
        LOG.info(
            "Waiting for PID " + ProcessUtil.pidStrOfProcess(process) + " for " + timeLeftMs +
            " ms");
        process.waitFor(timeLeftMs, TimeUnit.MILLISECONDS);
        LOG.info("IsAlive: " + process.isAlive());
      } else {
        break;
      }
    }
    boolean isTerminatedGracefully = true;
    for (Process process : processes) {
      if (process.isAlive()) {
        isTerminatedGracefully = false;
        final int pid = ProcessUtil.pidOfProcess(process);
        LOG.info("Trying to kill stuck process " + pid);
        process.destroyForcibly();
        LOG.info(("Waiting for process " + pid + " to terminate ..."));
        if (!process.waitFor(PROCESS_TERMINATE_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
          LOG.warn(
              "Failed to kill process " + pid + " within " + PROCESS_TERMINATE_TIMEOUT_MS + " ms");
        }
      }
    }
    LOG.info("Stopping log printers...");
    for (LogPrinter logPrinter : logPrinters) {
      logPrinter.stop();
    }
    logPrinters.clear();
    for (MiniYBDaemon daemon : allDaemons) {
      daemon.waitForShutdown();
    }
    if (syncClient != null) {
      syncClient.shutdown();
      syncClient = null;
    }
    if (!isTerminatedGracefully) {
      final String errorMessage =
          "Cluster failed to gracefully terminate within " + PROCESS_TERMINATE_TIMEOUT_MS +
          " ms, had to forcibly kill some processes (see logs above).";
      LOG.error(errorMessage);
      fail(errorMessage);
    }
  }

  /**
   * Returns the comma-separated list of master addresses.
   * @return master addresses
   */
  public String getMasterAddresses() {
    return masterAddresses;
  }

  /**
   * Returns a list of master addresses.
   * @return master addresses
   */
  public List<HostAndPort> getMasterHostPorts() {
    return masterHostPorts;
  }

  /**
   * Returns the master address host and port at the given index.
   * @return the master address host/port, if it exists.
   */
  public HostAndPort getMasterHostPort(int index) throws Exception {
    return masterHostPorts.get(index);
  }

  /**
   * Returns the current number of masters.
   * @return count of masters
   */
  public int getNumMasters() {
    return masterHostPorts.size();
  }

  /**
   * Returns the current number of tablet servers.
   * @return count of tablet servers
   */
  public int getNumTServers() {
    return tserverProcesses.size();
  }

  /**
   * Returns the number of shards per tserver.
   * @return number of shards per tserver.
   */
  public int getNumShardsPerTserver() {
    return clusterParameters.numShardsPerTServer;
  }

  /**
   * Returns a list of CQL contact points.
   * @return CQL contact points
   */
  public List<InetSocketAddress> getCQLContactPoints() {
    return cqlContactPoints;
  }

  /**
   * Returns a list of REDIS contact points.
   * @return REDIS contact points
   */
  public List<InetSocketAddress> getRedisContactPoints() {
    return redisContactPoints;
  }

  /**
   * Returns a list of PostgreSQL contact points.
   * @return PostgreSQL contact points
   */
  public List<InetSocketAddress> getPostgresContactPoints() {
    return pgsqlContactPoints;
  }

  /**
   * Returns a comma separated list of CQL contact points.
   * @return CQL contact points
   */
  public String getCQLContactPointsAsString() {
    String cqlContactPoints = "";
    for (InetSocketAddress contactPoint : getCQLContactPoints()) {
      if (!cqlContactPoints.isEmpty()) {
        cqlContactPoints += ",";
      }
      cqlContactPoints += contactPoint.getHostName() + ":" + contactPoint.getPort();
    }
    return cqlContactPoints;
  }

  /**
   * Returns a client to this YB cluster.
   * @return YBClient
   */
  public YBClient getClient() {
    return syncClient;
  }
}
