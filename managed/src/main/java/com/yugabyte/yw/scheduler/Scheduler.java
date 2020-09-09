/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.scheduler;

import akka.actor.ActorRef;
import akka.actor.ActorSystem;
import scala.concurrent.ExecutionContext;
import scala.concurrent.duration.Duration;

import java.time.ZonedDateTime;
import java.time.LocalDateTime;
import java.time.Instant;
import java.time.ZoneId;
import java.util.Date;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.text.SimpleDateFormat;

import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.MultiTableBackup;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.models.helpers.TaskType;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.Schedule;
import com.yugabyte.yw.models.ScheduleTask;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.forms.ITaskParams;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.cronutils.model.Cron;
import com.cronutils.model.definition.CronDefinitionBuilder;
import com.cronutils.model.time.ExecutionTime;
import com.cronutils.parser.CronParser;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.util.concurrent.ThreadFactoryBuilder;
import com.google.inject.Inject;
import com.google.inject.Singleton;

import play.api.Play;
import play.libs.Json;
import com.fasterxml.jackson.databind.JsonNode;

import static com.cronutils.model.CronType.UNIX;

@Singleton
public class Scheduler {

  public static final Logger LOG = LoggerFactory.getLogger(Scheduler.class);

  // Minimum number of scheduled threads.
  private static final int SCHEDULE_THREADS = 1;
  private final int YB_SCHEDULER_INTERVAL = 2;
  private final int MIN_TO_SEC = 60;

  private final ActorSystem actorSystem;
  private final ExecutionContext executionContext;

  private AtomicBoolean running = new AtomicBoolean(false);

  SimpleDateFormat tsFormat = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss");

  @Inject
  Commissioner commissioner;

  @Inject
  public Scheduler(ActorSystem actorSystem, ExecutionContext executionContext) {
    this.actorSystem = actorSystem;
    this.executionContext = executionContext;
    this.initialize();

    LOG.info("Starting scheduling service");
  }

  private void initialize() {
    this.actorSystem.scheduler().schedule(
        Duration.create(0, TimeUnit.MINUTES), // initialDelay
        Duration.create(YB_SCHEDULER_INTERVAL, TimeUnit.MINUTES), // interval
        () -> scheduleRunner(),
        this.executionContext
    );
  }

  /**
  * Iterates through all the schedule entries and runs the tasks that are due to be scheduled.
  */
  public void scheduleRunner() {
    // Check if last scheduled thread is still running.
    if (running.get()) {
       LOG.info("Previous scheduler still running");
       return;
    }

    LOG.info("Running scheduler");
    try {
      running.set(true);
      for (Schedule schedule : Schedule.getAllActive()) {
        Date currentTime = new Date();
        long frequency = schedule.getFrequency();
        String cronExpression = schedule.getCronExpression();
        if (cronExpression == null && frequency == 0) {
          LOG.error("Scheduled task does not have a recurrence specified {}",
                    schedule.getScheduleUUID());
          continue;
        }
        TaskType taskType = schedule.getTaskType();
        // TODO: Come back and maybe address if using relations between schedule and schedule_task is
        // a better approach.
        ScheduleTask lastTask = ScheduleTask.getLastTask(schedule.getScheduleUUID());
        Date lastScheduledTime = null;
        Date lastCompletedTime = null;
        if (lastTask != null) {
          lastScheduledTime = lastTask.getScheduledTime();
          lastCompletedTime = lastTask.getCompletedTime();
        }
        boolean runTask = false;
        long diff = 0;

        // Check if task needs to be scheduled again.
        if (lastScheduledTime != null && lastCompletedTime != null) {
          diff = Math.abs(currentTime.getTime() - lastScheduledTime.getTime());
        } else if (lastScheduledTime == null) {
          diff = Long.MAX_VALUE;
        }
        // If frequency if specified, check if the task needs to be scheduled.
        // The check sees the difference between the last scheduled task and the current
        // time. If the diff is greater than the frequency, means we need to run the task
        // again.
        if (frequency != 0L && diff > frequency) {
          runTask = true;
        }
        // In the case frequency is not defined and we have a cron expression, we compute
        // solely in accordance to the cron execution time. If the execution time is within the
        // scheduler interval, we run the task.
        else if (cronExpression != null) {
          CronParser unixCronParser =
              new CronParser(CronDefinitionBuilder.instanceDefinitionFor(UNIX));
          Cron parsedUnixCronExpression = unixCronParser.parse(cronExpression);
          Instant now = Instant.now();
          // LocalDateTime now = LocalDateTime.ofInstant(Instant.now(), ZoneId.of("UTC"));
          ZonedDateTime utcNow = now.atZone(ZoneId.of("UTC"));
          ExecutionTime executionTime = ExecutionTime.forCron(parsedUnixCronExpression);
          long timeFromLastExecution = executionTime.timeFromLastExecution(utcNow).get().getSeconds();
          if (timeFromLastExecution < YB_SCHEDULER_INTERVAL * MIN_TO_SEC) {
            // In case the last task was completed, or the last task was never even scheduled,
            // we run the task. If the task was scheduled, but didn't complete, we skip this
            // iteration completely.
            if (lastCompletedTime != null || lastScheduledTime == null) {
              runTask = true;
            } else if (lastScheduledTime != null) {
              LOG.warn("Previous scheduled task still running, skipping this iteration's task. " +
                       "Will try again next at {}.", executionTime.nextExecution(utcNow).get());
            }
          }
        }

        if (runTask) {
          if (taskType == TaskType.BackupUniverse) {
            this.runBackupTask(schedule);
          }
          if (taskType == TaskType.MultiTableBackup) {
            this.runMultiTableBackupsTask(schedule);
          }
        }
      }
    } catch (Exception e) {
      System.out.println(e);
      LOG.error("Error Running scheduler thread");
    } finally {
      running.set(false);
    }
  }

