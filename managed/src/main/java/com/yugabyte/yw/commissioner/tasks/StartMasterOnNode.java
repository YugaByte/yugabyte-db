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

import static com.yugabyte.yw.common.Util.areMastersUnderReplicated;

import java.util.Arrays;
import java.util.HashSet;

import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

// Allows the addition of the master server to a node. Spawns master the process and ensures
// the task waits for the right set of load balance primitives.
public class StartMasterOnNode extends UniverseDefinitionTaskBase {

  @Override
  protected NodeTaskParams taskParams() {
    return (NodeTaskParams) taskParams;
  }

  @Override
  public void run() {
    LOG.info("Started {} task for node {} in univ uuid={}", getName(), taskParams().nodeName,
        taskParams().universeUUID);
    NodeDetails currentNode = null;
    boolean hitException = false;
    try {
      checkUniverseVersion();
      // Create the task list sequence.
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      // Update the DB to prevent other changes from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      currentNode = universe.getNode(taskParams().nodeName);
      if (currentNode == null) {
        String msg = "No node " + taskParams().nodeName + " in universe " + universe.name;
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

      if (currentNode.isMaster) {
        String msg = "Node " + taskParams().nodeName + " already has the Master process running.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      if (currentNode.state == NodeState.Stopped || currentNode.state == NodeState.Removed
          || currentNode.state == NodeState.Decommissioned) {
        String msg = "Node " + taskParams().nodeName + " is in removed or decommissioned state"
            + ", the Master process cannot be started. Use \"Start Node\" instead.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      if (!areMastersUnderReplicated(currentNode, universe)) {
        String msg = "Unable to start the Master process on node " + taskParams().nodeName
            + ", no more Masters allowed.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      LOG.info("Bringing up master for under replicated universe {} ({})", universe.universeUUID,
          universe.name);

      // Update node state to Starting Master.
      createSetNodeStateTask(currentNode, NodeState.Starting)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Set default gflags.
      addDefaultGFlags(taskParams().getClusterByUuid(currentNode.placementUuid).userIntent);

      // Set gflags for master.
      createGFlagsOverrideTasks(ImmutableList.of(currentNode), ServerType.MASTER);

      // Update master configuration on the node.
      createConfigureServerTasks(Arrays.asList(currentNode), true /* isShell */,
          true /* updateMasterAddrs */, true /* isMaster */)
              .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Start a master process.
      createStartMasterTasks(new HashSet<NodeDetails>(Arrays.asList(currentNode)))
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Mark node as isMaster in YW DB.
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.MASTER, true)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Wait for the master to be responsive.
      createWaitForServersTasks(new HashSet<NodeDetails>(Arrays.asList(currentNode)),
          ServerType.MASTER).setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Add master to the quorum.
      createChangeConfigTask(currentNode, true /* isAdd */, SubTaskGroupType.ConfigureUniverse);

      // Update all server conf files with new master information.
      createMasterInfoUpdateTask(universe, currentNode);

      // Update node state to running.
      createSetNodeStateTask(currentNode, NodeDetails.NodeState.Live)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Update the swamper target file.
      createSwamperTargetUpdateTask(false /* removeFile */);

      // Mark universe update success to true.
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

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

      // Mark the update of the universe as done. This will allow future updates to
      // the universe.
      unlockUniverseForUpdate();
    }
    LOG.info("Finished {} task for node {} in univ uuid={}", getName(), taskParams().nodeName,
        taskParams().universeUUID);
  }
}
