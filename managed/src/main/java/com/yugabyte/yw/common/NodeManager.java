/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common;

import com.google.common.collect.ImmutableList;
import com.google.inject.Inject;
import com.google.inject.Singleton;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.UpgradeUniverse;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.commissioner.tasks.subtasks.AnsibleClusterServerCtl;
import com.yugabyte.yw.commissioner.tasks.subtasks.AnsibleConfigureServers;
import com.yugabyte.yw.commissioner.tasks.subtasks.AnsibleDestroyServer;
import com.yugabyte.yw.commissioner.tasks.subtasks.AnsibleSetupServer;
import com.yugabyte.yw.commissioner.tasks.subtasks.InstanceActions;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;

import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.CertificateInfo;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.DeviceInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.*;

@Singleton
public class NodeManager extends DevopsBase {
  private static final String YB_CLOUD_COMMAND_TYPE = "instance";
  private static final List<String> VALID_CONFIGURE_PROCESS_TYPES = ImmutableList.of(
      ServerType.MASTER.name(),
      ServerType.TSERVER.name());

  @Inject
  ReleaseManager releaseManager;

  @Override
  protected String getCommandType() {
    return YB_CLOUD_COMMAND_TYPE;
  }

  // Currently we need to define the enum such that the lower case value matches the action
  public enum NodeCommandType {
    Provision,
    Configure,
    Destroy,
    List,
    Control,
    Tags,
    InitYSQL,
    Disk_Update
  }
  public static final Logger LOG = LoggerFactory.getLogger(NodeManager.class);

  @Inject
  play.Configuration appConfig;

  private UserIntent getUserIntentFromParams(NodeTaskParams nodeTaskParam) {
    Universe universe = Universe.get(nodeTaskParam.universeUUID);
    NodeDetails nodeDetails = universe.getNode(nodeTaskParam.nodeName);
    if (nodeDetails == null) {
      nodeDetails = universe.getUniverseDetails().nodeDetailsSet.iterator().next();
      LOG.info("Node {} not found, so using {}.", nodeTaskParam.nodeName, nodeDetails.nodeName);
    }
    return universe.getUniverseDetails()
                   .getClusterByUuid(nodeDetails.placementUuid)
                   .userIntent;
  }

  private List<String> getCloudArgs(NodeTaskParams nodeTaskParam) {
    List<String> command = new ArrayList<String>();
    command.add("--zone");
    command.add(nodeTaskParam.getAZ().code);
    UserIntent userIntent = getUserIntentFromParams(nodeTaskParam);

    // Right now for docker we grab the network from application conf.
    if (userIntent.providerType.equals(Common.CloudType.docker)) {
      String networkName = appConfig.getString("yb.docker.network");
      if (networkName == null) {
        throw new RuntimeException("yb.docker.network is not set in application.conf");
      }
      command.add("--network");
      command.add(networkName);
    }

    if (userIntent.providerType.equals(Common.CloudType.onprem)) {
      NodeInstance node = NodeInstance.getByName(nodeTaskParam.nodeName);
      command.add("--node_metadata");
      command.add(node.getDetailsJson());
    }
    return command;
  }

  private List<String> getAccessKeySpecificCommand(NodeTaskParams params, NodeCommandType type) {
    List<String> subCommand = new ArrayList<>();
    if (params.universeUUID == null) {
      throw new RuntimeException("NodeTaskParams missing Universe UUID.");
    }
    UserIntent userIntent = getUserIntentFromParams(params);

    // TODO: [ENG-1242] we shouldn't be using our keypair, until we fix our VPC to support VPN
    if (userIntent != null && !userIntent.accessKeyCode.equalsIgnoreCase("yugabyte-default")) {
      AccessKey accessKey = AccessKey.get(params.getProvider().uuid, userIntent.accessKeyCode);
      AccessKey.KeyInfo keyInfo = accessKey.getKeyInfo();
      if (keyInfo.vaultFile != null) {
        subCommand.add("--vars_file");
        subCommand.add(keyInfo.vaultFile);
        subCommand.add("--vault_password_file");
        subCommand.add(keyInfo.vaultPasswordFile);
      }
      if (keyInfo.privateKey != null) {
        subCommand.add("--private_key_file");
        subCommand.add(keyInfo.privateKey);

        // We only need to include keyPair name for setup server call and if this is aws.
        if (params instanceof AnsibleSetupServer.Params && userIntent.providerType.equals(Common.CloudType.aws)) {
          subCommand.add("--key_pair_name");
          subCommand.add(userIntent.accessKeyCode);
          // Also we will add the security group information.
          Region r = params.getRegion();
          String customSecurityGroupId = r.getSecurityGroupId();
          if (customSecurityGroupId != null) {
            subCommand.add("--security_group_id");
            subCommand.add(customSecurityGroupId);
          }
        }
      }
      if (params instanceof AnsibleSetupServer.Params &&
          userIntent.providerType.equals(Common.CloudType.azu)) {
        Region r = params.getRegion();
        String customSecurityGroupId = r.getSecurityGroupId();
          if (customSecurityGroupId != null) {
            subCommand.add("--security_group_id");
            subCommand.add(customSecurityGroupId);
          }
      }
      subCommand.add("--custom_ssh_port");
      subCommand.add(keyInfo.sshPort.toString());

      if (type == NodeCommandType.Provision && keyInfo.sshUser != null) {
        subCommand.add("--ssh_user");
        subCommand.add(keyInfo.sshUser);
      }

      if (params instanceof AnsibleSetupServer.Params &&
          accessKey.getKeyInfo().airGapInstall) {
        subCommand.add("--air_gap");
      }
    }

    return subCommand;
  }

