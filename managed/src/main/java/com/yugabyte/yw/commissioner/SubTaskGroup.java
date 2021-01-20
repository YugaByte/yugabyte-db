// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.helpers.TaskType;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class SubTaskGroup implements Runnable {

  public static final Logger LOG = LoggerFactory.getLogger(SubTaskGroup.class);

  // User facing subtask. If this field is 'Invalid', the state of this task list  should
  // not be exposed to the user. Note that multiple task lists can be combined into a single user
  // facing entry by providing the same subtask id.
  private UserTaskDetails.SubTaskGroupType subTaskGroupType = UserTaskDetails.SubTaskGroupType.Invalid;

  // The state of the task to be displayed to the user.
  private TaskInfo.State userSubTaskState = TaskInfo.State.Initializing;

  // Task list name.
  private String name;

  // The list of tasks in this task list.
  private Map<AbstractTaskBase, TaskInfo> taskMap;

  // The list of futures to wait for.
  private Map<Future<?>, TaskInfo> futuresMap;

  private AtomicInteger numTasksCompleted;

  // The number of threads to run in parallel.
  int numThreads;

  // The threadpool executor in case parallel execution is requested.
  ExecutorService executor;

  // Flag to denote the task is done.
  boolean tasksDone = false;

  // Flag to denote if an exception needs to be thrown on failure.
  boolean ignoreErrors = false;

  /**
   * Creates the task list.
   *
   * @param name     : Name for the task list, used to name the threads.
   * @param executor : The threadpool to run the task on.
   */
  public SubTaskGroup(String name, ExecutorService executor) {
    this(name, executor, false);
  }

  /**
   * Creates the task list.
   *
   * @param name     : Name for the task list, used to name the threads.
   * @param executor : The threadpool to run the task on.
   * @param ignoreErrors : Flag to tell if an error needs to be thrown if the subTask fails.
   */
  public SubTaskGroup(String name, ExecutorService executor, boolean ignoreErrors) {
    this.name = name;
    this.executor = executor;
    this.taskMap = new HashMap<>();
    this.futuresMap = new HashMap<>();
    this.numTasksCompleted = new AtomicInteger(0);
    this.ignoreErrors = ignoreErrors;
  }

  public synchronized void setSubTaskGroupType(UserTaskDetails.SubTaskGroupType subTaskGroupType) {
    this.subTaskGroupType = subTaskGroupType;
    for (TaskInfo taskInfo : taskMap.values()) {
      taskInfo.setSubTaskGroupType(subTaskGroupType);
      taskInfo.save();
    }
  }

  public UserTaskDetails.SubTaskGroupType getSubTaskGroupType() {
    return subTaskGroupType;
  }

  public synchronized void setUserSubTaskState(TaskInfo.State userTaskState) {
    this.userSubTaskState = userTaskState;
    for (TaskInfo taskInfo : taskMap.values()) {
      taskInfo.setTaskState(userTaskState);
      taskInfo.save();
    }
  }

  public synchronized TaskInfo.State getUserSubTaskState() {
    return userSubTaskState;
  }

  public String getName() {
    return name;
  }

  @Override
  public String toString() {
    return getName() + " : completed " + getNumTasksDone() + " out of " + getNumTasks() + " tasks.";
  }

  public void addTask(AbstractTaskBase task) {
    LOG.info("Adding task #" + taskMap.size() + ": " + task.getName());
    LOG.debug("Details for task #" + taskMap.size() + ": " + task.toString());

    // Set up corresponding TaskInfo.
    TaskType taskType = TaskType.valueOf(task.getClass().getSimpleName());
    TaskInfo taskInfo = new TaskInfo(taskType);
    taskInfo.setTaskDetails(task.getTaskDetails());
    // Set the owner info in the TaskInfo.
    String hostname = "";
    try {
      hostname = InetAddress.getLocalHost().getHostName();
    } catch (UnknownHostException e) {
      LOG.error("Could not determine the hostname", e);
    }
    taskInfo.setOwner(hostname);
    // Set the SubTaskGroupType in TaskInfo
    if (this.subTaskGroupType != null) {
      taskInfo.setSubTaskGroupType(this.subTaskGroupType);
    }
    taskInfo.save();
    taskMap.put(task, taskInfo);
  }

  public int getNumTasks() {
    return taskMap.size();
  }

  public int getNumTasksDone() {
    return numTasksCompleted.get();
  }

  public void setTaskContext(int position, UUID userTaskUUID) {
    for (TaskInfo taskInfo : taskMap.values()) {
      taskInfo.setPosition(position);
      taskInfo.setParentUuid(userTaskUUID);
      taskInfo.save();
    }
  }

  /**
   * Asynchronously starts the tasks and returns. To wait for the tasks to complete, call the
   * waitFor() method.
   */
  @Override
  public void run() {
    if (taskMap.isEmpty()) {
      LOG.error("No tasks in task list {}.", getName());
      tasksDone = true;
      return;
    }
    LOG.info("Running task list {}.", getName());
    for (AbstractTaskBase task : taskMap.keySet()) {
      Future<?> future = executor.submit(task);
      futuresMap.put(future, taskMap.get(task));
    }
  }

  public boolean waitFor() {
    boolean hasErrored = false;
    for (Future<?> future : futuresMap.keySet()) {
      TaskInfo taskInfo = futuresMap.get(future);

      // Wait for each future to finish.
      String errorString = null;
      try {
        if (future.get() == null) {
          // Task succeeded.
          numTasksCompleted.incrementAndGet();
        } else {
          errorString = "ERROR: while running task " + taskInfo.toString() +
                        " " + taskInfo.getTaskUUID().toString();
          LOG.error(errorString);
        }
      } catch (Exception e) {
        errorString = "Failed to execute task " + taskInfo.getTaskDetails() + ", hit error " +
            e.getMessage() + ".";
        LOG.error(errorString, e);
      } finally {
        if (errorString != null) {
          hasErrored = true;
          ObjectNode details = taskInfo.getTaskDetails().deepCopy();
          details.put("errorString", errorString);
          taskInfo.setTaskDetails(details);
          taskInfo.save();
        }
      }
    }
    return !hasErrored;
  }
}
