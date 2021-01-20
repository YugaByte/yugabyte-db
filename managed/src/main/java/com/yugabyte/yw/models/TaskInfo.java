// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.EnumType;
import javax.persistence.Enumerated;
import javax.persistence.Id;

import io.ebean.*;
import io.ebean.annotation.CreatedTimestamp;
import io.ebean.annotation.DbJson;
import io.ebean.annotation.EnumValue;
import io.ebean.annotation.UpdatedTimestamp;
import com.fasterxml.jackson.databind.JsonNode;

import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.models.helpers.TaskType;
import play.data.validation.Constraints;

import static com.yugabyte.yw.commissioner.UserTaskDetails.createSubTask;

@Entity
public class TaskInfo extends Model {

  /**
   * These are the various states of the task and taskgroup.
   */
  public enum State {
    @EnumValue("Created")
    Created,

    @EnumValue("Initializing")
    Initializing,

    @EnumValue("Running")
    Running,

    @EnumValue("Success")
    Success,

    @EnumValue("Failure")
    Failure,

    @EnumValue("Unknown")
    Unknown,
  }

  // The task UUID.
  @Id
  private UUID uuid;

  // The UUID of the parent task (if any; CustomerTasks have no parent)
  private UUID parentUuid;

  // The position within the parent task's taskQueue (-1 for a CustomerTask)
  @Column(columnDefinition = "integer default -1")
  private Integer position = -1;

  // The task type.
  @Column(nullable = false)
  @Enumerated(EnumType.STRING)
  private TaskType taskType;

  // The task state.
  @Column(nullable = false)
  @Enumerated(EnumType.STRING)
  private State taskState = State.Created;

  // The subtask group type (if it is a subtask)
  @Enumerated(EnumType.STRING)
  private UserTaskDetails.SubTaskGroupType subTaskGroupType;

  // The task creation time.
  @CreatedTimestamp
  private Date createTime;

  // The task update time. Time of the latest update (including heartbeat updates) on this task.
  @UpdatedTimestamp
  private Date updateTime;

  // The percentage completeness of the task, which is a number from 0 to 100.
  @Column(columnDefinition = "integer default 0")
  private Integer percentDone = 0;

  // Details of the task, usually a JSON representation of the incoming task. This is used to
  // describe the details of the task that is being executed.
  @Constraints.Required
  @Column(columnDefinition = "TEXT default '{}'", nullable = false)
  @DbJson
  private JsonNode details;

  // Identifier of the process owning the task.
  @Constraints.Required
  @Column(nullable = false)
  private String owner;

  public TaskInfo(TaskType taskType) {
    this.taskType = taskType;
  }

  public Date getCreationTime() {
    return createTime;
  }

  public Date getLastUpdateTime() {
    return updateTime;
  }

  public UUID getParentUUID() {
    return parentUuid;
  }

  public int getPercentDone() {
    return percentDone;
  }

  public int getPosition() {
    return position;
  }

  public UserTaskDetails.SubTaskGroupType getSubTaskGroupType() {
    return subTaskGroupType;
  }

  public JsonNode getTaskDetails() {
    return details;
  }

  public State getTaskState() {
    return taskState;
  }

  boolean hasCompleted() {
    return taskState == State.Success || taskState == State.Failure;
  }

  public TaskType getTaskType() {
    return taskType;
  }

  public UUID getTaskUUID() {
    return uuid;
  }

  public void setTaskUUID(UUID taskUUID) {
    uuid = taskUUID;
  }

  public void setOwner(String owner) {
    this.owner = owner;
  }

  public void setParentUuid(UUID parentUuid) {
    this.parentUuid = parentUuid;
  }

  public void setPercentDone(int percentDone) {
    this.percentDone = percentDone;
  }

  public void setPosition(int position) {
    this.position = position;
  }

  public void setSubTaskGroupType(UserTaskDetails.SubTaskGroupType subTaskGroupType) {
    this.subTaskGroupType = subTaskGroupType;
  }

  public void setTaskState(State taskState) {
    this.taskState = taskState;
  }

  public void setTaskDetails(JsonNode details) {
    this.details = details;
  }

  public static final Finder<UUID, TaskInfo> find = new Finder<UUID, TaskInfo>(TaskInfo.class){};

  public static TaskInfo get(UUID taskUUID) {
    // Return the instance details object.
    return find.byId(taskUUID);
  }

  public List<TaskInfo> getSubTasks() {
    Query<TaskInfo> subTaskQuery = TaskInfo.find.query().where()
        .eq("parent_uuid", getTaskUUID())
        .orderBy("position asc");
    return subTaskQuery.findList();
  }

  public List<TaskInfo> getIncompleteSubTasks() {
    Object[] incompleteStates = {State.Created, State.Initializing, State.Running};
    return TaskInfo.find.query().where()
      .eq("parent_uuid", getTaskUUID())
      .in("task_state", incompleteStates)
      .findList();
  }

  @Override
  public String toString() {
    StringBuilder sb = new StringBuilder();
    sb.append("taskType : ").append(taskType);
    sb.append(", ");
    sb.append("taskState: ").append(taskState);
    return sb.toString();
  }

  /**
   * Retrieve the UserTaskDetails for the task mapped to this TaskInfo object. Should only be called
   * on the user-level parent task, since only that task will have subtasks. Nothing will break if
   * called on a SubTask, it just won't give you much useful information.
   *
   * @return UserTaskDetails object for this TaskInfo, including info on the state on each of the
   * subTaskGroups.
   */
  public UserTaskDetails getUserTaskDetails() {
    UserTaskDetails taskDetails = new UserTaskDetails();
    List<TaskInfo> result = getSubTasks();
    Map<SubTaskGroupType, SubTaskDetails> userTasksMap = new HashMap<>();
    boolean customerTaskFailure = taskState.equals(State.Failure);
    for (TaskInfo taskInfo : result) {
      SubTaskGroupType subTaskGroupType = taskInfo.getSubTaskGroupType();
      if (subTaskGroupType == SubTaskGroupType.Invalid) {
        continue;
      }
      SubTaskDetails subTask = userTasksMap.get(subTaskGroupType);
      if (subTask == null) {
        subTask = createSubTask(subTaskGroupType);
        taskDetails.add(subTask);
      } else if (subTask.getState().equals(State.Failure.name()) ||
          subTask.getState().equals(State.Running.name())) {
        continue;
      }
      switch (taskInfo.getTaskState()) {
        case Failure:
          subTask.setState(State.Failure);
          break;
        case Running:
          subTask.setState(State.Running);
          break;
        case Created:
          subTask.setState(customerTaskFailure ? State.Unknown : State.Created);
          break;
        default:
          break;
      }
      userTasksMap.put(subTaskGroupType, subTask);
    }
    return taskDetails;
  }

  /**
   * Returns the aggregate percentage completion across all the subtasks.
   *
   * @return a number between 0.0 and 100.0.
   */
  public double getPercentCompleted() {
    Query<TaskInfo> subTaskQuery = TaskInfo.find.query().where()
        .eq("parent_uuid", getTaskUUID())
        .orderBy("position asc");
    List<TaskInfo> result = subTaskQuery.findList();
    if (result == null || result.size() == 0) {
      return 0.0;
    }
    int numSubtasksCompleted = 0;
    for (TaskInfo taskInfo : result) {
      if (taskInfo.getTaskState().equals(TaskInfo.State.Success)) {
        ++numSubtasksCompleted;
      }
    }
    return numSubtasksCompleted * 100.0 / result.size();
  }
}