  private List<String> getDeviceArgs(NodeTaskParams params) {
    List<String> args = new ArrayList<>();
    if (params.deviceInfo.numVolumes != null && !params.getProvider().code.equals("onprem")) {
      args.add("--num_volumes");
      args.add(Integer.toString(params.deviceInfo.numVolumes));
    } else if (params.deviceInfo.mountPoints != null)  {
      args.add("--mount_points");
      args.add(params.deviceInfo.mountPoints);
    }
    if (params.deviceInfo.volumeSize != null) {
      args.add("--volume_size");
      args.add(Integer.toString(params.deviceInfo.volumeSize));
    }
    return args;
  }

  private String getThirdpartyPackagePath() {
    String packagePath = appConfig.getString("yb.thirdparty.packagePath");
    if (packagePath != null && !packagePath.isEmpty()) {
      File thirdpartyPackagePath = new File(packagePath);
      if (thirdpartyPackagePath.exists() && thirdpartyPackagePath.isDirectory()) {
        return packagePath;
      }
    }

    return null;
  }

  private List<String> getConfigureSubCommand(AnsibleConfigureServers.Params taskParam) {
    UserIntent userIntent = getUserIntentFromParams(taskParam);
    List<String> subcommand = new ArrayList<String>();
    Universe universe = Universe.get(taskParam.universeUUID);
    String masterAddresses = universe.getMasterAddresses(false);
    subcommand.add("--master_addresses_for_tserver");
    subcommand.add(masterAddresses);

    if (masterAddresses == null || masterAddresses.isEmpty()) {
      LOG.warn("No valid masters found during configure for {}.", taskParam.universeUUID);
    }

    if (!taskParam.isMasterInShellMode) {
      subcommand.add("--master_addresses_for_master");
      subcommand.add(masterAddresses);
    }

    String ybServerPackage = null;
    if (taskParam.ybSoftwareVersion != null) {
      ReleaseManager.ReleaseMetadata releaseMetadata =
          releaseManager.getReleaseByVersion(taskParam.ybSoftwareVersion);
      if (releaseMetadata != null) {
        ybServerPackage = releaseMetadata.filePath;
      }
    }

    if (!taskParam.itestS3PackagePath.isEmpty()
        && userIntent.providerType.equals(Common.CloudType.aws)) {
      subcommand.add("--itest_s3_package_path");
      subcommand.add(taskParam.itestS3PackagePath);
    }

    NodeDetails node = universe.getNode(taskParam.nodeName);

    // Pass in communication ports
    subcommand.add("--master_http_port");
    subcommand.add(Integer.toString(node.masterHttpPort));
    subcommand.add("--master_rpc_port");
    subcommand.add(Integer.toString(node.masterRpcPort));
    subcommand.add("--tserver_http_port");
    subcommand.add(Integer.toString(node.tserverHttpPort));
    subcommand.add("--tserver_rpc_port");
    subcommand.add(Integer.toString(node.tserverRpcPort));
    subcommand.add("--cql_proxy_rpc_port");
    subcommand.add(Integer.toString(node.yqlServerRpcPort));
    subcommand.add("--redis_proxy_rpc_port");
    subcommand.add(Integer.toString(node.redisServerRpcPort));

    switch(taskParam.type) {
      case Everything:
        boolean useHostname = universe.getUniverseDetails()
          .getPrimaryCluster().userIntent.useHostname;
        if (ybServerPackage == null) {
          throw new RuntimeException("Unable to fetch yugabyte release for version: " +
              taskParam.ybSoftwareVersion);
        }
        subcommand.add("--package");
        subcommand.add(ybServerPackage);
        Map<String, String> extra_gflags = new HashMap<>();
        extra_gflags.put("undefok", "enable_ysql");
        if (taskParam.isMaster) {
          extra_gflags.put("cluster_uuid", String.valueOf(taskParam.universeUUID));
          extra_gflags.put("replication_factor", String.valueOf(userIntent.replicationFactor));
        }
        extra_gflags.put("placement_uuid", String.valueOf(taskParam.placementUuid));
        // Add in the nodeName during configure.
        extra_gflags.put("metric_node_name", taskParam.nodeName);
        // TODO: add a shared path to massage flags across different flavors of configure.
        String pgsqlProxyBindAddress = node.cloudInfo.private_ip;

        if (useHostname) {
          subcommand.add("--server_broadcast_addresses");
          subcommand.add(node.cloudInfo.private_ip);
          pgsqlProxyBindAddress = "0.0.0.0";
        }
        if (taskParam.enableYSQL) {
          extra_gflags.put("enable_ysql", "true");
          extra_gflags.put("pgsql_proxy_bind_address", String.format(
            "%s:%s", pgsqlProxyBindAddress, node.ysqlServerRpcPort
          ));
        } else {
          extra_gflags.put("enable_ysql", "false");
        }
        if (taskParam.enableNodeToNodeEncrypt || taskParam.enableClientToNodeEncrypt) {
          CertificateInfo cert = CertificateInfo.get(taskParam.rootCA);
          if (cert == null) {
            throw new RuntimeException("No valid rootCA found for " + taskParam.universeUUID);
          }
          if (taskParam.enableNodeToNodeEncrypt) extra_gflags.put("use_node_to_node_encryption", "true");
          if (taskParam.enableClientToNodeEncrypt) extra_gflags.put("use_client_to_server_encryption", "true");
          extra_gflags.put("allow_insecure_connections", taskParam.allowInsecure ? "true" : "false");
          String yb_home_dir = taskParam.getProvider().getYbHome();
          // TODO: This directory location should also be passed into subcommand: --certs_node_dir
          extra_gflags.put("certs_dir", yb_home_dir + "/yugabyte-tls-config");
          subcommand.add("--rootCA_cert");
          subcommand.add(cert.certificate);
          subcommand.add("--rootCA_key");
          subcommand.add(cert.privateKey);
          if (taskParam.enableClientToNodeEncrypt) {
            subcommand.add("--client_cert");
            subcommand.add(CertificateHelper.getClientCertFile(taskParam.rootCA));
            subcommand.add("--client_key");
            subcommand.add(CertificateHelper.getClientKeyFile(taskParam.rootCA));
          }
        }
        if (taskParam.callhomeLevel != null){
          extra_gflags.put("callhome_collection_level", taskParam.callhomeLevel.toString().toLowerCase());
          if(taskParam.callhomeLevel.toString().equals("NONE")){
            extra_gflags.put("callhome_enabled", "false");
          }
        }
        subcommand.add("--extra_gflags");
        subcommand.add(Json.stringify(Json.toJson(extra_gflags)));
        break;
      case Software:
        {
          if (ybServerPackage == null) {
            throw new RuntimeException("Unable to fetch yugabyte release for version: " +
                taskParam.ybSoftwareVersion);
          }
          subcommand.add("--package");
          subcommand.add(ybServerPackage);
          String processType = taskParam.getProperty("processType");
          if (processType == null || !VALID_CONFIGURE_PROCESS_TYPES.contains(processType)) {
            throw new RuntimeException("Invalid processType: " + processType);
          } else {
            subcommand.add("--yb_process_type");
            subcommand.add(processType.toLowerCase());
          }
          String taskSubType = taskParam.getProperty("taskSubType");
          if (taskSubType == null) {
            throw new RuntimeException("Invalid taskSubType property: " + taskSubType);
          } else if (taskSubType.equals(UpgradeUniverse.UpgradeTaskSubType.Download.toString())) {
            subcommand.add("--tags");
            subcommand.add("download-software");
          } else if (taskSubType.equals(UpgradeUniverse.UpgradeTaskSubType.Install.toString())) {
            subcommand.add("--tags");
            subcommand.add("install-software");
          }
          Map<String, String> gflags = new HashMap<>();
          gflags.put("placement_uuid", String.valueOf(taskParam.placementUuid));
          subcommand.add("--extra_gflags");
          subcommand.add(Json.stringify(Json.toJson(gflags)));
        }
        break;
      case GFlags:
        {
          if (!taskParam.updateMasterAddrsOnly &&
              (taskParam.gflags == null || taskParam.gflags.isEmpty()) &&
              (taskParam.gflagsToRemove == null || taskParam.gflagsToRemove.isEmpty())) {
            throw new RuntimeException("GFlags data provided for " +
                                       taskParam.nodeName + "'s " +
                                       taskParam.getProperty("processType") + " process" +
                                        " have no changes from existing flags.");
          }

          String processType = taskParam.getProperty("processType");
          if (processType == null || !VALID_CONFIGURE_PROCESS_TYPES.contains(processType)) {
            throw new RuntimeException("Invalid processType: " + processType);
          } else {
            subcommand.add("--yb_process_type");
            subcommand.add(processType.toLowerCase());
          }
          subcommand.add("--replace_gflags");

          // Add in the nodeName during configure.
          Map<String, String> gflags = new HashMap<>(taskParam.gflags);

          if (taskParam.updateMasterAddrsOnly) {
            if (processType.equals(ServerType.MASTER.name())) {
              gflags.put("master_addresses", masterAddresses);
            } else {
              gflags.put("tserver_master_addrs", masterAddresses);
            }

          } else {
            gflags.put("placement_uuid", String.valueOf(taskParam.placementUuid));
            gflags.put("metric_node_name", taskParam.nodeName);
          }
          subcommand.add("--gflags");
          subcommand.add(Json.stringify(Json.toJson(gflags)));

          if (taskParam.gflagsToRemove != null && !taskParam.gflagsToRemove.isEmpty()) {
            subcommand.add("--gflags_to_remove");
            subcommand.add(Json.stringify(Json.toJson(taskParam.gflagsToRemove)));
          }
        }
        break;
    }
    return subcommand;
  }

