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

import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

import java.util.Arrays;
import java.util.HashSet;

import static com.yugabyte.yw.common.Util.areMastersUnderReplicated;

/**
 * Class contains the tasks to start a node in a given universe.
 * It starts the tserver process and the master process if needed.
 */
public class StartNodeInUniverse extends UniverseDefinitionTaskBase {

  @Override
  protected NodeTaskParams taskParams() {
    return (NodeTaskParams)taskParams;
  }

  @Override
  public void run() {
    NodeDetails currentNode = null;
    boolean hitException = false;
    try {
      checkUniverseVersion();
      // Create the task list sequence.
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
      // Set the 'updateInProgress' flag to prevent other updates from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);
      LOG.info("Start Node with name {} from universe {}", taskParams().nodeName, taskParams().universeUUID);

      currentNode = universe.getNode(taskParams().nodeName);
      if (currentNode == null) {
        String msg = "No node " + taskParams().nodeName + " found in universe " + universe.name;
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      taskParams().azUuid = currentNode.azUuid;
      taskParams().placementUuid = currentNode.placementUuid;
      if (!instanceExists(taskParams())) {
        String msg = "No instance exists for " + taskParams().nodeName;
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      // Update node state to Starting
      createSetNodeStateTask(currentNode, NodeState.Starting)
          .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

      // Start the tserver process
      createTServerTaskForNode(currentNode, "start")
          .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

      // Mark the node process flags as true.
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.TSERVER, true)
          .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

      // Bring up any masters, as needed.
      boolean masterAdded = false;
      if (areMastersUnderReplicated(currentNode, universe)) {
        // Clean the master addresses in the conf file for the current node so that
        // the master comes up as a shell master.
        createConfigureServerTasks(ImmutableList.of(currentNode), true /* isShell */,
            true /* updateMasterAddrs */, true /* isMaster */)
                .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

        // Set gflags for master.
        createGFlagsOverrideTasks(ImmutableList.of(currentNode), ServerType.MASTER);

        // Start a master process.
        createStartMasterTasks(new HashSet<NodeDetails>(Arrays.asList(currentNode)))
            .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

        // Mark node as isMaster in YW DB.
        createUpdateNodeProcessTask(taskParams().nodeName, ServerType.MASTER, true)
            .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

        // Wait for the master to be responsive.
        createWaitForServersTasks(new HashSet<NodeDetails>(Arrays.asList(currentNode)), ServerType.MASTER)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

        // Add stopped master to the quorum.
        createChangeConfigTask(currentNode, true /* isAdd */, SubTaskGroupType.ConfigureUniverse);

        masterAdded = true;
      }

      // Update all server conf files with new master information.
      if (masterAdded) {
        createMasterInfoUpdateTask(universe, currentNode);
      }

      // Update node state to running
      createSetNodeStateTask(currentNode, NodeDetails.NodeState.Live)
          .setSubTaskGroupType(SubTaskGroupType.StartingNode);

      // Update the swamper target file.
      // It is required because the node could be removed from the swamper file
      // between the Stop/Start actions as Inactive.
      createSwamperTargetUpdateTask(false /* removeFile */);

      // Mark universe update success to true
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.StartingNode);

      subTaskGroupQueue.run();
    } catch (Throwable t) {
      LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
      hitException = true;
      throw t;
    } finally {
      // Reset the state, on any failure, so that the actions can be retried.
      if (currentNode != null && hitException) {
        setNodeState(taskParams().nodeName, currentNode.state);
      }
      unlockUniverseForUpdate();
    }
    LOG.info("Finished {} task.", getName());
  }
}
