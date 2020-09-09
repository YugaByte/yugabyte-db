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
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

import java.util.Arrays;
import java.util.Collection;
import java.util.HashSet;
import java.util.Set;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import static com.yugabyte.yw.common.Util.areMastersUnderReplicated;

// Allows the addition of a node into a universe. Spawns the necessary processes - tserver
// and/or master and ensures the task waits for the right set of load balance primitives.
public class AddNodeToUniverse extends UniverseDefinitionTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(AddNodeToUniverse.class);

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

      // Update the DB to prevent other changes from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      currentNode = universe.getNode(taskParams().nodeName);
      if (currentNode == null) {
        String msg = "No node " + taskParams().nodeName + " in universe " + universe.name;
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      if (currentNode.state != NodeState.Removed &&
          currentNode.state != NodeState.Decommissioned) {
        String msg = "Node " + taskParams().nodeName + " is not in removed or decommissioned state"
                     + ", but is in " + currentNode.state + ", so cannot be added.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      // Update Node State to being added.
      createSetNodeStateTask(currentNode, NodeState.Adding)
          .setSubTaskGroupType(SubTaskGroupType.StartingNode);

      Collection<NodeDetails> node = new HashSet<NodeDetails>(Arrays.asList(currentNode));

      // First spawn an instance for Decommissioned node.
      boolean wasDecommissioned = currentNode.state == NodeState.Decommissioned;
      if (wasDecommissioned) {
        createSetupServerTasks(node)
            .setSubTaskGroupType(SubTaskGroupType.Provisioning);

        createServerInfoTasks(node)
            .setSubTaskGroupType(SubTaskGroupType.Provisioning);
      }

      // Re-install software.
      // TODO: Remove the need for version for existing instance, NodeManger needs changes.
      createConfigureServerTasks(node, true /* isShell */)
          .setSubTaskGroupType(SubTaskGroupType.InstallingSoftware);

      // Bring up any masters, as needed.
      boolean masterAdded = false;
      if (areMastersUnderReplicated(currentNode, universe)) {
        // Set gflags for master.
        createGFlagsOverrideTasks(node, ServerType.MASTER);

        // Start a shell master process.
        createStartMasterTasks(node)
            .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

        // Mark node as a master in YW DB.
        // Do this last so that master addresses does not pick up current node.
        createUpdateNodeProcessTask(taskParams().nodeName, ServerType.MASTER, true)
            .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

        // Wait for master to be responsive.
        createWaitForServersTasks(node, ServerType.MASTER)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

        // Add it into the master quorum.
        createChangeConfigTask(currentNode, true, SubTaskGroupType.WaitForDataMigration);

        masterAdded = true;
      }

      Cluster cluster = taskParams().getClusterByUuid(currentNode.placementUuid);

      UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();

      // Explicitly set webserver ports for each dql
      cluster.userIntent.tserverGFlags.put(
        "redis_proxy_webserver_port",
        Integer.toString(universeDetails.communicationPorts.redisServerHttpPort)
      );
      cluster.userIntent.tserverGFlags.put(
        "cql_proxy_webserver_port",
        Integer.toString(universeDetails.communicationPorts.yqlServerHttpPort)
      );
      if (universe.getUniverseDetails().getPrimaryCluster().userIntent.enableYSQL) {
        cluster.userIntent.tserverGFlags.put(
          "pgsql_proxy_webserver_port",
          Integer.toString(universeDetails.communicationPorts.ysqlServerHttpPort)
        );
      }

      // Set gflags for the tserver.
      createGFlagsOverrideTasks(node, ServerType.TSERVER);

      // Add the tserver process start task.
      createTServerTaskForNode(currentNode, "start")
          .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

      // Mark the node as tserver in the YW DB.
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.TSERVER, true)
          .setSubTaskGroupType(SubTaskGroupType.StartingNodeProcesses);

      // Wait for new tablet servers to be responsive.
      createWaitForServersTasks(node, ServerType.TSERVER)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Update the swamper target file.
      createSwamperTargetUpdateTask(false /* removeFile */);

      // Clear the host from master's blacklist.
      if (currentNode.state == NodeState.Removed) {
        createModifyBlackListTask(Arrays.asList(currentNode), false /* isAdd */)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
      }

      // Wait for load to balance.
      createWaitForLoadBalanceTask()
          .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);

      // Update all tserver conf files with new master information.
      if (masterAdded) {
        createMasterInfoUpdateTask(universe, currentNode);
      }

      // Update node state to live.
      createSetNodeStateTask(currentNode, NodeState.Live)
          .setSubTaskGroupType(SubTaskGroupType.StartingNode);

      if (wasDecommissioned) {
        UserIntent userIntent = universe.getUniverseDetails()
                                        .getClusterByUuid(currentNode.placementUuid)
                                        .userIntent;

        // Update the DNS entry for this universe.
        createDnsManipulationTask(DnsManager.DnsCommandType.Edit, false, userIntent.providerType,
                                  userIntent.provider, userIntent.universeName)
            .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
      }

      // Mark universe task state to success.
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.StartingNode);

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

      // Mark the update of the universe as done. This will allow future updates to the universe.
      unlockUniverseForUpdate();
    }
    LOG.info("Finished {} task.", getName());
  }

  // Setup a configure task to update the new master list in the conf files of all servers.
  // Skip the newly added node as it would have gotten the new master list after provisioing.
  private void createMasterInfoUpdateTask(Universe universe, NodeDetails addedNode) {
    Set<NodeDetails> tserverNodes = new HashSet<NodeDetails>(universe.getTServers());
    Set<NodeDetails> masterNodes = new HashSet<NodeDetails>(universe.getMasters());
    // We need to add the node explicitly since the node wasn't marked as a master or tserver
    // before the task is completed.
    tserverNodes.add(addedNode);
    masterNodes.add(addedNode);
    // Configure all tservers to pick the new master node ip as well.
    createConfigureServerTasks(tserverNodes, false /* isShell */, true /* updateMasterAddr */)
        .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    // Update the master addresses in memory.
    createSetFlagInMemoryTasks(tserverNodes, ServerType.TSERVER, true /* force flag update */,
                               null /* no gflag to update */, true /* updateMasterAddr */);
    // Change the master addresses in the conf file for the all masters to reflect the changes.
    createConfigureServerTasks(masterNodes, false /* isShell */, true /* updateMasterAddrs */,
        true /* isMaster */).setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    createSetFlagInMemoryTasks(masterNodes, ServerType.MASTER, true /* force flag update */,
                               null /* no gflag to update */, true /* updateMasterAddr */);
  }
}
