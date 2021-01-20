// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.commissioner;

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.forms.ITaskParams;

import java.util.UUID;

public interface ITask extends Runnable {

  /**
   * Initialize the task by reading various parameters.
   */
  public void initialize(ITaskParams taskParams);

  /**
   * A short name representing the task.
   */
  public String getName();

  /**
   * Return a string representation (usually JSON) of the task details. This is used to describe the
   * task to a user in a read-only mode.
   */
  public JsonNode getTaskDetails();

  /**
   * Run the task. Can throw runtime exception on errors.
   */
  @Override
  public void run();

  /**
   * A friendly string representation of the task used for logging.
   * @return string representation of the task.
   */
  @Override
  public String toString();

  /**
   * Set the user-facing top-level task for the Task tree that this Task belongs to.
   * E.g. CreateUniverse, EditUniverse, etc.
   * @param userTaskUUID UUID of the user-facing top-level task for this Task's Task tree.
   */
  public void setUserTaskUUID(UUID userTaskUUID);

  public boolean shouldSendNotification();

  public void sendNotification();
}
