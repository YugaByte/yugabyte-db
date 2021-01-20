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

import java.util.Map;

import com.yugabyte.yw.commissioner.tasks.UniverseTaskBase;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.forms.UniverseTaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Universe.UniverseUpdater;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class UpdateAndPersistGFlags extends UniverseTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(UpdateAndPersistGFlags.class);

  // Parameters for setting and persisting universe gflags.
  public static class Params extends UniverseTaskParams {
    public Map<String, String> masterGFlags;
    public Map<String, String> tserverGFlags;
  }

  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public String getName() {
    return super.getName() + "(" + taskParams().universeUUID + ")";
  }

  @Override
  public void run() {
    try {
      LOG.info("Running {}", getName());

      // Create the update lambda.
      UniverseUpdater updater = new UniverseUpdater() {
        @Override
        public void run(Universe universe) {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          // If this universe is not being updated, fail the request.
          if (!universeDetails.updateInProgress) {
            String msg = "UserUniverse " + taskParams().universeUUID + " is not being updated.";
            LOG.error(msg);
            throw new RuntimeException(msg);
          }

          // Update the gflags.
          UserIntent userIntent = universeDetails.getPrimaryCluster().userIntent;
          userIntent.masterGFlags = taskParams().masterGFlags;
          userIntent.tserverGFlags = taskParams().tserverGFlags;
          for (Cluster cluster : universeDetails.getReadOnlyClusters()) {
            cluster.userIntent.masterGFlags = taskParams().masterGFlags;
            cluster.userIntent.tserverGFlags = taskParams().tserverGFlags;
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
