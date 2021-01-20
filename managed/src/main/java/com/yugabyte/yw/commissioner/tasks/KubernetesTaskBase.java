// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.SubTaskGroup;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesWaitForPod;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCheckNumPod;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase;
import com.yugabyte.yw.commissioner.tasks.subtasks.LoadBalancerStateChange;
import com.yugabyte.yw.commissioner.tasks.UpgradeUniverse.UpgradeTaskType;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.UpgradeParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;

public abstract class KubernetesTaskBase extends UniverseDefinitionTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(KubernetesTaskBase.class);

  public class KubernetesPlacement {
    Map<UUID, Integer> masters = new HashMap<>();
    Map<UUID, Integer> tservers = new HashMap<>();
    Map<UUID, Map<String, String>> configs = new HashMap<>();

    public KubernetesPlacement(PlacementInfo pi) {
      masters = PlacementInfoUtil.getNumMasterPerAZ(pi);
      tservers = PlacementInfoUtil.getNumTServerPerAZ(pi);

      // Mapping of the deployment zone and its corresponding Kubeconfig.
      configs = PlacementInfoUtil.getConfigPerAZ(pi);
    }
  }

  public void createPodsTask(KubernetesPlacement placement, String masterAddresses) {
    createPodsTask(placement, masterAddresses, null,
                   null, new PlacementInfo());
  }

  public void createPodsTask(KubernetesPlacement newPlacement, String masterAddresses,
                             KubernetesPlacement currPlacement, ServerType serverType,
                             PlacementInfo activeZones) {

    boolean edit = currPlacement != null;
    boolean isMultiAz = masterAddresses != null;

    Map<UUID, Map<String, String>> activeDeploymentConfigs =
        PlacementInfoUtil.getConfigPerAZ(activeZones);

    // Only used for new deployments, so maybe empty.
    SubTaskGroup createNamespaces = new SubTaskGroup(
        KubernetesCommandExecutor.CommandType.CREATE_NAMESPACE.getSubTaskGroupName(), executor);
    createNamespaces.setSubTaskGroupType(SubTaskGroupType.Provisioning);

    SubTaskGroup applySecrets = new SubTaskGroup(
        KubernetesCommandExecutor.CommandType.APPLY_SECRET.getSubTaskGroupName(), executor);
    applySecrets.setSubTaskGroupType(SubTaskGroupType.Provisioning);

    SubTaskGroup helmInstalls = new SubTaskGroup(
        KubernetesCommandExecutor.CommandType.HELM_INSTALL.getSubTaskGroupName(), executor);
    helmInstalls.setSubTaskGroupType(SubTaskGroupType.Provisioning);

    SubTaskGroup podsWait = new SubTaskGroup(
        KubernetesCheckNumPod.CommandType.WAIT_FOR_PODS.getSubTaskGroupName(), executor);
    podsWait.setSubTaskGroupType(SubTaskGroupType.Provisioning);

    for (Entry<UUID, Map<String, String>> entry : newPlacement.configs.entrySet()) {
      UUID azUUID = entry.getKey();
      String azCode = isMultiAz ? AvailabilityZone.get(azUUID).code : null;

      if (!newPlacement.masters.containsKey(azUUID) && serverType == ServerType.MASTER) {
        continue;
      }

      Map<String, String> config = entry.getValue();

      PlacementInfo tempPI = new PlacementInfo();
      PlacementInfoUtil.addPlacementZone(azUUID, tempPI);

      int currNumTservers = 0, currNumMasters = 0;
      int newNumMasters = newPlacement.masters.getOrDefault(azUUID, 0);
      int newNumTservers = newPlacement.tservers.getOrDefault(azUUID, 0);

      tempPI.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ =
          newNumTservers;
      tempPI.cloudList.get(0).regionList.get(0).azList.get(0).replicationFactor =
          newNumMasters;

      if (edit) {
        currNumMasters = currPlacement.masters.getOrDefault(azUUID, 0);
        currNumTservers = currPlacement.tservers.getOrDefault(azUUID, 0);
        // If the num pods are now less, we do not want to add anything,
        // so we skip.
        if (serverType == ServerType.MASTER) {
          if (newNumMasters <= currNumMasters) {
            continue;
          }
          // When adding new masters, we want to not increase the number of tservers
          // in the same operation.
          tempPI.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ =
              currNumTservers;
        } else {
          if (newNumTservers <= currNumTservers) {
            continue;
          }
        }
      }

      // This will always be false in the case of a new universe.
      if (activeDeploymentConfigs.containsKey(azUUID)) {
        // Helm Upgrade
        // Potential changes:
        // 1) Adding masters: Do not want either old masters or tservers to be rolled.
        // 2) Adding tservers:
        //    a) No masters changed, that means the master addresses are the same. Do not need
        //       to set partition on tserver or master.
        //    b) Masters changed, that means the master addresses changed, and we don't want to
        //       roll the older pods (or the new masters, since they will be in shell mode).
        int tserverPartition = currNumMasters != newNumMasters ? currNumTservers : 0;
        int masterPartition = currNumMasters != newNumMasters ?
            (serverType == ServerType.MASTER ? currNumMasters : newNumMasters): 0;
        helmInstalls.addTask(createKubernetesExecutorTaskForServerType(CommandType.HELM_UPGRADE,
                             tempPI, azCode, masterAddresses, null, serverType,
                             config, masterPartition,
                             tserverPartition));

        // When adding masters, the number of tservers will be still the same as before.
        // They get added later.
        int podsToWaitFor = serverType == ServerType.MASTER ?
            newNumMasters + currNumTservers : newNumMasters + newNumTservers;
        podsWait.addTask(createKubernetesCheckPodNumTask(
            KubernetesCheckNumPod.CommandType.WAIT_FOR_PODS, azCode, config,
            podsToWaitFor));
      } else {
        // Create the namespaces of the deployment.
        createNamespaces.addTask(createKubernetesExecutorTask(
            KubernetesCommandExecutor.CommandType.CREATE_NAMESPACE, azCode, config));

        // Apply the necessary pull secret to each namespace.
        applySecrets.addTask(createKubernetesExecutorTask(
            KubernetesCommandExecutor.CommandType.APPLY_SECRET, azCode, config));

        // Create the helm deployments.
        helmInstalls.addTask(createKubernetesExecutorTask(
            KubernetesCommandExecutor.CommandType.HELM_INSTALL, tempPI, azCode,
            masterAddresses, config));

        // Add zone to active configs.
        PlacementInfoUtil.addPlacementZone(azUUID, activeZones);
      }
    }

    subTaskGroupQueue.add(createNamespaces);
    subTaskGroupQueue.add(applySecrets);
    subTaskGroupQueue.add(helmInstalls);
    subTaskGroupQueue.add(podsWait);
  }

  public void upgradePodsTask(KubernetesPlacement newPlacement, String masterAddresses,
                              KubernetesPlacement currPlacement, ServerType serverType,
                              String softwareVersion, int waitTime, boolean masterChanged,
                              boolean tserverChanged) {

    boolean edit = currPlacement != null;
    boolean isMultiAz = masterAddresses != null;

    Map<UUID, Integer> serversToUpdate = serverType == ServerType.MASTER ?
        newPlacement.masters : newPlacement.tservers;
    String sType = serverType == ServerType.MASTER ? "yb-master" : "yb-tserver";

    for (Entry<UUID, Integer> entry : serversToUpdate.entrySet()) {
      UUID azUUID = entry.getKey();
      String azCode = isMultiAz ? AvailabilityZone.get(azUUID).code : null;

      PlacementInfo tempPI = new PlacementInfo();
      PlacementInfoUtil.addPlacementZone(azUUID, tempPI);

      int currNumMasters = 0, currNumTservers = 0;
      int newNumMasters = newPlacement.masters.getOrDefault(azUUID, 0);
      int newNumTservers = newPlacement.tservers.getOrDefault(azUUID, 0);

      int numPods = serverType == ServerType.MASTER ? newNumMasters : newNumTservers;

      if (edit) {
        currNumMasters = currPlacement.masters.getOrDefault(azUUID, 0);
        currNumTservers = currPlacement.tservers.getOrDefault(azUUID, 0);
        if (serverType == ServerType.TSERVER) {
          // Since we only want to roll the old pods and not the new ones.
          numPods = currNumTservers;
          if (currNumTservers == 0) {
            continue;
          }
        }
      }

      tempPI.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ = newNumTservers;
      tempPI.cloudList.get(0).regionList.get(0).azList.get(0).replicationFactor = newNumMasters;

      Map<String, String> config = newPlacement.configs.get(azUUID);

      // Upgrade the master pods individually for each deployment.
      for (int partition = numPods - 1; partition >= 0; partition--) {
        // Possible scenarios:
        // 1) Upgrading masters.
        //    a) The tservers have no changes. In that case, the tserver partition should
        //       be 0.
        //    b) The tservers have changes. In the case of an edit, the value should be
        //       the old number of tservers (since the new will already have the updated values).
        //       Otherwise, the value should be the number of existing pods (since we don't
        //       want any pods to be rolled.)
        // 2) Upgrading the tserver. In this case, the masters will always have already rolled.
        //    So it is safe to assume for all current supported operations via upgrade,
        //    this will be a no-op. But for future proofing, we try to model it the same way
        //    as the tservers.
        int masterPartition = masterChanged ? (edit ? currNumMasters : newNumMasters) : 0;
        int tserverPartition = tserverChanged ? (edit ? currNumTservers : newNumTservers) : 0;
        masterPartition = serverType == ServerType.MASTER ? partition : masterPartition;
        tserverPartition = serverType == ServerType.TSERVER ? partition : tserverPartition;
        createSingleKubernetesExecutorTaskForServerType(CommandType.HELM_UPGRADE,
            tempPI, azCode, masterAddresses, softwareVersion, serverType, config,
            masterPartition, tserverPartition);
        String podName = String.format("%s-%d", sType, partition);
        createKubernetesWaitForPodTask(KubernetesWaitForPod.CommandType.WAIT_FOR_POD,
            podName, azCode, config);

        NodeDetails node = getPodName(partition, azCode, serverType, isMultiAz);
        List<NodeDetails> nodeList = new ArrayList<>();
        nodeList.add(node);
        createWaitForServersTasks(nodeList, serverType)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
        createWaitForServerReady(node, serverType, waitTime)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
      }
    }
  }

  public void deletePodsTask(KubernetesPlacement currPlacement) {
    deletePodsTask(currPlacement, null, null, false);
  }

  public void deletePodsTask(KubernetesPlacement currPlacement, String masterAddresses,
                             KubernetesPlacement newPlacement, boolean userIntentChange) {

    boolean edit = newPlacement != null;
    boolean isMultiAz = masterAddresses != null;

    // If no config in new placement, delete deployment.
    SubTaskGroup helmDeletes = new SubTaskGroup(
        KubernetesCommandExecutor.CommandType.HELM_DELETE.getSubTaskGroupName(), executor);
    helmDeletes.setSubTaskGroupType(SubTaskGroupType.RemovingUnusedServers);

    SubTaskGroup volumeDeletes = new SubTaskGroup(
        KubernetesCommandExecutor.CommandType.VOLUME_DELETE.getSubTaskGroupName(), executor);
    volumeDeletes.setSubTaskGroupType(SubTaskGroupType.RemovingUnusedServers);

    SubTaskGroup namespaceDeletes = new SubTaskGroup(
        KubernetesCommandExecutor.CommandType.NAMESPACE_DELETE.getSubTaskGroupName(), executor);
    namespaceDeletes.setSubTaskGroupType(SubTaskGroupType.RemovingUnusedServers);

    SubTaskGroup podsWait = new SubTaskGroup(
        KubernetesCheckNumPod.CommandType.WAIT_FOR_PODS.getSubTaskGroupName(), executor);
    podsWait.setSubTaskGroupType(SubTaskGroupType.RemovingUnusedServers);

    for (Entry<UUID, Map<String, String>> entry : currPlacement.configs.entrySet()) {
      UUID azUUID = entry.getKey();
      String azCode = isMultiAz ? AvailabilityZone.get(azUUID).code : null;
      Map<String, String> config = entry.getValue();

      // If the new placement also has the AZ, we need to scale down. But if there
      // was a change in the instance type, the updateRemainingPod itself would have taken care
      // of the deployments' scale down.

      boolean keepDeployment = false;
      if (edit) {
        keepDeployment = newPlacement.configs.containsKey(azUUID) && !userIntentChange;
      }
      if (keepDeployment) {
        PlacementInfo tempPI = new PlacementInfo();
        PlacementInfoUtil.addPlacementZone(azUUID, tempPI);
        tempPI.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ =
            newPlacement.tservers.get(azUUID);
        tempPI.cloudList.get(0).regionList.get(0).azList.get(0).replicationFactor =
            newPlacement.masters.getOrDefault(azUUID, 0);
        helmDeletes.addTask(createKubernetesExecutorTask(CommandType.HELM_UPGRADE,
                            tempPI, azCode, masterAddresses, config));
        podsWait.addTask(createKubernetesCheckPodNumTask(
                         KubernetesCheckNumPod.CommandType.WAIT_FOR_PODS, azCode,
                         config, newPlacement.tservers.get(azUUID) +
                         newPlacement.masters.getOrDefault(azUUID, 0)));
      } else {
        // Delete the helm deployments.
        helmDeletes.addTask(createKubernetesExecutorTask(
            KubernetesCommandExecutor.CommandType.HELM_DELETE, azCode, config));

        // Delete the PVs created for the deployments.
        volumeDeletes.addTask(createKubernetesExecutorTask(
            KubernetesCommandExecutor.CommandType.VOLUME_DELETE, azCode, config));

        // Delete the namespaces of the deployments.
        namespaceDeletes.addTask(createKubernetesExecutorTask(
            KubernetesCommandExecutor.CommandType.NAMESPACE_DELETE, azCode, config));
      }
    }
    subTaskGroupQueue.add(helmDeletes);
    subTaskGroupQueue.add(volumeDeletes);
    subTaskGroupQueue.add(namespaceDeletes);
    subTaskGroupQueue.add(podsWait);
  }

  /*
  Returns the NodeDetails of the pod that we need to wait for.
  */
  public NodeDetails getPodName(int partition, String azCode, ServerType serverType,
                                boolean isMultiAz) {
    String sType = serverType == ServerType.MASTER ? "yb-master" : "yb-tserver";
    Set<NodeDetails> tservers = new HashSet<>();
    String podName = isMultiAz ? String.format("%s-%d_%s", sType, partition, azCode) :
        String.format("%s-%d", sType, partition);
    NodeDetails node = new NodeDetails();
    node.nodeName = podName;
    return node;
  }

  /*
  Sends a collection of all the pods that need to be added.
  */
  public Set<NodeDetails> getPodsToAdd(Map<UUID, Integer> newPlacement,
                                       Map<UUID, Integer> currPlacement,
                                       ServerType serverType, boolean isMultiAz) {

    Set<NodeDetails> podsToAdd = new HashSet<NodeDetails>();
    for (Entry<UUID, Integer> entry : newPlacement.entrySet()) {
      UUID azUUID = entry.getKey();
      String azCode = AvailabilityZone.get(azUUID).code;
      int numNewReplicas = entry.getValue();
      int numCurrReplicas = 0;
      if (currPlacement != null) {
        numCurrReplicas = currPlacement.getOrDefault(azUUID, 0);
      }
      for (int i = numCurrReplicas; i < numNewReplicas; i++) {
        NodeDetails node = getPodName(i, azCode, serverType, isMultiAz);
        podsToAdd.add(node);
      }
    }
    return podsToAdd;
  }

  /*
  Sends a collection of all the pods that need to be removed.
  */
  public Set<NodeDetails> getPodsToRemove(Map<UUID, Integer> newPlacement,
                                          Map<UUID, Integer> currPlacement,
                                          ServerType serverType, Universe universe,
                                          boolean isMultiAz) {
    Set<NodeDetails> podsToRemove = new HashSet<NodeDetails>();
    for (Entry<UUID, Integer> entry : currPlacement.entrySet()) {
      UUID azUUID = entry.getKey();
      String azCode = AvailabilityZone.get(azUUID).code;
      int numCurrReplicas = entry.getValue();
      int numNewReplicas = newPlacement.getOrDefault(azUUID, 0);
      for (int i = numCurrReplicas - 1; i >= numNewReplicas; i--) {
        NodeDetails node = getPodName(i, azCode, serverType, isMultiAz);
        podsToRemove.add(universe.getNode(node.nodeName));
      }
    }
    return podsToRemove;
  }

  // Create Kubernetes Executor task for creating the namespaces and pull secrets.
  public KubernetesCommandExecutor createKubernetesExecutorTask(
      KubernetesCommandExecutor.CommandType commandType, String az, Map<String, String> config) {
    return createKubernetesExecutorTask(commandType, null, az, null, config);
  }

  // Create the Kubernetes Executor task for the helm deployments. (USED)
  public KubernetesCommandExecutor createKubernetesExecutorTask(
      KubernetesCommandExecutor.CommandType commandType, PlacementInfo pi, String az,
      String masterAddresses, Map<String, String> config) {
    return createKubernetesExecutorTaskForServerType(commandType, pi, az, masterAddresses, null,
        ServerType.EITHER, config, 0 /* master partition */, 0 /* tserver partition */);
  }

  // Create and return the Kubernetes Executor task for deployment of a k8s universe.
  public KubernetesCommandExecutor createKubernetesExecutorTaskForServerType(
      KubernetesCommandExecutor.CommandType commandType,
      PlacementInfo pi, String az, String masterAddresses,
      String ybSoftwareVersion, ServerType serverType,
      Map<String, String> config, int masterPartition,
      int tserverPartition) {
    KubernetesCommandExecutor.Params params = new KubernetesCommandExecutor.Params();
    UniverseDefinitionTaskParams.Cluster primary = taskParams().getPrimaryCluster();
    params.providerUUID = UUID.fromString(
        primary.userIntent.provider);
    params.commandType = commandType;
    params.nodePrefix = taskParams().nodePrefix;
    params.universeUUID = taskParams().universeUUID;

    if (az != null) {
      params.nodePrefix = String.format("%s-%s", params.nodePrefix, az);
    }
    if (masterAddresses != null) {
      params.masterAddresses = masterAddresses;
    }
    if (ybSoftwareVersion != null) {
      params.ybSoftwareVersion = ybSoftwareVersion;
    }
    if (pi != null) {
      params.placementInfo = pi;
    }
    if (config != null) {
      params.config = config;
    }
    params.masterPartition = masterPartition;
    params.tserverPartition = tserverPartition;
    params.enableNodeToNodeEncrypt = primary.userIntent.enableNodeToNodeEncrypt;
    params.enableClientToNodeEncrypt = primary.userIntent.enableClientToNodeEncrypt;
    params.rootCA = taskParams().rootCA;
    params.serverType = serverType;
    KubernetesCommandExecutor task = new KubernetesCommandExecutor();
    task.initialize(params);
    return task;
  }

  public void createSingleKubernetesExecutorTask(
      KubernetesCommandExecutor.CommandType commandType) {
    createSingleKubernetesExecutorTask(commandType, null);
  }

  // Create a single Kubernetes Executor task in case we cannot execute tasks in parallel.
  public void createSingleKubernetesExecutorTask(KubernetesCommandExecutor.CommandType commandType,
                                                 PlacementInfo pi) {
    createSingleKubernetesExecutorTaskForServerType(commandType, pi, null, null, null,
                                                    ServerType.EITHER, null,
                                                    0 /* master partition */,
                                                    0 /* tserver partition */);
  }

  // Create a single Kubernetes Executor task in case we cannot execute tasks in parallel.
  public void createSingleKubernetesExecutorTaskForServerType(
      KubernetesCommandExecutor.CommandType commandType,
      PlacementInfo pi, String az, String masterAddresses,
      String ybSoftwareVersion, ServerType serverType,
      Map<String, String> config, int masterPartition, int tserverPartition) {
    SubTaskGroup subTaskGroup = new SubTaskGroup(commandType.getSubTaskGroupName(), executor);
    KubernetesCommandExecutor.Params params = new KubernetesCommandExecutor.Params();
    UniverseDefinitionTaskParams.Cluster primary = taskParams().getPrimaryCluster();
    params.providerUUID = UUID.fromString(
        primary.userIntent.provider);
    params.commandType = commandType;
    params.nodePrefix = taskParams().nodePrefix;
    params.universeUUID = taskParams().universeUUID;

    if (az != null) {
      params.nodePrefix = String.format("%s-%s", params.nodePrefix, az);
    }
    if (masterAddresses != null) {
      params.masterAddresses = masterAddresses;
    }
    if (ybSoftwareVersion != null) {
      params.ybSoftwareVersion = ybSoftwareVersion;
    }
    if (pi != null) {
      params.placementInfo = pi;
    }
    if (config != null) {
      params.config = config;
    }
    params.masterPartition = masterPartition;
    params.tserverPartition = tserverPartition;
    params.enableNodeToNodeEncrypt = primary.userIntent.enableNodeToNodeEncrypt;
    params.enableClientToNodeEncrypt = primary.userIntent.enableClientToNodeEncrypt;
    params.rootCA = taskParams().rootCA;
    params.serverType = serverType;
    KubernetesCommandExecutor task = new KubernetesCommandExecutor();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    subTaskGroup.setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.Provisioning);
  }

  public void createKubernetesWaitForPodTask(KubernetesWaitForPod.CommandType commandType,
                                             String podName, String az,
                                             Map<String, String> config) {
    SubTaskGroup subTaskGroup = new SubTaskGroup(commandType.getSubTaskGroupName(), executor);
    KubernetesWaitForPod.Params params = new KubernetesWaitForPod.Params();
    UniverseDefinitionTaskParams.Cluster primary = taskParams().getPrimaryCluster();
    params.providerUUID = UUID.fromString(primary.userIntent.provider);
    params.commandType = commandType;
    params.nodePrefix = taskParams().nodePrefix;
    if (az != null) {
      params.nodePrefix = String.format("%s-%s", params.nodePrefix, az);
    }
    if (config != null) {
      params.config = config;
    }
    params.universeUUID = taskParams().universeUUID;
    params.podName = podName;
    KubernetesWaitForPod task = new KubernetesWaitForPod();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    subTaskGroup.setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.KubernetesWaitForPod);
  }

    public KubernetesCheckNumPod createKubernetesCheckPodNumTask(
        KubernetesCheckNumPod.CommandType commandType,
        String az, Map<String, String> config, int numPods) {
    KubernetesCheckNumPod.Params params = new KubernetesCheckNumPod.Params();
    UniverseDefinitionTaskParams.Cluster primary = taskParams().getPrimaryCluster();
    params.providerUUID = UUID.fromString(primary.userIntent.provider);
    params.commandType = commandType;
    params.nodePrefix = taskParams().nodePrefix;
    if (az != null) {
      params.nodePrefix = String.format("%s-%s", params.nodePrefix, az);
    }
    if (config != null) {
      params.config = config;
    }
    params.universeUUID = taskParams().universeUUID;
    params.podNum = numPods;
    KubernetesCheckNumPod task = new KubernetesCheckNumPod();
    task.initialize(params);
    return task;
  }
}
