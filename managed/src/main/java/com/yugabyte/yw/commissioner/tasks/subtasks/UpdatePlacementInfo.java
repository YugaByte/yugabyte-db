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

import java.util.List;
import java.util.Set;
import java.util.UUID;

import com.yugabyte.yw.commissioner.tasks.UniverseTaskBase;
import com.yugabyte.yw.forms.UniverseTaskParams;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.Common;
import org.yb.client.AbstractModifyMasterClusterConfig;
import org.yb.client.ProtobufHelper;
import org.yb.client.YBClient;
import org.yb.master.Master;

import com.google.common.net.HostAndPort;
import com.google.protobuf.ByteString;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementAZ;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementCloud;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementRegion;

import play.api.Play;

public class UpdatePlacementInfo extends UniverseTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(UpdatePlacementInfo.class);

  // The YB client.
  public YBClientService ybService = null;

  // Parameters for placement info update task.
  public static class Params extends UniverseTaskParams {
    // If present, then we intend to decommission these nodes.
    public Set<String> blacklistNodes = null;
  }

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public void initialize(ITaskParams params) {
    super.initialize(params);
    ybService = Play.current().injector().instanceOf(YBClientService.class);
  }

  @Override
  public String getName() {
    return super.getName() + "'(" + taskParams().universeUUID + " " +
        taskParams().blacklistNodes + ")'";
  }

  @Override
  public void run() {
    Universe universe = Universe.get(taskParams().universeUUID);
    String hostPorts = universe.getMasterAddresses();
    String certificate = universe.getCertificate();
    YBClient client = null;
    try {
      LOG.info("Running {}: hostPorts={}.", getName(), hostPorts);
      client = ybService.getClient(hostPorts, certificate);

      ModifyUniverseConfig modifyConfig = new ModifyUniverseConfig(client,
                                                                   taskParams().universeUUID,
                                                                   taskParams().blacklistNodes);
      modifyConfig.doCall();
      if (shouldIncrementVersion()) universe.incrementVersion();
    } catch (Exception e) {
      LOG.error("{} hit error : {}", getName(), e.getMessage());
      throw new RuntimeException(e);
    } finally {
      ybService.closeClient(client, hostPorts);
    }
  }

  // TODO: in future, AbstractModifyMasterClusterConfig.run() should have retries.
  public static class ModifyUniverseConfig extends AbstractModifyMasterClusterConfig {
    UUID universeUUID;
    Set<String> blacklistNodes;

    public ModifyUniverseConfig(YBClient client,
                                UUID universeUUID,
                                Set<String> blacklistNodes) {
      super(client);
      this.universeUUID = universeUUID;
      this.blacklistNodes = blacklistNodes;
    }

    public void generatePlacementInfoPB(Master.PlacementInfoPB.Builder placementInfoPB, Cluster cluster) {
      PlacementInfo placementInfo = cluster.placementInfo;
      for (PlacementCloud placementCloud : placementInfo.cloudList) {
        Provider cloud = Provider.find.byId(placementCloud.uuid);
        for (PlacementRegion placementRegion : placementCloud.regionList) {
          Region region = Region.get(placementRegion.uuid);
          for (PlacementAZ placementAz : placementRegion.azList) {
            AvailabilityZone az = AvailabilityZone.find.byId(placementAz.uuid);
            // Create the cloud info object.
            Common.CloudInfoPB.Builder ccb = Common.CloudInfoPB.newBuilder();
            ccb.setPlacementCloud(placementCloud.code)
               .setPlacementRegion(region.code)
               .setPlacementZone(az.code);

            Master.PlacementBlockPB.Builder pbb = Master.PlacementBlockPB.newBuilder();
            // Set the cloud info.
            pbb.setCloudInfo(ccb);
            // Set the minimum number of replicas in this PlacementAZ.
            pbb.setMinNumReplicas(placementAz.replicationFactor);
            placementInfoPB.addPlacementBlocks(pbb);
          }
        }
      }
      placementInfoPB.setNumReplicas(cluster.userIntent.replicationFactor);
      placementInfoPB.setPlacementUuid(ByteString.copyFromUtf8(cluster.uuid.toString()));
      placementInfoPB.build();
    }

    public void addAffinitizedPlacements(Master.ReplicationInfoPB.Builder replicationInfoPB,
                                         PlacementInfo placementInfo) {
      for (PlacementCloud placementCloud : placementInfo.cloudList) {
        Provider cloud = Provider.find.byId(placementCloud.uuid);
        for (PlacementRegion placementRegion : placementCloud.regionList) {
          Region region = Region.get(placementRegion.uuid);
          for (PlacementAZ placementAz : placementRegion.azList) {
            AvailabilityZone az = AvailabilityZone.find.byId(placementAz.uuid);
            // Create the cloud info object.
            Common.CloudInfoPB.Builder ccb = Common.CloudInfoPB.newBuilder();
            ccb.setPlacementCloud(placementCloud.code)
               .setPlacementRegion(region.code)
               .setPlacementZone(az.code);
            if (placementAz.isAffinitized) {
              replicationInfoPB.addAffinitizedLeaders(ccb);
            }
          }
        }
      }
    }

    @Override
    public Master.SysClusterConfigEntryPB modifyConfig(Master.SysClusterConfigEntryPB config) {
      Universe universe = Universe.get(universeUUID);

      Master.SysClusterConfigEntryPB.Builder configBuilder =
          Master.SysClusterConfigEntryPB.newBuilder(config);

      // Clear the replication info, as it is no longer valid.
      Master.ReplicationInfoPB.Builder replicationInfoPB =
          configBuilder.clearReplicationInfo().getReplicationInfoBuilder();
      // Build the live replicas from the replication info.
      Master.PlacementInfoPB.Builder placementInfoPB =
          replicationInfoPB.getLiveReplicasBuilder();
      // Create the placement info for the universe.
      PlacementInfo placementInfo = universe.getUniverseDetails().getPrimaryCluster().placementInfo;
      generatePlacementInfoPB(placementInfoPB, universe.getUniverseDetails().getPrimaryCluster());

      List<Cluster> readOnlyClusters = universe.getUniverseDetails().getReadOnlyClusters();
      for (Cluster cluster : readOnlyClusters) {
        Master.PlacementInfoPB.Builder placementInfoReadPB = replicationInfoPB.addReadReplicasBuilder();
        generatePlacementInfoPB(placementInfoReadPB, cluster);
      }

      addAffinitizedPlacements(replicationInfoPB, placementInfo);
      replicationInfoPB.build();

      // Add in any black listed nodes of tablet servers.
      if (blacklistNodes != null) {
        Master.BlacklistPB.Builder blacklistBuilder = configBuilder.getServerBlacklistBuilder();
        for (String nodeName : blacklistNodes) {
          NodeDetails node = universe.getNode(nodeName);
          if (node.isTserver && node.cloudInfo.private_ip != null) {
            blacklistBuilder.addHosts(ProtobufHelper.hostAndPortToPB(
                HostAndPort.fromParts(node.cloudInfo.private_ip, node.tserverRpcPort)));
          }
        }
        blacklistBuilder.build();
      }

      Master.SysClusterConfigEntryPB newConfig = configBuilder.build();
      LOG.info("Updating cluster config, old config = [{}], new config = [{}]",
               config.toString(), newConfig.toString());
      return newConfig;
    }
  }
}
