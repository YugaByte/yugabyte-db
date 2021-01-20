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

import java.util.ArrayList;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;

// Tracks edit intents to the cluster and then performs the sequence of configuration changes on
// this universe to go from the current set of master/tserver nodes to the final configuration.
public class EditUniverse extends UniverseDefinitionTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(EditUniverse.class);

  // Get the new masters from the node list.
  Set<NodeDetails> newMasters = new HashSet<NodeDetails>();

  // Masters that need to be removed, if any.
  Set<NodeDetails> removeMasters = new HashSet<NodeDetails>();

  @Override
  public void run() {
    LOG.info("Started {} task for uuid={}", getName(), taskParams().universeUUID);
    String errorString = null;

    try {
      checkUniverseVersion();
      // Verify the task params.
      verifyParams(UniverseOpType.EDIT);

      // Create the task list sequence.
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      // Update the universe DB with the changes to be performed and set the 'updateInProgress' flag
      // to prevent other updates from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      // Set all the node names.
      setNodeNames(UniverseOpType.EDIT, universe);

      // Run preflight checks on onprem nodes to be added.
      if (reserveAndCheckOnpremNodesToBeAdded()) {
        // Select master nodes, if needed.
        selectMasters();

        // Update the user intent.
        writeUserIntentToUniverse(false, false);

        for (Cluster cluster : taskParams().clusters) {
          addDefaultGFlags(cluster.userIntent);
          editCluster(universe, cluster);
        }

        // Update the DNS entry for this universe, based in primary provider info.
        UserIntent primaryIntent = universe.getUniverseDetails().getPrimaryCluster().userIntent;
        createDnsManipulationTask(DnsManager.DnsCommandType.Edit, false, primaryIntent.providerType,
                primaryIntent.provider, primaryIntent.universeName)
                .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

        // Marks the update of this universe as a success only if all the tasks before it succeeded.
        createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
      } else {
        errorString = "Preflight checks failed.";
      }

      // Run all the tasks.
      subTaskGroupQueue.run();
    } catch (Throwable t) {
      LOG.error("Error executing task {} with error='{}'.", getName(), t.getMessage(), t);
      throw t;
    } finally {
      // Mark the update of the universe as done. This will allow future edits/updates to the
      // universe to happen.
      unlockUniverseForUpdate(errorString);
    }
    LOG.info("Finished {} task.", getName());
  }

  private void editCluster(Universe universe, Cluster cluster) {
    UserIntent userIntent = cluster.userIntent;
    Set<NodeDetails> nodes = taskParams().getNodesInCluster(cluster.uuid);

    LOG.info("Configure numNodes={}, Replication factor={}", userIntent.numNodes,
             userIntent.replicationFactor);

    Collection<NodeDetails> nodesToBeRemoved = PlacementInfoUtil.getNodesToBeRemoved(nodes);

    Collection<NodeDetails> nodesToProvision = PlacementInfoUtil.getNodesToProvision(nodes);

    // Set the old nodes' state to to-be-removed.
    if (!nodesToBeRemoved.isEmpty()) {
      createSetNodeStateTasks(nodesToBeRemoved, NodeDetails.NodeState.ToBeRemoved)
          .setSubTaskGroupType(SubTaskGroupType.Provisioning);
    }

    // Update any tags on nodes that are not going to be removed and not being added.
    Cluster existingCluster = universe.getCluster(cluster.uuid);
    if (!cluster.areTagsSame(existingCluster)) {
      LOG.info("Tags changed from '{}' to '{}'.", existingCluster.userIntent.instanceTags,
               cluster.userIntent.instanceTags);
      createUpdateInstanceTagsTasks(PlacementInfoUtil.getLiveNodes(nodes),
                                    Util.getKeysNotPresent(existingCluster.userIntent.instanceTags,
                                                           cluster.userIntent.instanceTags));
    }

    if (!nodesToProvision.isEmpty()) {
      // Create the required number of nodes in the appropriate locations.
      createSetupServerTasks(nodesToProvision)
          .setSubTaskGroupType(SubTaskGroupType.Provisioning);

      // Get all information about the nodes of the cluster. This includes the public ip address,
      // the private ip address (in the case of AWS), etc.
      createServerInfoTasks(nodesToProvision)
          .setSubTaskGroupType(SubTaskGroupType.Provisioning);

      // Configures and deploys software on all the nodes (masters and tservers).
      createConfigureServerTasks(nodesToProvision, true /* isShell */)
          .setSubTaskGroupType(SubTaskGroupType.InstallingSoftware);

      // Override master (on primary cluster only) and tserver flags as necessary.
      if (cluster.clusterType == ClusterType.PRIMARY) {
        createGFlagsOverrideTasks(nodesToProvision, ServerType.MASTER);
      }
      createGFlagsOverrideTasks(nodesToProvision, ServerType.TSERVER);
    }

    newMasters = PlacementInfoUtil.getMastersToProvision(nodes);
    removeMasters = PlacementInfoUtil.getMastersToBeRemoved(nodes);

    // Ensure all masters are covered in nodes to be removed.
    if (!removeMasters.isEmpty()) {
      if (nodesToBeRemoved.isEmpty()) {
        String errMsg = "If masters are being removed, corresponding nodes need removal too.";
        LOG.error(errMsg + " masters: " + nodeNames(removeMasters));
        throw new IllegalStateException(errMsg);
      }
      if (!nodesToBeRemoved.containsAll(removeMasters)) {
        String errMsg = "If masters are being removed, all those nodes need removal too.";
        LOG.error(errMsg + " masters: " + nodeNames(removeMasters) + " , but removing only " +
                  nodeNames(nodesToBeRemoved));
        throw new IllegalStateException(errMsg);
      }
    }

    // All necessary nodes are created. Data moving will coming soon.
    createSetNodeStateTasks(nodesToProvision, NodeDetails.NodeState.ToJoinCluster)
        .setSubTaskGroupType(SubTaskGroupType.Provisioning);

    // Creates the primary cluster by first starting the masters.
    if (!newMasters.isEmpty()) {
      if (cluster.clusterType == ClusterType.ASYNC) {
        String errMsg = "Read-only cluster " + cluster.uuid + " should not have masters.";
        LOG.error(errMsg);
        throw new IllegalStateException(errMsg);
      }

      createStartMasterTasks(newMasters)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Wait for masters to be responsive.
      createWaitForServersTasks(newMasters, ServerType.MASTER)
         .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    }

    Set<NodeDetails> newTservers = PlacementInfoUtil.getTserversToProvision(nodes);

    if (!newTservers.isEmpty()) {
      // Start the tservers in the clusters.
      createStartTServersTasks(newTservers)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Wait for all tablet servers to be responsive.
      createWaitForServersTasks(newTservers, ServerType.TSERVER)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Remove them from blacklist, in case master is still tracking these.
      createModifyBlackListTask(new ArrayList(newTservers), false /* isAdd */)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    }

    if (!nodesToProvision.isEmpty()) {
      // Set the new nodes' state to live.
      createSetNodeStateTasks(nodesToProvision, NodeDetails.NodeState.Live)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    }

    Collection<NodeDetails> tserversToBeRemoved = PlacementInfoUtil.getTserversToBeRemoved(nodes);

    // Persist the placement info and blacklisted node info into the YB master.
    // This is done after master config change jobs, so that the new master leader can perform
    // the auto load-balancing, and all tablet servers are heart beating to new set of masters.
    if (!nodesToBeRemoved.isEmpty()) {
      // Add any nodes to be removed to tserver removal to be considered for blacklisting.
      tserversToBeRemoved.addAll(nodesToBeRemoved);
    }

    // Update the blacklist servers on master leader.
    createPlacementInfoTask(tserversToBeRemoved)
        .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);

    // Update the swamper target file.
    createSwamperTargetUpdateTask(false /* removeFile */);

    if (!nodesToBeRemoved.isEmpty()) {
      // Wait for %age completion of the tablet move from master.
      createWaitForDataMoveTask()
          .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
    } else {
      if (!tserversToBeRemoved.isEmpty()) {
        String errMsg = "Universe shrink should have been handled using node decommision.";
        LOG.error(errMsg);
        throw new IllegalStateException(errMsg);
      }
      // If only tservers are added, wait for load to balance across all tservers.
      createWaitForLoadBalanceTask()
          .setSubTaskGroupType(SubTaskGroupType.WaitForDataMigration);
    }

    if (cluster.clusterType == ClusterType.PRIMARY &&
        PlacementInfoUtil.didAffinitizedLeadersChange(
            universe.getUniverseDetails().getPrimaryCluster().placementInfo,
            cluster.placementInfo)) {
      createWaitForLeadersOnPreferredOnlyTask();
    }

    if (!newMasters.isEmpty()) {
      // Now finalize the master quorum change tasks.
      createMoveMastersTasks(SubTaskGroupType.WaitForDataMigration);

      // Wait for a master leader to be elected.
      createWaitForMasterLeaderTask()
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Update these older ones to be not masters anymore so tserver info can be updated with the
      // final master list and other future cluster client operations.
      createUpdateNodeProcessTasks(removeMasters, ServerType.MASTER, false);

      // Change the master addresses in the conf file for the new tservers.
      createConfigureServerTasks(newTservers, false /* isShell */, true /* updateMasterAddrs */);
      createSetFlagInMemoryTasks(newTservers, ServerType.TSERVER, true /* force flag update */,
                                 null /* no gflag to update */, true /* updateMasterAddrs */);

      // Change the master addresses in the conf file for the new masters.
      createConfigureServerTasks(newMasters, false /* isShell */, true /* updateMasterAddrs */,
                                 true /* isMaster */);
      createSetFlagInMemoryTasks(newMasters, ServerType.MASTER, true /* force flag update */,
                                 null /* no gflag to update */, true /* updateMasterAddrs */);

      // Wait for the master leader to hear from all tservers.
      createWaitForTServerHeartBeatsTask()
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    }

    // Finally send destroy to the old set of nodes and remove them from this universe.
    if (!nodesToBeRemoved.isEmpty()) {
      createDestroyServerTasks(nodesToBeRemoved, false /* isForceDelete */, true /* deleteNode */)
          .setSubTaskGroupType(SubTaskGroupType.RemovingUnusedServers);
    }
  }

  /**
   * Fills in the series of steps needed to move the masters using the node names. The
   * actual node details (such as ip addresses) are found at runtime by querying the database.
   */
  private void createMoveMastersTasks(SubTaskGroupType subTask) {
    // Get the list of node names to add as masters.
    List<NodeDetails> mastersToAdd = new ArrayList<>();
    for (NodeDetails node : newMasters) {
      mastersToAdd.add(node);
    }

    // Get the list of node names to remove as masters.
    List<NodeDetails> mastersToRemove = new ArrayList<>();
    for (NodeDetails node : removeMasters) {
      mastersToRemove.add(node);
    }

    // Find the minimum number of master changes where we can perform an add followed by a remove.
    int numIters = Math.min(mastersToAdd.size(), mastersToRemove.size());

    // Perform a master add followed by a remove if possible. Need not remove the (current) master
    // leader last - even if we get current leader, it might change by the time we run the actual
    // task. So we might do multiple leader stepdown's, which happens automatically in the
    // client code during the task's run.
    for (int idx = 0; idx < numIters; idx++) {
      createChangeConfigTask(mastersToAdd.get(idx), true, subTask);
      createChangeConfigTask(mastersToRemove.get(idx), false, subTask);
    }

    // Perform any additions still left.
    for (int idx = numIters; idx < newMasters.size(); idx++) {
      createChangeConfigTask(mastersToAdd.get(idx), true, subTask);
    }

    // Perform any removals still left.
    for (int idx = numIters; idx < removeMasters.size(); idx++) {
      createChangeConfigTask(mastersToRemove.get(idx), false, subTask);
    }
  }
}
