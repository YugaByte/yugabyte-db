// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import java.lang.reflect.Field;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.UUID;
import java.util.stream.Collectors;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.GeneratedValue;
import javax.persistence.GenerationType;
import javax.persistence.Id;
import javax.persistence.SequenceGenerator;

import io.ebean.*;
import io.ebean.annotation.*;
import com.fasterxml.jackson.annotation.JsonFormat;

import play.data.validation.Constraints;

@Entity
public class CustomerTask extends Model {
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
    KMSConfiguration;
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

    @EnumValue("Remove")
    Remove,

    @EnumValue("Add")
    Add,

    @EnumValue("Release")
    Release,

    @EnumValue("UpgradeSoftware")
    UpgradeSoftware,

    @EnumValue("UpdateDiskSize")
    UpdateDiskSize,

    @EnumValue("UpgradeGflags")
    UpgradeGflags,

    @EnumValue("BulkImportData")
    BulkImportData,

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
    DisableEncryptionAtRest;

    public String toString(boolean completed) {
      switch(this) {
        case Create:
          return completed ? "Created " : "Creating ";
        case Update:
          return completed ? "Updated " : "Updating ";
        case Delete:
          return completed ? "Deleted " : "Deleting ";
        case UpgradeSoftware:
          return completed ? "Upgraded Software " : "Upgrading Software ";
        case UpgradeGflags:
          return completed ? "Upgraded GFlags " : "Upgrading GFlags ";
        case BulkImportData:
          return completed ? "Bulk imported data" : "Bulk importing data";
        case Restore:
          return completed ? "Restored " : "Restoring ";
        case Backup:
          return completed ? "Backed up" : "Backing up";
        case SetEncryptionKey:
          return completed ? "Set encryption key" : "Setting encryption key";
        case EnableEncryptionAtRest:
          return completed ? "Enabled encryption at rest" : "Enabling encryption at rest";
        case RotateEncryptionKey:
          return completed ? "Rotated encryption at rest universe key" :
                  "Rotating encryption at rest universe key";
        case DisableEncryptionAtRest:
          return completed ? "Disabled encryption at rest" : "Disabling encryption at rest";
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
  }

  @Id
  @SequenceGenerator(
    name="customer_task_id_seq", sequenceName="customer_task_id_seq", allocationSize=1)
  @GeneratedValue(strategy = GenerationType.SEQUENCE, generator="customer_task_id_seq")
  private Long id;

  @Constraints.Required
  @Column(nullable = false)
  private UUID customerUUID;
  public UUID getCustomerUUID() { return customerUUID; }

  @Constraints.Required
  @Column(nullable = false)
  private UUID taskUUID;
  public UUID getTaskUUID() { return taskUUID; }

  @Constraints.Required
  @Column(nullable = false)
  private TargetType targetType;
  public TargetType getTarget() { return targetType; }

  @Constraints.Required
  @Column(nullable = false)
  private String targetName;
  public String getTargetName() { return targetName; }

  @Constraints.Required
  @Column(nullable = false)
  private TaskType type;
  public TaskType getType() { return type; }

  @Constraints.Required
  @Column(nullable = false)
  private UUID targetUUID;
  public UUID getTargetUUID() { return targetUUID; }

  @Constraints.Required
  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date createTime;
  public Date getCreateTime() { return createTime; }

  @Column
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date completionTime;
  public Date getCompletionTime() { return completionTime; }
  public void markAsCompleted() {
    if (completionTime == null) {
      completionTime = new Date();
      this.save();
    }
  }

  public static final Finder<Long, CustomerTask> find =
    new Finder<Long, CustomerTask>(CustomerTask.class){};

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

  public String getFriendlyDescription() {
    StringBuilder sb = new StringBuilder();
    sb.append(type.toString(completionTime != null));
    sb.append(targetType.name());
    sb.append(" : " + targetName);
    return sb.toString();
  }

  public static CustomerTask findByTaskUUID(UUID taskUUID) {
    return find.query().where().eq("task_uuid", taskUUID).findOne();
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
    if (tasks != null && tasks.size() > 0) {
      return tasks.get(0);
    } else {
      return null;
    }
  }
}
