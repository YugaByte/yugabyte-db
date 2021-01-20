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

import org.yb.annotations.InterfaceAudience;
import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.master.Master;


@InterfaceAudience.Public
public abstract class AbstractModifyMasterClusterConfig {
  private YBClient ybClient = null;

  public AbstractModifyMasterClusterConfig(YBClient client) {
    ybClient = client;
  }

  private Master.SysClusterConfigEntryPB getConfig() throws Exception {
    GetMasterClusterConfigResponse getResponse = ybClient.getMasterClusterConfig();
    if (getResponse.hasError()) {
      throw new RuntimeException("Get config hit error: " + getResponse.errorMessage());
    }

    return getResponse.getConfig();
  }

  public Master.SysClusterConfigEntryPB doCall() throws Exception {
    Master.SysClusterConfigEntryPB newConfig = modifyConfig(getConfig());
    ChangeMasterClusterConfigResponse changeResp = ybClient.changeMasterClusterConfig(newConfig);
    if (changeResp.hasError()) {
      throw new RuntimeException("ChangeConfig hit error: " + changeResp.errorMessage());
    }

    return getConfig();
  }

  abstract protected Master.SysClusterConfigEntryPB modifyConfig(
    Master.SysClusterConfigEntryPB config);
}
