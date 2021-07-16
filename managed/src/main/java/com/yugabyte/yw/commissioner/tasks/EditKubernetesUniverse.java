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

import com.yugabyte.yw.commissioner.BaseTaskDependencies;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import lombok.extern.slf4j.Slf4j;

import javax.inject.Inject;
import java.util.*;

@Slf4j
public class EditKubernetesUniverse extends KubernetesTaskBase {

  static final int DEFAULT_WAIT_TIME_MS = 10000;

  PlacementInfo activeZones = new PlacementInfo();
  boolean isMultiAz = false;

  KubernetesPlacement newPlacement, currPlacement;

  @Inject
  protected EditKubernetesUniverse(BaseTaskDependencies baseTaskDependencies) {
    super(baseTaskDependencies);
  }

  @Override
  public void run() {
    try {
      checkUniverseVersion();
      // Verify the task params.
      verifyParams(UniverseOpType.EDIT);

      // Create the task list sequence.
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      Provider provider =
          Provider.get(UUID.fromString(taskParams().getPrimaryCluster().userIntent.provider));

      /* Steps for multi-cluster edit
      1) Compute masters with the new placement info.
      2) If the masters are different to the old one, continue with step 3, else go to step 6.
      3) Check if the instance type has changed from xsmall/dev to something else. If so,
         roll all the current masters.
      4) Bring up the new master pods while ensuring nothing else changes in the old deployments.
      5) Create change config task to update the new master addresses (adding one and removing one at a time).
      6) Make changes to the tservers. Either adding new ones and/or updating the current ones.
      7) Create the blacklist to remove unnecessary tservers from the universe.
      8) Wait for the data to move.
      9) Remove the old masters and tservers.
      */

      // Get requested user intent.
      Cluster primaryCluster = taskParams().getPrimaryCluster();
      UserIntent userIntent = primaryCluster.userIntent;
      PlacementInfo newPI = primaryCluster.placementInfo;

      isMultiAz = PlacementInfoUtil.isMultiAZ(provider);

      selectNumMastersAZ(newPI);

      newPlacement = new KubernetesPlacement(newPI);

      // Get current universe's intent.
      UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
      UserIntent currIntent = universeDetails.getPrimaryCluster().userIntent.clone();
      PlacementInfo currPI = universeDetails.getPrimaryCluster().placementInfo;

      currPlacement = new KubernetesPlacement(currPI);

      // TODO(bhavin192): REVIEW: should this be a taskParam? I
      // personally don't think so.
      boolean newNamingStyle = universe.usesHelmNewNamingStyle();

      String nodePrefix = taskParams().nodePrefix;

      Set<NodeDetails> mastersToAdd =
          getPodsToAdd(
              newPlacement.masters,
              currPlacement.masters,
              ServerType.MASTER,
              nodePrefix,
              isMultiAz,
              newNamingStyle);
      Set<NodeDetails> mastersToRemove =
          getPodsToRemove(
              newPlacement.masters,
              currPlacement.masters,
              ServerType.MASTER,
              universe,
              nodePrefix,
              isMultiAz,
              newNamingStyle);

      Set<NodeDetails> tserversToAdd =
          getPodsToAdd(
              newPlacement.tservers,
              currPlacement.tservers,
              ServerType.TSERVER,
              nodePrefix,
              isMultiAz,
              newNamingStyle);
      Set<NodeDetails> tserversToRemove =
          getPodsToRemove(
              newPlacement.tservers,
              currPlacement.tservers,
              ServerType.TSERVER,
              universe,
              nodePrefix,
              isMultiAz,
              newNamingStyle);

      for (UUID currAZs : currPlacement.configs.keySet()) {
        PlacementInfoUtil.addPlacementZone(currAZs, activeZones);
      }

      boolean userIntentChange = false;
      boolean isNumNodeChange = false;
      boolean isFirstIteration = true;
      boolean masterChange = false;

      // Check if number of nodes changed.
      if (currIntent.numNodes != userIntent.numNodes) {
        isNumNodeChange = true;
        currIntent.numNodes = userIntent.numNodes;
      }

      List<String> masterResourceChangeInstances = Arrays.asList("dev", "xsmall");
      // Check if the instance type has changed. In that case, we still
      // need to perform rolling upgrades.
      if (!currIntent.instanceType.equals(userIntent.instanceType)) {
        // If the instance type changed from dev/xsmall to anything else,
        // master resources will also change.
        if (masterResourceChangeInstances.contains(currIntent.instanceType)) {
          masterChange = true;
        }
        userIntentChange = true;
      }

      // Update the user intent.
      writeUserIntentToUniverse();

      // Bring up new masters and update the configs.
      if (!mastersToAdd.isEmpty()) {
        masterChange = true;
        startNewPods(
            mastersToAdd,
            ServerType.MASTER,
            newPI,
            provider,
            universeDetails.communicationPorts.masterRpcPort,
            newNamingStyle);

        // Update master addresses to the latest required ones.
        createMoveMasterTasks(new ArrayList(mastersToAdd), new ArrayList(mastersToRemove));
      }

      // Bring up new tservers.
      if (!tserversToAdd.isEmpty()) {
        startNewPods(
            tserversToAdd,
            ServerType.TSERVER,
            newPI,
            provider,
            universeDetails.communicationPorts.masterRpcPort,
            newNamingStyle);
      }

      // Update the blacklist servers on master leader.
      createPlacementInfoTask(tserversToRemove)
          .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);

      // If the tservers have been removed, move the data.
      if (!tserversToRemove.isEmpty()) {
        createWaitForDataMoveTask().setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
      }
      // If tservers have been added, we wait for the load to balance.
      if (!tserversToAdd.isEmpty()) {
        createWaitForLoadBalanceTask().setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
      }

      // Now roll all the old pods that haven't been removed and aren't newly added.
      // This will update the master addresses as well as the instance type changes.
      if (userIntentChange || !mastersToAdd.isEmpty()) {
        if (masterChange) {
          updateRemainingPods(
              ServerType.MASTER,
              newPI,
              provider,
              universeDetails.communicationPorts.masterRpcPort,
              true,
              true,
              newNamingStyle);
        }
        updateRemainingPods(
            ServerType.TSERVER,
            newPI,
            provider,
            universeDetails.communicationPorts.masterRpcPort,
            false,
            true,
            newNamingStyle);
      }

      // If tservers have been removed, check if some deployments need to be completely
      // removed. Also modify the blacklist to untrack deleted pods.
      if (!tserversToRemove.isEmpty()) {
        removeDeployments(
            newPI,
            provider,
            userIntentChange,
            universeDetails.communicationPorts.masterRpcPort,
            newNamingStyle);
        createModifyBlackListTask(new ArrayList(tserversToRemove), false /* isAdd */)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
      }

      // Update the universe to the new state.
      createSingleKubernetesExecutorTask(KubernetesCommandExecutor.CommandType.POD_INFO, newPI);

      // Update the swamper target file.
      createSwamperTargetUpdateTask(false /* removeFile */);

      // Marks the update of this universe as a success only if all the tasks before it succeeded.
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);
      // Run all the tasks.
      subTaskGroupQueue.run();
    } catch (Throwable t) {
      log.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
      throw t;
    } finally {
      unlockUniverseForUpdate();
    }
    log.info("Finished {} task.", getName());
  }

  /*
  Sends the RPC to update the master addresses in the config.
  */
  public void createMoveMasterTasks(
      List<NodeDetails> mastersToAdd, List<NodeDetails> mastersToRemove) {

    UserTaskDetails.SubTaskGroupType subTask = SubTaskGroupType.WaitForDataMigration;

    // Perform adds.
    for (int idx = 0; idx < mastersToAdd.size(); idx++) {
      createChangeConfigTask(mastersToAdd.get(idx), true, subTask);
    }
    // Perform removes.
    for (int idx = 0; idx < mastersToRemove.size(); idx++) {
      createChangeConfigTask(mastersToRemove.get(idx), false, subTask);
    }
    // Wait for master leader.
    createWaitForMasterLeaderTask().setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
  }

  /*
  Starts up the new pods as requested by the user.
  */
  public void startNewPods(
      Set<NodeDetails> podsToAdd,
      ServerType serverType,
      PlacementInfo newPI,
      Provider provider,
      int masterRpcPort,
      boolean newNamingStyle) {
    // If starting new masters, we want them to come up in shell-mode.
    String masterAddresses =
        serverType == ServerType.MASTER
            ? ""
            : PlacementInfoUtil.computeMasterAddresses(
                newPI,
                newPlacement.masters,
                taskParams().nodePrefix,
                provider,
                masterRpcPort,
                newNamingStyle);

    createPodsTask(newPlacement, masterAddresses, currPlacement, serverType, activeZones);

    createSingleKubernetesExecutorTask(KubernetesCommandExecutor.CommandType.POD_INFO, activeZones);

    createWaitForServersTasks(podsToAdd, serverType)
        .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
  }

  /*
  Performs the updates to the helm charts to modify the master addresses as well as
  update the instance type.
  */
  public void updateRemainingPods(
      ServerType serverType,
      PlacementInfo newPI,
      Provider provider,
      int masterRpcPort,
      boolean masterChanged,
      boolean tserverChanged,
      boolean newNamingStyle) {
    String masterAddresses =
        PlacementInfoUtil.computeMasterAddresses(
            newPI,
            newPlacement.masters,
            taskParams().nodePrefix,
            provider,
            masterRpcPort,
            newNamingStyle);

    upgradePodsTask(
        newPlacement,
        masterAddresses,
        currPlacement,
        serverType,
        null,
        DEFAULT_WAIT_TIME_MS,
        masterChanged,
        tserverChanged,
        newNamingStyle);
  }

  /*
  Deletes/Scales down the helm deployments.
  */
  public void removeDeployments(
      PlacementInfo newPI,
      Provider provider,
      boolean userIntentChange,
      int masterRpcPort,
      boolean newNamingStyle) {
    String masterAddresses =
        PlacementInfoUtil.computeMasterAddresses(
            newPI,
            newPlacement.masters,
            taskParams().nodePrefix,
            provider,
            masterRpcPort,
            newNamingStyle);

    // Need to unify with DestroyKubernetesUniverse.
    deletePodsTask(currPlacement, masterAddresses, newPlacement, userIntentChange);
  }
}
