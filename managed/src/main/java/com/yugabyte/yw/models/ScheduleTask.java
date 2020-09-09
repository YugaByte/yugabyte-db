// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import io.ebean.*;
import io.ebean.annotation.*;
import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.databind.JsonNode;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import javax.persistence.EnumType;
import javax.persistence.Enumerated;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.UUID;

@Entity
public class ScheduleTask extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(ScheduleTask.class);

  @Id
  public UUID taskUUID;
  public UUID getTaskUUID() { return taskUUID; }

  @Column(nullable = false)
  private UUID scheduleUUID;
  public UUID getScheduleUUID() { return scheduleUUID; }

  @Column
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date completedTime;
  public Date getCompletedTime() { return completedTime; }

  @Column
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date scheduledTime;
  public Date getScheduledTime() { return scheduledTime; }

  public static final Finder<UUID, ScheduleTask> find =
    new Finder<UUID, ScheduleTask>(ScheduleTask.class){};

  public static ScheduleTask create(UUID taskUUID, UUID scheduleUUID) {
    ScheduleTask task = new ScheduleTask();
    task.scheduleUUID = scheduleUUID;
    task.taskUUID = taskUUID;
    task.scheduledTime = new Date();
    task.save();
    return task;
  }

  public static ScheduleTask fetchByTaskUUID(UUID taskUUID) {
    return find.query().where().eq("task_uuid", taskUUID).findOne();
  }

  public static List<ScheduleTask> getAll() {
    return find.query().findList();
  }

  public static ScheduleTask getLastTask(UUID scheduleUUID) {
    List<ScheduleTask> tasks = find.query().where()
      .eq("schedule_uuid", scheduleUUID)
      .orderBy("scheduled_time desc")
      .findList();
    if (tasks.isEmpty()) {
      return null;
    }
    return tasks.get(0);
  }

  public void setCompletedTime() {
    this.completedTime = new Date();
    save();
  }
}
