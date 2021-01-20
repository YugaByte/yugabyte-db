/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Provider;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;

import java.util.List;


public class AnsibleSetupServer extends NodeTaskBase {

  public static final Logger LOG = LoggerFactory.getLogger(AnsibleSetupServer.class);

  // Additional parameters for this task.
  public static class Params extends NodeTaskParams {
    // The VPC into which the node is to be provisioned.
    public String subnetId;

    public boolean assignPublicIP = true;

    // For AWS, this will dictate if we use the Time Sync Service.
    public boolean useTimeSync = false;

    // If this is set to the universe's AWS KMS CMK arn, AWS EBS volume
    // encryption will be enabled
    public String cmkArn;

    // If set, we will use this Amazon Resource Name of the user's
    // instance profile instead of an access key id and secret
    public String ipArnString;
  }

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public void run() {
    Provider p = taskParams().getProvider();
    List<AccessKey> accessKeys = AccessKey.getAll(p.uuid);
    boolean skipProvision = false;

    // For now we will skipProvision if it's set in accessKeys.
    if (p.code.equals(Common.CloudType.onprem.name()) && accessKeys.size() > 0) {
      skipProvision = accessKeys.get(0).getKeyInfo().skipProvisioning;
    }

    if (skipProvision) {
      LOG.info("Skipping ansible provision.");
    } else {
      // Execute the ansible command.
      ShellResponse response = getNodeManager().nodeCommand(
          NodeManager.NodeCommandType.Provision, taskParams());
      processShellResponse(response);
    }
  }
}
