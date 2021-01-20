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

import java.util.Optional;

/**
 * Contains common cluster parameters to avoid code duplication between MiniYBCluster and
 * MiniYBClusterBuilder.
 */
public class MiniYBClusterParameters {
  public static final int DEFAULT_NUM_MASTERS = 3;
  public static final int DEFAULT_NUM_TSERVERS = 3;
  public static final int DEFAULT_NUM_SHARDS_PER_TSERVER = 3;
  public static boolean DEFAULT_USE_IP_WITH_CERTIFICATE = false;

  public int numMasters = 1;
  public int numTservers = DEFAULT_NUM_TSERVERS;
  public int numShardsPerTServer = DEFAULT_NUM_SHARDS_PER_TSERVER;
  public boolean useIpWithCertificate = DEFAULT_USE_IP_WITH_CERTIFICATE;
  public int defaultTimeoutMs = 50000;
  public int replicationFactor = -1;
  public boolean startPgSqlProxy = false;
  public boolean pgTransactionsEnabled = false;
  public Optional<Integer> tserverHeartbeatTimeoutMsOpt = Optional.empty();
  public Optional<Integer> yqlSystemPartitionsVtableRefreshSecsOpt = Optional.empty();

  public int getTServerHeartbeatTimeoutMs() {
    return tserverHeartbeatTimeoutMsOpt.get();
  }

  public int getYQLSystemPartitionsVtableRefreshSecs() {
    return yqlSystemPartitionsVtableRefreshSecsOpt.get();
  }
}