  public void runBackupTask(Schedule schedule) {
    UUID customerUUID = schedule.getCustomerUUID();
    Customer customer = Customer.get(customerUUID);
    JsonNode params = schedule.getTaskParams();
    BackupTableParams taskParams = Json.fromJson(params, BackupTableParams.class);
    Universe universe = null;
    try {
      universe = Universe.get(taskParams.universeUUID);
    } catch (Exception e) {
      schedule.stopSchedule();
      return;
    }
    if (universe.getUniverseDetails().updateInProgress ||
        universe.getUniverseDetails().backupInProgress) {
      LOG.warn("Cannot run Backup task since the universe {} is currently {}",
               taskParams.universeUUID.toString(), "in a locked state");
      return;
    }
    Backup backup = Backup.create(customerUUID, taskParams);
    UUID taskUUID = commissioner.submit(TaskType.BackupUniverse, taskParams);
    ScheduleTask.create(taskUUID, schedule.getScheduleUUID());
    LOG.info("Submitted task to backup table {}:{}, task uuid = {}.",
        taskParams.tableUUID, taskParams.tableName, taskUUID);
    backup.setTaskUUID(taskUUID);
    CustomerTask.create(customer,
        taskParams.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Backup,
        CustomerTask.TaskType.Create,
        taskParams.tableName);
    LOG.info("Saved task uuid {} in customer tasks table for table {}:{}.{}", taskUUID,
        taskParams.tableUUID, taskParams.keyspace, taskParams.tableName);
  }

  public void runMultiTableBackupsTask(Schedule schedule) {
    UUID customerUUID = schedule.getCustomerUUID();
    Customer customer = Customer.get(customerUUID);
    JsonNode params = schedule.getTaskParams();
    MultiTableBackup.Params taskParams = Json.fromJson(params,
        MultiTableBackup.Params.class);
    Universe universe = null;
    try {
      universe = Universe.get(taskParams.universeUUID);
    } catch (Exception e) {
      schedule.stopSchedule();
      return;
    }
    Map<String, String> config = universe.getConfig();
    if (universe.getUniverseDetails().updateInProgress || config.isEmpty() ||
        config.get(Universe.TAKE_BACKUPS).equals("false") ||
        universe.getUniverseDetails().backupInProgress) {
      LOG.warn("Cannot run MultiTableBackup task since the universe {} is currently {}",
               taskParams.universeUUID.toString(), "in a locked state");
      return;
    }
    UUID taskUUID = commissioner.submit(TaskType.MultiTableBackup, taskParams);
    ScheduleTask.create(taskUUID, schedule.getScheduleUUID());
    LOG.info("Submitted backup for universe: {}, task uuid = {}.",
        taskParams.universeUUID, taskUUID);
    CustomerTask.create(customer,
        customerUUID,
        taskUUID,
        CustomerTask.TargetType.Universe,
        CustomerTask.TaskType.Backup,
        universe.name
        );
    LOG.info("Saved task uuid {} in customer tasks table for universe {}:{}", taskUUID,
        taskParams.universeUUID, universe.name);
  }
}
