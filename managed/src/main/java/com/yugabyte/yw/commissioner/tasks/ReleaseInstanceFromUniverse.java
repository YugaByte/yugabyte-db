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

import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.commissioner.tasks.subtasks.ChangeMasterConfig;
import com.yugabyte.yw.commissioner.tasks.subtasks.WaitForLoadBalance;
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

import java.util.Arrays;
import java.util.HashSet;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

// Allows the removal of the instance from a universe. That node is already not part of the
// universe and is in Removed state.
public class ReleaseInstanceFromUniverse extends UniverseTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(ReleaseInstanceFromUniverse.class);

  @Override
  protected NodeTaskParams taskParams() {
    return (NodeTaskParams)taskParams;
  }

  @Override
  public void run() {
    LOG.info("Started {} task for node {} in univ uuid={}", getName(),
             taskParams().nodeName, taskParams().universeUUID);
    NodeDetails currentNode = null;
    boolean hitException = false;
    try {
      // Create the task list sequence.
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      // Set the 'updateInProgress' flag to prevent other updates from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      currentNode = universe.getNode(taskParams().nodeName);
      if (currentNode == null) {
        String msg = "No node " + taskParams().nodeName + " found in universe " + universe.name;
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      if (currentNode.state != NodeState.Removed &&
          currentNode.state != NodeState.Stopped) {
        String msg = "Node " + taskParams().nodeName + " is not on removed or added state, but " +
                     "is in " + currentNode.state + ", so cannot be released.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      UserIntent userIntent = universe.getUniverseDetails()
                                      .getClusterByUuid(currentNode.placementUuid)
                                      .userIntent;

      // Update Node State to BeingDecommissioned.
      createSetNodeStateTask(currentNode, NodeState.BeingDecommissioned)
          .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);

      taskParams().azUuid = currentNode.azUuid;
      taskParams().placementUuid = currentNode.placementUuid;

      // Wait for Master Leader before doing Master operations, like blacklisting.
      createWaitForMasterLeaderTask()
          .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);
      // Create a task for removal of this server from blacklist on master leader.
      createModifyBlackListTask(Arrays.asList(currentNode), false /* isAdd */)
          .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);

      if (instanceExists(taskParams())) {
        if (userIntent.providerType.equals(CloudType.onprem)) {
          // Stop master and tservers.
          createStopServerTasks(universe.getNodes(), "master", false)
              .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
          createStopServerTasks(universe.getNodes(), "tserver", false)
              .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
        }

        // Create tasks to terminate that instance. Force delete and ignore errors.
        createDestroyServerTasks(new HashSet<NodeDetails>(Arrays.asList(currentNode)), true, false)
            .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);
      }

      // Update Node State to Decommissioned.
      createSetNodeStateTask(currentNode, NodeState.Decommissioned)
          .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);

      // Delete and reset node metadata for onprem universes.
      if (userIntent.providerType.equals(CloudType.onprem)) {
        deleteNodeFromUniverseTask(taskParams().nodeName)
          .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);
      }

      // Update the DNS entry for this universe.
      createDnsManipulationTask(DnsManager.DnsCommandType.Edit, false, userIntent.providerType,
                                userIntent.provider, userIntent.universeName)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Update the swamper target file.
      createSwamperTargetUpdateTask(false /* removeFile */);

      // Mark universe task state to success
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.ReleasingInstance);

      // Run all the tasks.
      subTaskGroupQueue.run();
    } catch (Throwable t) {
      LOG.error("Error executing task {} with error='{}'.", getName(), t.getMessage(), t);
      hitException = true;
      throw t;
    } finally {
      // Reset the state, on any failure, so that the actions can be retried.
      if (currentNode != null && hitException) {
        setNodeState(taskParams().nodeName, currentNode.state);
      }

      // Mark the update of the universe as done. This will allow future edits/updates to the
      // universe to happen.
      unlockUniverseForUpdate();
    }
    LOG.info("Finished {} task.", getName());
  }
}
