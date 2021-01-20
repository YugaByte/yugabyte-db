// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.fasterxml.jackson.annotation.JsonFormat;
import com.google.common.annotations.VisibleForTesting;
import com.google.common.base.Preconditions;
import io.ebean.Finder;
import io.ebean.Model;
import io.ebean.annotation.EnumValue;
import io.ebean.annotation.Transactional;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.validation.Constraints;

import javax.persistence.*;
import java.lang.reflect.Field;
import java.time.Duration;
import java.time.Instant;
import java.time.temporal.TemporalUnit;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.UUID;
import java.util.stream.Collectors;

@Entity
public class CustomerTask extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(CustomerTask.class);

  public enum TargetType {
    @EnumValue("Universe")
    Universe,

    @EnumValue("Cluster")
    Cluster,

    @EnumValue("Table")
    Table,

    @EnumValue("Provider")
    Provider,

    @EnumValue("Node")
    Node,

    @EnumValue("Backup")
    Backup,

    @EnumValue("KMS Configuration")
    KMSConfiguration,
  }

  public enum TaskType {
    @EnumValue("Create")
    Create,

    @EnumValue("Update")
    Update,

    @EnumValue("Delete")
    Delete,

    @EnumValue("Stop")
    Stop,

    @EnumValue("Start")
    Start,

    @EnumValue("Restart")
    Restart,

    @EnumValue("Remove")
    Remove,

    @EnumValue("Add")
    Add,

    @EnumValue("Release")
    Release,

    @EnumValue("UpgradeSoftware")
    UpgradeSoftware,

    @EnumValue("UpdateCert")
    UpdateCert,

    @EnumValue("UpdateDiskSize")
    UpdateDiskSize,

    @EnumValue("UpgradeGflags")
    UpgradeGflags,

    @EnumValue("BulkImportData")
    BulkImportData,

    @Deprecated
    @EnumValue("Backup")
    Backup,

    @EnumValue("Restore")
    Restore,

    @Deprecated
    @EnumValue("SetEncryptionKey")
    SetEncryptionKey,

    @EnumValue("EnableEncryptionAtRest")
    EnableEncryptionAtRest,

    @EnumValue("RotateEncryptionKey")
    RotateEncryptionKey,

    @EnumValue("DisableEncryptionAtRest")
    DisableEncryptionAtRest,

    @EnumValue("StartMaster")
    StartMaster;

    public String toString(boolean completed) {
      switch (this) {
        case Create:
          return completed ? "Created " : "Creating ";
        case Update:
          return completed ? "Updated " : "Updating ";
        case Delete:
          return completed ? "Deleted " : "Deleting ";
        case UpgradeSoftware:
          return completed ? "Upgraded Software " : "Upgrading Software ";
        case UpdateCert:
          return completed ? "Updated Cert " : "Updating Cert ";
        case UpgradeGflags:
          return completed ? "Upgraded GFlags " : "Upgrading GFlags ";
        case BulkImportData:
          return completed ? "Bulk imported data" : "Bulk importing data";
        case Restore:
          return completed ? "Restored " : "Restoring ";
        case Restart:
          return completed ? "Restarted " : "Restarting ";
        case Backup:
          return completed ? "Backed up " : "Backing up ";
        case SetEncryptionKey:
          return completed ? "Set encryption key" : "Setting encryption key";
        case EnableEncryptionAtRest:
          return completed ? "Enabled encryption at rest" : "Enabling encryption at rest";
        case RotateEncryptionKey:
          return completed ? "Rotated encryption at rest universe key" :
            "Rotating encryption at rest universe key";
        case DisableEncryptionAtRest:
          return completed ? "Disabled encryption at rest" : "Disabling encryption at rest";
        case StartMaster:
          return completed ? "Started Master process on " : "Starting Master process on ";
        default:
          return null;
      }
    }

    public static List<TaskType> filteredValues() {
      return Arrays.stream(TaskType.values()).filter(value -> {
        try {
          Field field = TaskType.class.getField(value.name());
          return !field.isAnnotationPresent(Deprecated.class);
        } catch (Exception e) {
          return false;
        }
      }).collect(Collectors.toList());
    }

    public String getFriendlyName() {
      switch (this) {
        case StartMaster:
          return "Start Master Process on";
        default:
          return name();
      }
    }
  }

  // Use IDENTITY strategy because `customer_task.id` is a `bigserial` type; not a sequence.
  @Id
  @GeneratedValue(strategy = GenerationType.IDENTITY)
  private Long id;

  public Long getId() {
    return id;
  }

  @Constraints.Required
  @Column(nullable = false)
  private UUID customerUUID;

  public UUID getCustomerUUID() {
    return customerUUID;
  }

  @Constraints.Required
  @Column(nullable = false)
  private UUID taskUUID;

  public UUID getTaskUUID() {
    return taskUUID;
  }

  @Constraints.Required
  @Column(nullable = false)
  private TargetType targetType;

  public TargetType getTarget() {
    return targetType;
  }

  @Constraints.Required
  @Column(nullable = false)
  private String targetName;

  public String getTargetName() {
    return targetName;
  }

  @Constraints.Required
  @Column(nullable = false)
  private TaskType type;

  public TaskType getType() {
    return type;
  }

  @Constraints.Required
  @Column(nullable = false)
  private UUID targetUUID;

  public UUID getTargetUUID() {
    return targetUUID;
  }

  @Constraints.Required
  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date createTime;

  public Date getCreateTime() {
    return createTime;
  }

  @Column
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date completionTime;

  public Date getCompletionTime() {
    return completionTime;
  }

  public void markAsCompleted() {
    markAsCompleted(new Date());
  }

  @VisibleForTesting
  void markAsCompleted(Date completionTime) {
    if (this.completionTime == null) {
      this.completionTime = completionTime;
      this.save();
    }
  }

  public static final Finder<Long, CustomerTask> find =
    new Finder<Long, CustomerTask>(CustomerTask.class) {
    };

  public static CustomerTask create(Customer customer, UUID targetUUID, UUID taskUUID,
                                    TargetType targetType, TaskType type, String targetName) {
    CustomerTask th = new CustomerTask();
    th.customerUUID = customer.uuid;
    th.targetUUID = targetUUID;
    th.taskUUID = taskUUID;
    th.targetType = targetType;
    th.type = type;
    th.targetName = targetName;
    th.createTime = new Date();
    th.save();
    return th;
  }

  public static CustomerTask get(Long id) {
    return CustomerTask.find.query().where()
      .idEq(id).findOne();
  }

  public static CustomerTask get(UUID customerUUID, UUID taskUUID) {
    return CustomerTask.find.query().where()
    .eq("customer_uuid", customerUUID)
    .eq("task_uuid", taskUUID)
    .findOne();
  }

  public String getFriendlyDescription() {
    StringBuilder sb = new StringBuilder();
    sb.append(type.toString(completionTime != null));
    sb.append(targetType.name());
    sb.append(" : ").append(targetName);
    return sb.toString();
  }

  /**
   * deletes customer_task, task_info and all its subtasks of a given task.
   * Assumes task_info tree is one level deep. If this assumption changes then
   * this code needs to be reworked to recurse.
   * When successful; it deletes at least 2 rows because there is always
   * customer_task and associated task_info row that get deleted.
   *
   * @return number of rows deleted.
   * ==0 - if deletion was skipped due to data integrity issues.
   * >=2 - number of rows deleted
   */
  @Transactional
  public int cascadeDeleteCompleted() {
    Preconditions.checkNotNull(completionTime,
      String.format("CustomerTask %s has not completed", id));
    TaskInfo rootTaskInfo = TaskInfo.get(taskUUID);
    if (!rootTaskInfo.hasCompleted()) {
      LOG.warn("Completed CustomerTask(id:{}, type:{}) has incomplete task_info {}",
        id, type, rootTaskInfo);
      return 0;
    }
    List<TaskInfo> subTasks = rootTaskInfo.getSubTasks();
    List<TaskInfo> incompleteSubTasks = subTasks.stream()
      .filter(taskInfo -> !taskInfo.hasCompleted())
      .collect(Collectors.toList());
    if (rootTaskInfo.getTaskState() == TaskInfo.State.Success && !incompleteSubTasks.isEmpty()) {
      LOG.warn(
        "For a customer_task.id: {}, Successful task_info.uuid ({}) has {} incomplete subtasks {}",
        id, rootTaskInfo.getTaskUUID(), incompleteSubTasks.size(), incompleteSubTasks);
      return 0;
    }
    // Note: delete leaf nodes first to preserve referential integrity.
    subTasks.forEach(Model::delete);
    rootTaskInfo.delete();
    this.delete();
    return 2 + subTasks.size();
  }

  public static CustomerTask findByTaskUUID(UUID taskUUID) {
    return find.query().where().eq("task_uuid", taskUUID).findOne();
  }

  public static List<CustomerTask> findOlderThan(Customer customer, Duration duration) {
    Date cutoffDate = new Date(Instant.now().minus(duration).toEpochMilli());
    return find.query().where()
      .eq("customerUUID", customer.uuid)
      .le("completion_time", cutoffDate)
      .findList();
  }

  public static List<CustomerTask> findIncompleteByTargetUUID(UUID targetUUID) {
    return find.query().where()
      .eq("target_uuid", targetUUID)
      .isNull("completion_time")
      .findList();
  }

  public static CustomerTask getLatestByUniverseUuid(UUID universeUUID) {
    List<CustomerTask> tasks = find.query().where()
      .eq("target_uuid", universeUUID)
      .isNotNull("completion_time")
      .orderBy("completion_time desc")
      .setMaxRows(1)
      .findList();
    if (tasks.size() > 0) {
      return tasks.get(0);
    } else {
      return null;
    }
  }

  public String getNotificationTargetName() {
    if (getType().equals(TaskType.Create) && getTarget().equals(TargetType.Backup)) {
      return Universe.get(getTargetUUID()).name;
    } else {
      return getTargetName();
    }
  }
}
