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

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;

import java.util.Set;

import static com.yugabyte.yw.common.PlacementInfoUtil.*;

public class DeleteNode extends NodeTaskBase {

  @Override
  public void run() {
    try {
      LOG.info("Running {}", getName());
      // Create the update lambda.
      Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
        @Override
        public void run(Universe universe) {
          // If this universe is not being edited, fail the request.
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          NodeDetails nodeDetails = universe.getNode(taskParams().nodeName);
          if (nodeDetails == null) {
            throw new RuntimeException("No node in universe with name " + taskParams().nodeName);
          }
          Cluster cluster = universeDetails.getClusterByUuid(nodeDetails.placementUuid);

          if (isNodeRemovable(taskParams().nodeName, universeDetails.nodeDetailsSet)) {
            // Remove Node from universe node detail set
            removeNodeByName(taskParams().nodeName, universeDetails.nodeDetailsSet);
            // Update Placement Info to reflect new nodeDetailSet
            Set<NodeDetails> clusterNodes = universeDetails.getNodesInCluster(cluster.uuid);
            updatePlacementInfo(clusterNodes, cluster.placementInfo);

            // Update userIntent to reflect new numNodes
            cluster.userIntent.numNodes = universeDetails.nodeDetailsSet.size();
            // If OnPrem Free up the node.
            if (cluster.userIntent.providerType.equals(Common.CloudType.onprem)) {
              try {
                NodeInstance node = NodeInstance.getByName(taskParams().nodeName);
                node.clearNodeDetails();
              } catch (Exception ex) {
                LOG.warn("On-prem node {} in universe {} doesn't have a linked instance. "
                    + "Deletion is skipped.", taskParams().nodeName, universe.name);
              }
            }
            universe.setUniverseDetails(universeDetails);
          }
        }
      };
      // Perform the update. If unsuccessful, this will throw a runtime exception which we do not
      // catch as we want to fail.
      saveUniverseDetails(updater);
    } catch (Exception e) {
      String msg = getName() + " failed with exception "  + e.getMessage();
      LOG.warn(msg, e.getMessage());
      throw new RuntimeException(msg, e);
    }
  }
}
