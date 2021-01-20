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

package org.yb.client;

import org.yb.master.Master;

public class ModifyClusterConfigIncrementVersion extends AbstractModifyMasterClusterConfig {
  // The current version of the cluster config
  // (RPC will fail if this does does not match current version in cluster config)
  int version;

  public ModifyClusterConfigIncrementVersion(YBClient client, int version) {
    super(client);
    this.version = version;
  }

  @Override
  public Master.SysClusterConfigEntryPB modifyConfig(Master.SysClusterConfigEntryPB config) {
    Master.SysClusterConfigEntryPB.Builder builder =
      Master.SysClusterConfigEntryPB.newBuilder(config);
    if (version > 0) {
      builder = builder.setVersion(version);
    }

    return builder.build();
  }

  /**
   * Increment the cluster config version without changing anything else in the config
   *
   * @return the version of the cluster config after being incremented
   * @throws Exception if the version provided to this class does not match the current cluster
   * config version
   */
  public int incrementVersion() throws Exception {
    return doCall().getVersion();
  }
}
