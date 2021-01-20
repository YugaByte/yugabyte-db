// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import java.util.UUID;
import java.util.concurrent.CopyOnWriteArrayList;

import com.yugabyte.yw.models.TaskInfo;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class SubTaskGroupQueue {

  public static final Logger LOG = LoggerFactory.getLogger(SubTaskGroupQueue.class);

  // The list of tasks lists in this task list sequence.
  CopyOnWriteArrayList<SubTaskGroup> subTaskGroups = new CopyOnWriteArrayList<SubTaskGroup>();

  private UUID userTaskUUID;

  public SubTaskGroupQueue(UUID userTaskUUID) {
    this.userTaskUUID = userTaskUUID;
  }

  /**
   * Add a task list to this sequence.
   */
  public boolean add(SubTaskGroup subTaskGroup) {
    subTaskGroup.setTaskContext(subTaskGroups.size(), userTaskUUID);
    return subTaskGroups.add(subTaskGroup);
  }

  /**
   * Execute the sequence of task lists in a sequential manner.
   */
  public void run() {
    boolean runSuccess = true;
    for (SubTaskGroup subTaskGroup : subTaskGroups) {
      boolean subTaskGroupSuccess = false;
      subTaskGroup.setUserSubTaskState(TaskInfo.State.Running);
      try {
        subTaskGroup.run();
        subTaskGroupSuccess = subTaskGroup.waitFor();
      } catch (Throwable t) {
        // Update task state to failure
        subTaskGroup.setUserSubTaskState(TaskInfo.State.Failure);
        if (!subTaskGroup.ignoreErrors) {
          throw t;
        }
      }

      if (!subTaskGroupSuccess) {
        LOG.error("SubTaskGroup '{}' waitFor() returned failed status.", subTaskGroup.toString());
        subTaskGroup.setUserSubTaskState(TaskInfo.State.Failure);
        if (!subTaskGroup.ignoreErrors) {
          throw new RuntimeException(subTaskGroup.toString() + " failed.");
        }
      }

      runSuccess = runSuccess && subTaskGroupSuccess;

      if (subTaskGroupSuccess) subTaskGroup.setUserSubTaskState(TaskInfo.State.Success);
    }

    if (!runSuccess) throw new RuntimeException("One or more subTaskGroups failed while running.");
  }
}
