/**
 * Copyright (c) YugaByte, Inc.
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

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;

public class MiniYBClusterBuilder {

  private MiniYBClusterParameters clusterParameters = new MiniYBClusterParameters();
  private List<String> masterArgs = new ArrayList<>();

  /** Arguments for each tablet server. */
  private List<List<String>> perTServerArgs = new ArrayList<>();

  /** Extra arguments added to each tablet server's command line. */
  private List<String> commonTServerArgs = new ArrayList<>();

  private String testClassName = null;

  private String certFile = null;

  private Map<String, String> tserverEnvVars = new TreeMap<String, String>();

  public MiniYBClusterBuilder numMasters(int numMasters) {
    this.clusterParameters.numMasters = numMasters;
    return this;
  }

  public MiniYBClusterBuilder numTservers(int numTservers) {
    this.clusterParameters.numTservers = numTservers;
    return this;
  }

  public MiniYBClusterBuilder numShardsPerTServer(int numShardsPerTServer) {
    this.clusterParameters.numShardsPerTServer = numShardsPerTServer;
    return this;
  }

  public MiniYBClusterBuilder useIpWithCertificate(boolean value) {
    this.clusterParameters.useIpWithCertificate = value;
    return this;
  }

  public MiniYBClusterBuilder sslCertFile(String certFile) {
    this.certFile = certFile;
    return this;
  }

  /**
   * Configures the internal client to use the given timeout for all operations. Also uses the
   * timeout for tasks like waiting for tablet servers to check in with the master.
   * @param defaultTimeoutMs timeout in milliseconds
   * @return this instance
   */
  public MiniYBClusterBuilder defaultTimeoutMs(int defaultTimeoutMs) {
    this.clusterParameters.defaultTimeoutMs = defaultTimeoutMs;
    return this;
  }

  /**
   * Configure additional command-line arguments for starting master. This replaces the list of
   * existing additional master arguments.
   *
   * @param masterArgs additional command-line arguments
   * @return this instance
   */
  public MiniYBClusterBuilder masterArgs(List<String> masterArgs) {
    this.masterArgs = masterArgs;
    return this;
  }

  /**
   * Configure additional command-line arguments for starting master. This appends to the list of
   * additional master arguments.
   */
  public MiniYBClusterBuilder addMasterArgs(String... newArgs) {
    this.masterArgs.addAll(Arrays.asList(newArgs));
    return this;
  }


  /**
   * Configure additional command-line arguments for starting tserver.
   */
  public MiniYBClusterBuilder perTServerArgs(List<List<String>> tserverArgs) {
    this.perTServerArgs = tserverArgs;
    return this;
  }

  public MiniYBClusterBuilder commonTServerArgs(List<String> commonTServerArgs) {
    this.commonTServerArgs = commonTServerArgs;
    return this;
  }

  public MiniYBClusterBuilder addCommonTServerArgs(String... newArgs) {
    if (this.commonTServerArgs == null) {
      this.commonTServerArgs = new ArrayList<>();
    }
    this.commonTServerArgs.addAll(Arrays.asList(newArgs));
    return this;
  }

  /**
   * Configure additional command-line arguments for starting both master and tserver.
   */
  public MiniYBClusterBuilder addCommonArgs(String... args) {
    addMasterArgs(args);
    addCommonTServerArgs(args);
    return this;
  }

  /**
   * Sets test class name that this mini cluster is created for. We're including this in daemon
   * command line to be able to identify stuck master/tablet server processes later.
   */
  public MiniYBClusterBuilder testClassName(String testClassName) {
    this.testClassName = testClassName;
    return this;
  }

  /**
   * Sets the replication factor for the mini-cluster.
   */
  public MiniYBClusterBuilder replicationFactor(int replicationFactor) {
    this.clusterParameters.replicationFactor = replicationFactor;
    return this;
  }

  /**
   * Enable PostgreSQL server API in tablet servers.
   */
  public MiniYBClusterBuilder enablePostgres(boolean enablePostgres) {
    this.clusterParameters.startPgSqlProxy = enablePostgres;
    return this;
  }

  /**
   * Enable PostgreSQL server API in tablet servers.
   */
  public MiniYBClusterBuilder enablePgTransactions(boolean enablePgTransactions) {
    if (enablePgTransactions) {
      enablePostgres(true);
    }
    this.clusterParameters.pgTransactionsEnabled = enablePgTransactions;
    return this;
  }

  public MiniYBClusterBuilder tserverHeartbeatTimeoutMs(final int tserverHeartbeatTimeoutMs) {
    this.clusterParameters.tserverHeartbeatTimeoutMsOpt = Optional.of(tserverHeartbeatTimeoutMs);
    return this;
  }

  public MiniYBClusterBuilder yqlSystemPartitionsVtableRefreshSecs(
      final int yqlSystemPartitionsVtableRefreshSecs) {
    this.clusterParameters.yqlSystemPartitionsVtableRefreshSecsOpt =
        Optional.of(yqlSystemPartitionsVtableRefreshSecs);
    return this;
  }

  /**
   * Add environment variables.
   */
  public MiniYBClusterBuilder addEnvironmentVariables(Map<String, String> tserverEnvVars) {
    this.tserverEnvVars.putAll(tserverEnvVars);
    return this;
  }

  public MiniYBCluster build() throws Exception {
    if (perTServerArgs != null && perTServerArgs.size() != clusterParameters.numTservers) {
      throw new AssertionError(
          "Per-tablet-server arguments list has " + perTServerArgs.size() + " elements (" +
              perTServerArgs + ") but numTServers=" + clusterParameters.numTservers);
    }

    return new MiniYBCluster(
        clusterParameters,
        masterArgs,
        perTServerArgs,
        commonTServerArgs,
        tserverEnvVars,
        testClassName,
        certFile);
  }
}
