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
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.models.helpers.PlacementInfo;

import java.util.Arrays;
import java.util.HashSet;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.api.Play;

// Allows the removal of a node from a universe. Ensures the task waits for the right set of
// server data move primitives. And stops using the underlying instance, though YW still owns it.
public class RemoveNodeFromUniverse extends UniverseTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(RemoveNodeFromUniverse.class);

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
      checkUniverseVersion();
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

      if (currentNode.state != NodeDetails.NodeState.Live
          && currentNode.state != NodeDetails.NodeState.ToBeRemoved
          && currentNode.state != NodeDetails.NodeState.ToJoinCluster) {
        String msg = "Node " + taskParams().nodeName
            + " is not in Live/ToJoinCluster/ToBeRemoved states, but is in " + currentNode.state
            + ", so cannot be removed.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      taskParams().azUuid = currentNode.azUuid;
      taskParams().placementUuid = currentNode.placementUuid;

      String masterAddrs = universe.getMasterAddresses();

      Cluster currCluster = universe.getUniverseDetails()
                                    .getClusterByUuid(taskParams().placementUuid);
      UserIntent userIntent = currCluster.userIntent;
      PlacementInfo pi = currCluster.placementInfo;

      // Update Node State to being removed.
      createSetNodeStateTask(currentNode, NodeState.Removing)
          .setSubTaskGroupType(SubTaskGroupType.RemovingNode);

      boolean instanceAlive = false;
      try {
        instanceAlive = instanceExists(taskParams());
      } catch (Exception e) {
        LOG.info("Instance {} in universe {} not found, assuming dead", taskParams().nodeName,
            universe.name);
      }

      if (instanceAlive) {
        // Remove the master on this node from master quorum and update its state from YW DB,
        // only if it reachable.
        boolean masterReachable = isMasterAliveOnNode(currentNode, masterAddrs);
        LOG.info("Master {}, reachable = {}.", currentNode.cloudInfo.private_ip, masterReachable);
        if (currentNode.isMaster) {
          // Wait for Master Leader before doing any MasterChangeConfig operations.
          createWaitForMasterLeaderTask()
              .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
          if (masterReachable) {
            createChangeConfigTask(currentNode, false, SubTaskGroupType.WaitForDataMigration);
            createStopMasterTasks(new HashSet<>(Arrays.asList(currentNode)))
                .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
            createWaitForMasterLeaderTask()
                .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
          } else {
            createChangeConfigTask(currentNode, false, SubTaskGroupType.WaitForDataMigration, true);
          }
        }

      } else {
        if (currentNode.isMaster) {
          createWaitForMasterLeaderTask()
              .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
          createChangeConfigTask(currentNode, false, SubTaskGroupType.WaitForDataMigration, true);
        }
      }

      // Mark the tserver as blacklisted on the master leader.
      createPlacementInfoTask(new HashSet<>(Arrays.asList(currentNode)))
          .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);

      // Wait for tablet quorums to remove the blacklisted tserver. Do not perform load balance
      // if node is not reachable so as to avoid cases like 1 node in an 3 node cluster is being
      // removed and we know LoadBalancer will not be able to handle that.
      if (instanceAlive) {

        int rfInZone = PlacementInfoUtil.getZoneRF(pi, currentNode.cloudInfo.cloud,
                                                   currentNode.cloudInfo.region,
                                                   currentNode.cloudInfo.az);
        long nodesInZone = PlacementInfoUtil.getNumActiveTserversInZone(
            universe.getNodes(), currentNode.cloudInfo.cloud,
            currentNode.cloudInfo.region, currentNode.cloudInfo.az);

        if (rfInZone == -1 || nodesInZone == 0) {
          throw new RuntimeException("Error getting placement info for cluster with node: " +
                                     currentNode.nodeName);
        }
        // Perform a data migration and stop the tserver process only if it is reachable.
        boolean tserverReachable = isTserverAliveOnNode(currentNode, masterAddrs);
        LOG.info("Tserver {}, reachable = {}.", currentNode.cloudInfo.private_ip, tserverReachable);
        if (tserverReachable) {
          // Since numNodes can never be less, that will mean there is a potential node to move
          // data to.
          if (userIntent.numNodes > userIntent.replicationFactor) {
            // We only want to move data if the number of nodes in the zone are more than the RF
            // of the zone.
            if (nodesInZone > rfInZone) {
              createWaitForDataMoveTask()
                  .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
            }
          }
          createTServerTaskForNode(currentNode, "stop")
              .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);
        }
      }

      // Remove master status (even when it does not exists or is not reachable).
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.MASTER, false)
          .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);

      // Remove its tserver status in DB.
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.TSERVER, false)
          .setSubTaskGroupType(SubTaskGroupType.StoppingNodeProcesses);

      // Update Node State to Removed
      createSetNodeStateTask(currentNode, NodeState.Removed)
          .setSubTaskGroupType(SubTaskGroupType.RemovingNode);

      // Mark universe task state to success
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.RemovingNode);

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
