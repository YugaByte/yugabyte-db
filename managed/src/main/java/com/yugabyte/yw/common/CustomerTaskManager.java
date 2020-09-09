// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import io.ebean.Ebean;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import static com.yugabyte.yw.models.CustomerTask.TargetType;
import javax.inject.Singleton;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

@Singleton
public class CustomerTaskManager {

  public static final Logger LOG = LoggerFactory.getLogger(CustomerTaskManager.class);

  public void failPendingTask(CustomerTask customerTask, TaskInfo taskInfo) {
    try {
      // Mark each subtask as a failure
      taskInfo.getIncompleteSubTasks().forEach(subtask -> {
        subtask.setTaskState(TaskInfo.State.Failure);
        subtask.save();
      });
      // Mark task as a failure
      taskInfo.setTaskState(TaskInfo.State.Failure);
      taskInfo.save();
      // Mark customer task as completed
      customerTask.markAsCompleted();

      // Unlock the universe for future operations
      if (customerTask.getTarget().equals(TargetType.Universe)) {
        // Create the update lambda.
        Universe.UniverseUpdater updater = universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          if (universeDetails.updateInProgress) {
            universeDetails.updateInProgress = false;
            universe.setUniverseDetails(universeDetails);
          }
        };

        Universe.saveDetails(customerTask.getTargetUUID(), updater);
        LOG.debug("Unlocked universe {} for updates.", customerTask.getTargetUUID());
      }
    } catch (Exception e) {
      LOG.error(String.format("Error encountered failing task %s", customerTask.getTaskUUID()), e);
    }
  }

  public void failAllPendingTasks() {
    LOG.info("Failing incomplete tasks...");
    try {
      // Retrieve all incomplete customer tasks
      String query = "SELECT ti.uuid AS task_uuid, ct.id AS customer_task_id " +
        "FROM task_info ti, customer_task ct " +
        "WHERE ti.uuid = ct.task_uuid " +
        "AND ct.completion_time IS NULL " +
        "AND ti.task_state IN ('Created', 'Initializing', 'Running')";
      Ebean.createSqlQuery(query).findList().forEach(row -> {
        TaskInfo taskInfo = TaskInfo.get(row.getUUID("task_uuid"));
        CustomerTask customerTask = CustomerTask.get(row.getLong("customer_task_id"));
        failPendingTask(customerTask, taskInfo);
      });
    } catch (Exception e) {
      LOG.error("Encountered error failing pending tasks", e);
    }
  }
}
