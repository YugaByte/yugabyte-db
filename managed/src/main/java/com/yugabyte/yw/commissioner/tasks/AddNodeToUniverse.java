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
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

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
    String errorString = null;

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

      if (currentNode.state != NodeState.Removed &&
          currentNode.state != NodeState.Decommissioned) {
        String msg = "Node " + taskParams().nodeName + " is not in removed or decommissioned state"
                     + ", but is in " + currentNode.state + ", so cannot be added.";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }

      Cluster cluster = taskParams().getClusterByUuid(currentNode.placementUuid);
      Collection<NodeDetails> node = Collections.singletonList(currentNode);

      boolean wasDecommissioned = currentNode.state == NodeState.Decommissioned;
      // For onprem universes, allocate an available node
      // from the provider's node_instance table.
      if (wasDecommissioned && cluster.userIntent.providerType.equals(CloudType.onprem)) {
          Map<UUID, List<String>> onpremAzToNodes = new HashMap<UUID, List<String>>();
          List<String> nodeNameList = new ArrayList<>();
          nodeNameList.add(currentNode.nodeName);
          onpremAzToNodes.put(currentNode.azUuid, nodeNameList);
          String instanceType = currentNode.cloudInfo.instance_type;

          Map<String, NodeInstance> nodeMap = NodeInstance.pickNodes(onpremAzToNodes, instanceType);
          currentNode.nodeUuid = nodeMap.get(currentNode.nodeName).nodeUuid;
      }

      NodeTaskParams nodeParams = new NodeTaskParams();
      UserIntent userIntent = taskParams().getClusterByUuid(currentNode.placementUuid).userIntent;
      nodeParams.nodeName = currentNode.nodeName;
      nodeParams.deviceInfo = userIntent.deviceInfo;
      nodeParams.azUuid = currentNode.azUuid;
      nodeParams.universeUUID = taskParams().universeUUID;
      nodeParams.extraDependencies.installNodeExporter =
        taskParams().extraDependencies.installNodeExporter;

      String preflightStatus = performPreflightCheck(currentNode, nodeParams);
      if (preflightStatus != null) {
        Map<NodeInstance, String> failedNodes = new HashMap<>();
        failedNodes.put(NodeInstance.getByName(currentNode.nodeName), preflightStatus);
        createFailedPrecheckTask(failedNodes)
          .setSubTaskGroupType(SubTaskGroupType.PreflightChecks);
        errorString = "Preflight checks failed.";
      } else {
        // Update Node State to being added.
        createSetNodeStateTask(currentNode, NodeState.Adding)
            .setSubTaskGroupType(SubTaskGroupType.StartingNode);

        // First spawn an instance for Decommissioned node.
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

        // Set default gflags
        addDefaultGFlags(cluster.userIntent);

        // All necessary nodes are created. Data moving will coming soon.
        createSetNodeStateTasks(node, NodeDetails.NodeState.ToJoinCluster)
            .setSubTaskGroupType(SubTaskGroupType.Provisioning);

        // Bring up any masters, as needed.
        boolean masterAdded = false;
        if (areMastersUnderReplicated(currentNode, universe)) {
            LOG.info(
            "Bringing up master for under replicated universe {} ({})",
            universe.universeUUID, universe.name
            );
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
            // Update the DNS entry for this universe.
            createDnsManipulationTask(DnsManager.DnsCommandType.Edit, false,
                                      userIntent.providerType, userIntent.provider,
                                      userIntent.universeName)
                .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
        }

        // Mark universe task state to success.
        createMarkUniverseUpdateSuccessTasks()
            .setSubTaskGroupType(SubTaskGroupType.StartingNode);
      }

      // Run all the tasks.
      subTaskGroupQueue.run();
    } catch (Throwable t) {
      LOG.error("Error executing task {} with error='{}'.", getName(), t.getMessage(), t);
      throw t;
    } finally {
      // Mark the update of the universe as done. This will allow future updates to the universe.
      unlockUniverseForUpdate(errorString);
    }
    LOG.info("Finished {} task.", getName());
  }
}
