/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.SubTaskGroup;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.tasks.params.CloudTaskParams;
import com.yugabyte.yw.commissioner.tasks.subtasks.cloud.CloudAccessKeySetup;
import com.yugabyte.yw.commissioner.tasks.subtasks.cloud.CloudInitializer;
import com.yugabyte.yw.commissioner.tasks.subtasks.cloud.CloudRegionSetup;
import com.yugabyte.yw.commissioner.tasks.subtasks.cloud.CloudSetup;
import com.yugabyte.yw.models.Provider;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import play.libs.Json;

public class CloudBootstrap extends CloudTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(CloudBootstrap.class);

  public static class Params extends CloudTaskParams {
    // Class to encapsulate custom network bootstrap overrides per region.
    public static class PerRegionMetadata {
      // Custom VPC ID to use for this region
      // Default: created by YB.
      // Required: True for custom input, False for YW managed.
      public String vpcId;

      // Custom CIDR to use for the VPC, if YB is creating it.
      // Default: chosen by YB.
      // Required: False.
      public String vpcCidr;

      // Custom map from AZ name to Subnet ID for AWS.
      // Default: created by YB.
      // Required: True for custom input, False for YW managed.
      public Map<String, String> azToSubnetIds;

      // Region Subnet ID for GCP.
      // Default: created by YB.
      // Required: True for custom input, False for YW managed.
      public String subnetId;

      // TODO(bogdan): does this not need a custom SSH user as well???
      // Custom AMI ID to use for YB nodes.
      // Default: hardcoded in devops.
      // Required: False.
      public String customImageId;

      // Custom SG ID to use for the YB nodes.
      // Default: created by YB.
      // Required: True for custom input, False for YW managed.
      public String customSecurityGroupId;
    }

    // Map from region name to metadata.
    public Map<String, PerRegionMetadata> perRegionMetadata = new HashMap<>();

    // Custom keypair name to use when spinning up YB nodes.
    // Default: created and managed by YB.
    public String keyPairName = null;

    // Custom SSH private key component.
    // Default: created and managed by YB.
    public String sshPrivateKeyContent = null;

    // Custom SSH user to login to machines.
    // Default: created and managed by YB.
    public String sshUser = null;

    // Whether provider should use airgapped install.
    // Default: false.
    public boolean airGapInstall = false;

    // Port to open for connections on the instance.
    public Integer sshPort = 54422;

    public String hostVpcId = null;
    public String hostVpcRegion = null;
    public List<String> customHostCidrs = new ArrayList<>();
    // TODO(bogdan): only used/needed for GCP.
    public String destVpcId = null;
  }

  @Override
  protected Params taskParams() { return (Params) taskParams; }

  @Override
  public void run() {
    subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
    Provider p = Provider.get(taskParams().providerUUID);
    if (p.code.equals(Common.CloudType.gcp.toString())
        || p.code.equals(Common.CloudType.aws.toString())
        || p.code.equals(Common.CloudType.azu.toString())) {
      createCloudSetupTask()
        .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.BootstrappingCloud);
    }
    taskParams().perRegionMetadata.forEach((regionCode, metadata) -> {
      createRegionSetupTask(regionCode, metadata)
        .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.BootstrappingRegion);
    });
    taskParams().perRegionMetadata.forEach((regionCode, metadata) -> {
      createAccessKeySetupTask(regionCode).setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.CreateAccessKey);
    });

    createInitializerTask()
        .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.InitializeCloudMetadata);

    subTaskGroupQueue.run();
  }

  public SubTaskGroup createCloudSetupTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("Create Cloud setup task", executor);

    CloudSetup.Params params = new CloudSetup.Params();
    params.providerUUID = taskParams().providerUUID;
    params.customPayload = Json.stringify(Json.toJson(taskParams()));
    CloudSetup task = new CloudSetup();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createRegionSetupTask(
      String regionCode, Params.PerRegionMetadata metadata) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("Create Region task", executor);

    CloudRegionSetup.Params params = new CloudRegionSetup.Params();
    params.providerUUID = taskParams().providerUUID;
    params.regionCode = regionCode;
    params.metadata = metadata;
    params.destVpcId = taskParams().destVpcId;

    CloudRegionSetup task = new CloudRegionSetup();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createAccessKeySetupTask(String regionCode) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("Create Access Key", executor);
    CloudAccessKeySetup.Params params = new CloudAccessKeySetup.Params();
    params.providerUUID = taskParams().providerUUID;
    params.regionCode = regionCode;
    params.keyPairName = taskParams().keyPairName;
    params.sshPrivateKeyContent = taskParams().sshPrivateKeyContent;
    params.sshUser = taskParams().sshUser;
    params.sshPort = taskParams().sshPort;
    params.airGapInstall = taskParams().airGapInstall;
    CloudAccessKeySetup task = new CloudAccessKeySetup();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createInitializerTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("Create Cloud initializer task", executor);
    CloudInitializer.Params params = new CloudInitializer.Params();
    params.providerUUID = taskParams().providerUUID;
    CloudInitializer task = new CloudInitializer();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }
}
