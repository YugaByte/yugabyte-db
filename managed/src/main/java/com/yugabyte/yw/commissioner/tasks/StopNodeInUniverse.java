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

import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;

import java.util.Arrays;
import java.util.HashSet;

public class StopNodeInUniverse extends UniverseTaskBase {

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
      LOG.info("Stop Node with name {} from universe {}", taskParams().nodeName,
               taskParams().universeUUID);

      currentNode = universe.getNode(taskParams().nodeName);
      if (currentNode == null) {
        String msg = "No node " + taskParams().nodeName + " found in universe " + universe.name;
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      // Update Node State to Stopping
      createSetNodeStateTask(currentNode, NodeState.Stopping)
          .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);

      taskParams().azUuid = currentNode.azUuid;
      taskParams().placementUuid = currentNode.placementUuid;
      if (instanceExists(taskParams())) {
        // Stop the tserver.
        createTServerTaskForNode(currentNode, "stop")
            .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);

        // Stop the master process on this node.
        if (currentNode.isMaster) {
          createStopMasterTasks(new HashSet<NodeDetails>(Arrays.asList(currentNode)))
              .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
          createWaitForMasterLeaderTask()
              .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
        }
      }

      // Update the per process state in YW DB.
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.TSERVER, false)
          .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
      if (currentNode.isMaster) {
        createChangeConfigTask(currentNode, false /* isAdd */, SubTaskGroupType.ConfigureUniverse,
                               true /* useHostPort */);
        createUpdateNodeProcessTask(taskParams().nodeName, ServerType.MASTER, false)
            .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
      }

      // Update Node State to Stopped
      createSetNodeStateTask(currentNode, NodeState.Stopped)
          .setSubTaskGroupType(SubTaskGroupType.StoppingNode);

      // Mark universe task state to success
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.StoppingNode);

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