  public ShellProcessHandler.ShellResponse nodeCommand(NodeCommandType type,
                                                       NodeTaskParams nodeTaskParam) throws RuntimeException {
    List<String> commandArgs = new ArrayList<>();
    UserIntent userIntent = getUserIntentFromParams(nodeTaskParam);
    switch (type) {
      case Provision: {
        if (!(nodeTaskParam instanceof AnsibleSetupServer.Params)) {
          throw new RuntimeException("NodeTaskParams is not AnsibleSetupServer.Params");
        }
        AnsibleSetupServer.Params taskParam = (AnsibleSetupServer.Params) nodeTaskParam;
        Common.CloudType cloudType = userIntent.providerType;
        if (!cloudType.equals(Common.CloudType.onprem)) {
          commandArgs.add("--instance_type");
          commandArgs.add(taskParam.instanceType);
          commandArgs.add("--cloud_subnet");
          commandArgs.add(taskParam.subnetId);

          // For now we wouldn't add machine image for aws and fallback on the default
          // one devops gives us, we need to transition to having this use versioning
          // like base_image_version [ENG-1859]
          String ybImage = taskParam.getRegion().ybImage;
          if (ybImage != null && !ybImage.isEmpty()) {
            commandArgs.add("--machine_image");
            commandArgs.add(ybImage);
          }
          /*
          // TODO(bogdan): talk to Ram about this, if we want/use it for kube/onprem?
          if (!cloudType.equals(Common.CloudType.aws) && !cloudType.equals(Common.CloudType.gcp)) {
            commandArgs.add("--machine_image");
            commandArgs.add(taskParam.getRegion().ybImage);
          }
          */
          if (taskParam.assignPublicIP) {
            commandArgs.add("--assign_public_ip");
          }
        }

        commandArgs.add("--node_exporter_port");
        commandArgs.add(Integer.toString(taskParam.communicationPorts.nodeExporterPort));

        commandArgs.add("--install_node_exporter");
        commandArgs.add(Boolean.toString(taskParam.extraDependencies.installNodeExporter));

        if (cloudType.equals(Common.CloudType.aws)) {
          if (taskParam.useTimeSync) {
            commandArgs.add("--use_chrony");
          }
          if (userIntent.instanceTags != null && !userIntent.instanceTags.isEmpty()) {
            Map<String, String> useTags = userIntent.getInstanceTagsForInstanceOps();
            commandArgs.add("--instance_tags");
            commandArgs.add(Json.stringify(Json.toJson(useTags)));
          }
          if (taskParam.cmkArn != null) {
            commandArgs.add("--cmk_res_name");
            commandArgs.add(taskParam.cmkArn);
          }
          if (taskParam.ipArnString != null) {
            commandArgs.add("--iam_profile_arn");
            commandArgs.add(taskParam.ipArnString);
          }
        }
        if (cloudType.equals(Common.CloudType.azu)) {
          Region r = taskParam.getRegion();
          String vnetName = r.getVnetName();
          if (vnetName != null && !vnetName.isEmpty()) {
            commandArgs.add("--vpcId");
            commandArgs.add(vnetName);
          }
        }
        commandArgs.addAll(getAccessKeySpecificCommand(taskParam, type));
        if (nodeTaskParam.deviceInfo != null) {
          commandArgs.addAll(getDeviceArgs(nodeTaskParam));
          DeviceInfo deviceInfo = nodeTaskParam.deviceInfo;
          if (deviceInfo.storageType != null) {
            commandArgs.add("--volume_type");
            commandArgs.add(deviceInfo.storageType.toString().toLowerCase());
            if (deviceInfo.storageType.equals(PublicCloudConstants.StorageType.IO1) &&
                deviceInfo.diskIops != null) {
              commandArgs.add("--disk_iops");
              commandArgs.add(Integer.toString(deviceInfo.diskIops));
            }
          }
        }

        String localPackagePath = getThirdpartyPackagePath();
        if (localPackagePath != null) {
          commandArgs.add("--local_package_path");
          commandArgs.add(localPackagePath);
        }

        break;
      }
      case Configure: {
        if (!(nodeTaskParam instanceof AnsibleConfigureServers.Params)) {
          throw new RuntimeException("NodeTaskParams is not AnsibleConfigureServers.Params");
        }
        AnsibleConfigureServers.Params taskParam = (AnsibleConfigureServers.Params) nodeTaskParam;
        commandArgs.addAll(getConfigureSubCommand(taskParam));
        commandArgs.addAll(getAccessKeySpecificCommand(taskParam, type));
        if (nodeTaskParam.deviceInfo != null) {
          commandArgs.addAll(getDeviceArgs(nodeTaskParam));
        }
        break;
      }
      case List: {
        commandArgs.add("--as_json");
        break;
      }
      case Destroy: {
        if (!(nodeTaskParam instanceof AnsibleDestroyServer.Params)) {
          throw new RuntimeException("NodeTaskParams is not AnsibleDestroyServer.Params");
        }
        commandArgs.add("--instance_type");
        commandArgs.add(nodeTaskParam.instanceType);
        if (nodeTaskParam.deviceInfo != null) {
          commandArgs.addAll(getDeviceArgs(nodeTaskParam));
        }
        commandArgs.addAll(getAccessKeySpecificCommand(nodeTaskParam, type));
        break;
      }
      case Control: {
        if (!(nodeTaskParam instanceof AnsibleClusterServerCtl.Params)) {
          throw new RuntimeException("NodeTaskParams is not AnsibleClusterServerCtl.Params");
        }
        AnsibleClusterServerCtl.Params taskParam = (AnsibleClusterServerCtl.Params) nodeTaskParam;
        commandArgs.add(taskParam.process);
        commandArgs.add(taskParam.command);
        commandArgs.addAll(getAccessKeySpecificCommand(taskParam, type));
        break;
      }
      case Tags: {
        if (!(nodeTaskParam instanceof InstanceActions.Params)) {
          throw new RuntimeException("NodeTaskParams is not InstanceActions.Params");
        }
        InstanceActions.Params taskParam = (InstanceActions.Params) nodeTaskParam;
        if (userIntent.providerType.equals(Common.CloudType.aws)) {
          if (userIntent.instanceTags == null || userIntent.instanceTags.isEmpty()) {
            throw new RuntimeException("Invalid instance tags");
          }
          Map<String, String> useTags = userIntent.getInstanceTagsForInstanceOps();
          commandArgs.add("--instance_tags");
          commandArgs.add(Json.stringify(Json.toJson(useTags)));
          if (!taskParam.deleteTags.isEmpty()) {
            commandArgs.add("--remove_tags");
            commandArgs.add(taskParam.deleteTags);
          }
        }
        break;
      }
      case Disk_Update: {
        if (!(nodeTaskParam instanceof InstanceActions.Params)) {
          throw new RuntimeException("NodeTaskParams is not InstanceActions.Params");
        }
        InstanceActions.Params taskParam = (InstanceActions.Params) nodeTaskParam;
        commandArgs.addAll(getAccessKeySpecificCommand(taskParam, type));
        commandArgs.add("--instance_type");
        commandArgs.add(taskParam.instanceType);
        if (taskParam.deviceInfo != null) {
          commandArgs.addAll(getDeviceArgs(taskParam));
        }
        break;
      }
    }
    commandArgs.add(nodeTaskParam.nodeName);

    return execCommand(nodeTaskParam.getRegion().uuid, null, null, type.toString().toLowerCase(),
      commandArgs, getCloudArgs(nodeTaskParam));
  }
}
