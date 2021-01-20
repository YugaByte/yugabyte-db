// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import java.util.Iterator;
import java.util.Map;
import java.util.Map.Entry;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;

import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.helpers.TaskType;
import com.yugabyte.yw.models.TaskInfo;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.util.concurrent.ThreadFactoryBuilder;
import com.google.inject.Singleton;

import play.libs.Json;

@Singleton
public class Commissioner {

  public static final Logger LOG = LoggerFactory.getLogger(Commissioner.class);

  // Minimum number of concurrent tasks to execute at a time.
  private static final int TASK_THREADS = 200;

  // The maximum time that excess idle threads will wait for new tasks before terminating.
  // The unit is specified in the API (and is seconds).
  private static final long THREAD_ALIVE_TIME = 60L;

  // The interval after which progress monitor wakes up and does work.
  private final long PROGRESS_MONITOR_SLEEP_INTERVAL = 300;

  // The background progress monitor for the tasks.
  static ProgressMonitor progressMonitor;

  // Threadpool to run user submitted tasks.
  static ExecutorService executor;

  // A map of all task UUID's to the task runner objects for all the user tasks that are currently
  // active. Recently completed tasks are also in this list, their completion percentage should be
  // persisted before removing the task from this map.
  static Map<UUID, TaskRunner> runningTasks = new ConcurrentHashMap<UUID, TaskRunner>();

  public Commissioner() {
    // Initialize the tasks threadpool.
    ThreadFactory namedThreadFactory =
        new ThreadFactoryBuilder().setNameFormat("TaskPool-%d").build();
    // Create an task pool which can handle an unbounded number of tasks, while using an initial set
    // of threads that get spawned upto TASK_THREADS limit.
    executor =
        new ThreadPoolExecutor(TASK_THREADS, TASK_THREADS, THREAD_ALIVE_TIME,
                               TimeUnit.SECONDS, new LinkedBlockingQueue<Runnable>(),
                               namedThreadFactory);
    LOG.info("Started Commissioner TaskPool.");

    // TODO: Conisder replacing simple thread sleep with ScheduledExecutorService
    // Initialize the task manager.
    progressMonitor = new ProgressMonitor();
    progressMonitor.start();
    LOG.info("Started TaskProgressMonitor thread.");
  }

  /**
   * Creates a new task runner to run the required task, and submits it to a threadpool if needed.
   */
  public UUID submit(TaskType taskType, ITaskParams taskParams) {
    try {
      // Claim the task if we can - check if we will go above the max local concurrent task
      // threshold. If we can claim it, set ourselves as the owner of the task. Otherwise, do not
      // claim the task so that some other process can claim it.
      // TODO: enforce a limit on number of tasks here.
      boolean claimTask = true;

      // Create the task runner object based on the various parameters passed in.
      TaskRunner taskRunner = TaskRunner.createTask(taskType, taskParams, claimTask);

      if (claimTask) {
        // Add this task to our queue.
        runningTasks.put(taskRunner.getTaskUUID(), taskRunner);

        // If we had claimed ownership of the task, submit it to the task threadpool.
        executor.submit(taskRunner);
      }
      return taskRunner.getTaskUUID();
    } catch (Throwable t) {
      String msg = "Error processing " + taskType + " task for " + taskParams.toString();
      LOG.error(msg, t);
      throw new RuntimeException(msg, t);
    }
  }

  public ObjectNode getStatus(UUID taskUUID) {
    ObjectNode responseJson = Json.newObject();

    // Check if the task is in the DB
    TaskInfo taskInfo = TaskInfo.get(taskUUID);
    CustomerTask task = CustomerTask.find.query().where().eq("task_uuid", taskUUID).findOne();
    if (taskInfo != null && task != null) {
      // Add some generic information about the task
      responseJson.put("title", task.getFriendlyDescription());
      responseJson.put("createTime", task.getCreateTime().toString());
      responseJson.put("target", task.getTargetName());
      responseJson.put("targetUUID", task.getTargetUUID().toString());
      responseJson.put("type", task.getType().name());
      // Find out the state of the task.
      responseJson.put("status", taskInfo.getTaskState().toString());
      // Get the percentage of subtasks that ran and completed
      responseJson.put("percent", taskInfo.getPercentCompleted());
      // Get subtask groups
      responseJson.set("details", Json.toJson(taskInfo.getUserTaskDetails()));
      return responseJson;
    }

    // We are not able to find the task. Report an error.
    LOG.error("Not able to find task " + taskUUID);
    throw new RuntimeException("Not able to find task " + taskUUID);
  }

  public JsonNode getTaskDetails(UUID taskUUID) {
    TaskInfo taskInfo = TaskInfo.get(taskUUID);
    if (taskInfo != null) {
      return taskInfo.getTaskDetails();
    } else {
      return null;
    }
  }

  /**
   * A progress monitor to constantly write a last updated timestamp in the DB so that this
   * process and all its subtasks are considered to be alive.
   */
  private class ProgressMonitor extends Thread {

    public ProgressMonitor() {
      setName("TaskProgressMonitor");
    }

    @Override
    public void run() {
      while (true) {
        // Loop through all the active tasks.
        Iterator<Entry<UUID, TaskRunner>> iter = runningTasks.entrySet().iterator();
        while (iter.hasNext()) {
          Entry<UUID, TaskRunner> entry = iter.next();
          TaskRunner taskRunner = entry.getValue();

          // If the task is still running, update its latest timestamp as a part of the heartbeat.
          if (taskRunner.isTaskRunning()) {
            taskRunner.doHeartbeat();
          } else if (taskRunner.hasTaskSucceeded()) {
            LOG.info("Task " + taskRunner.toString() + " has succeeded.");
            // Remove task from the set of live tasks.
            iter.remove();
          } else if (taskRunner.hasTaskFailed()) {
            LOG.info("Task " + taskRunner.toString() + " has failed.");
            // Remove task from the set of live tasks.
            iter.remove();
          }
        }

        // TODO: Scan the DB for tasks that have failed to make progress and claim one if possible.

        // Sleep for the required interval.
        try {
          Thread.sleep(PROGRESS_MONITOR_SLEEP_INTERVAL);
        } catch (InterruptedException e) {
        }
      }
    }
  }
}
