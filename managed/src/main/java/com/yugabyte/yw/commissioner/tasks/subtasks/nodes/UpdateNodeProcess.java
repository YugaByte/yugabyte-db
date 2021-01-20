/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks.nodes;

import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.commissioner.tasks.subtasks.NodeTaskBase;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;

public class UpdateNodeProcess extends NodeTaskBase {

  // Parameters for updateProcess type
  public static class Params extends NodeTaskParams {
    public Boolean isAdd;
    public UniverseDefinitionTaskBase.ServerType processType;
  }

  protected UpdateNodeProcess.Params params()
  {
    return (UpdateNodeProcess.Params)taskParams;
  }

  @Override
  public void run() {
    try {
      LOG.info("Running {}", getName());
      /**
       * Current node process is either started or stopped
       * This lambda updates the universe definition task param with the same.
       * For instance if the node is stopped, the isMaster and isTServer flags
       * are set to false.
       */
      Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
        @Override
        public void run(Universe universe) {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          for (NodeDetails currentNode : universeDetails.nodeDetailsSet) {
            if (currentNode.nodeName.equals(params().nodeName)) {
              if (params().processType == UniverseDefinitionTaskBase.ServerType.MASTER) {
                currentNode.isMaster = params().isAdd;
              } else {
                currentNode.isTserver = params().isAdd;
              }
            }
          }
          universe.setUniverseDetails(universeDetails);
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
