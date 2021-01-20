/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Universe.UniverseUpdater;
import com.yugabyte.yw.models.helpers.NodeDetails;

public class AnsibleDestroyServer extends NodeTaskBase {

  public static class Params extends NodeTaskParams {
    // Flag to be set where errors from ansible will be ignored.
    public boolean isForceDelete;
    // Flag to track if node info should be deleted from universe db.
    public boolean deleteNode = true;
    // IP of node to be deleted.
    public String nodeIP = null;
  }

  @Override
  protected AnsibleDestroyServer.Params taskParams() {
    return (AnsibleDestroyServer.Params)taskParams;
  }

  public static final Logger LOG = LoggerFactory.getLogger(AnsibleDestroyServer.class);

  private void removeNodeFromUniverse(final String nodeName) {
    Universe u = Universe.get(taskParams().universeUUID);
    if (u.getNode(nodeName) == null) {
      LOG.error("No node in universe with name " + nodeName);
      return;
    }
    // Persist the desired node information into the DB.
    UniverseUpdater updater = new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        universeDetails.removeNode(nodeName);
        LOG.debug("Removing node " + nodeName + " from universe " + taskParams().universeUUID);
      }
    };

    saveUniverseDetails(updater);
  }

  @Override
  public void run() {
    // Update the node state as removing.
    setNodeState(NodeDetails.NodeState.Removing);
    // Execute the ansible command.
    try {
      ShellResponse response = getNodeManager().nodeCommand(
        NodeManager.NodeCommandType.Destroy, taskParams());
      processShellResponse(response);
    } catch (Exception e) {
      if (!taskParams().isForceDelete) {
        throw e;
      } else {
        LOG.debug("Ignoring error deleting {} due to isForceDelete being set.",
                  taskParams().nodeName, e);
      }
    }

    Universe u = Universe.get(taskParams().universeUUID);
    UserIntent userIntent = u.getUniverseDetails()
        .getClusterByUuid(u.getNode(taskParams().nodeName).placementUuid).userIntent;
    NodeDetails univNodeDetails = u.getNode(taskParams().nodeName);

    if (userIntent.providerType.equals(Common.CloudType.onprem) &&
        univNodeDetails.state != NodeDetails.NodeState.Decommissioned) {
      // Free up the node.
      try {
        NodeInstance providerNode = NodeInstance.getByName(taskParams().nodeName);
        providerNode.clearNodeDetails();
      } catch (Exception e) {
        if (!taskParams().isForceDelete) {
          throw e;
        }
      }
      LOG.info("Marked node instance {} as available", taskParams().nodeName);
    }

    if (taskParams().deleteNode) {
      // Update the node state to removed. Even though we remove the node below, this will
      // help tracking state for any nodes stuck in limbo.
      setNodeState(NodeDetails.NodeState.Removed);

      removeNodeFromUniverse(taskParams().nodeName);
    }
  }
}
