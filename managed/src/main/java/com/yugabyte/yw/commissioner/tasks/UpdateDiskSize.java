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

import com.yugabyte.yw.commissioner.SubTaskGroup;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.DiskIncreaseFormData;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;

import java.util.Set;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class UpdateDiskSize extends UniverseDefinitionTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(UpdateDiskSize.class);

  public static class Params extends DiskIncreaseFormData {}

  @Override
  protected DiskIncreaseFormData taskParams() {
    return (DiskIncreaseFormData)taskParams;
  }

  @Override
  public void run() {
    try {
      checkUniverseVersion();
      // Create the task list sequence.
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      // Update the universe DB with the update to be performed and set the 'updateInProgress' flag
      // to prevent other updates from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      // Update the user intent.
      writeUserIntentToUniverse();

      Cluster primaryCluster = universe.getUniverseDetails().getPrimaryCluster();

      Set<NodeDetails> nodes = taskParams().getNodesInCluster(primaryCluster.uuid);

      // Create Task to update the disk size.
      createUpdateDiskSizeTasks(PlacementInfoUtil.getLiveNodes(nodes));

      // Marks update of this universe as a success only if all the tasks before it succeeded.
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Run all the tasks.
      subTaskGroupQueue.run();
    } catch (Throwable t) {
      LOG.error("Error executing task {} with error={}.", getName(), t);
      throw t;
    } finally {
      unlockUniverseForUpdate();
    }
    LOG.info("Finished {} task.", getName());
  }
}
