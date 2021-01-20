// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import io.ebean.*;
import io.ebean.annotation.*;
import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.forms.BackupTableParams;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.UUID;
import java.util.stream.Collectors;

import static java.lang.Math.abs;

@Entity
public class Backup extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(Backup.class);
  SimpleDateFormat tsFormat = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss");

  public enum BackupState {
    @EnumValue("In Progress")
    InProgress,

    @EnumValue("Completed")
    Completed,

    @EnumValue("Failed")
    Failed,

    @EnumValue("Deleted")
    Deleted,

    @EnumValue("Skipped")
    Skipped
  }

  @Id
  public UUID backupUUID;

  @Column(nullable = false)
  public UUID customerUUID;

  @Column(nullable = false)
  public BackupState state;

  @Column(columnDefinition = "TEXT", nullable = false)
  @DbJson
  public JsonNode backupInfo;

  @Column(unique = true)
  public UUID taskUUID;

  @Column
  private UUID scheduleUUID;
  public UUID getScheduleUUID() { return scheduleUUID; }

  @Column
  // Unix timestamp at which backup will get deleted.
  private Date expiry;
  public Date getExpiry() { return expiry; }

  public void setBackupInfo(BackupTableParams params) {
    this.backupInfo = Json.toJson(params);
  }

  public BackupTableParams getBackupInfo() {
    return Json.fromJson(this.backupInfo, BackupTableParams.class);
  }

  @CreatedTimestamp
  private Date createTime;
  public Date getCreateTime() { return createTime; }

  @UpdatedTimestamp
  private Date updateTime;
  public Date getUpdateTime() { return updateTime; }

  public static final Finder<UUID, Backup> find = new Finder<UUID, Backup>(Backup.class){};

  // For creating new backup we would set the storage location based on
  // universe UUID and backup UUID.
  // univ-<univ_uuid>/backup-<timestamp>-<something_to_disambiguate_from_yugaware>/table-keyspace.table_name.table_uuid
  private void updateStorageLocation(BackupTableParams params) {
    CustomerConfig customerConfig = CustomerConfig.get(customerUUID, params.storageConfigUUID);
    if (params.tableUUIDList != null) {
      params.storageLocation = String.format("univ-%s/backup-%s-%d/multi-table-%s",
        params.universeUUID, tsFormat.format(new Date()), abs(backupUUID.hashCode()),
        params.keyspace);
    } else if (params.tableName == null && params.keyspace != null) {
      params.storageLocation = String.format("univ-%s/backup-%s-%d/keyspace-%s",
        params.universeUUID, tsFormat.format(new Date()), abs(backupUUID.hashCode()),
        params.keyspace);
    } else {
      params.storageLocation = String.format("univ-%s/backup-%s-%d/table-%s.%s",
        params.universeUUID, tsFormat.format(new Date()), abs(backupUUID.hashCode()),
        params.keyspace, params.tableName);
      if (params.tableUUID != null) {
        params.storageLocation = String.format("%s-%s",
          params.storageLocation,
          params.tableUUID.toString().replace("-", "")
        );
      }
    }

    if (customerConfig != null) {
      // TODO: These values, S3 vs NFS / S3_BUCKET vs NFS_PATH come from UI right now...
      JsonNode storageNode = customerConfig.getData().get("BACKUP_LOCATION");
      if (storageNode != null) {
        String storagePath = storageNode.asText();
        if (storagePath != null && !storagePath.isEmpty()) {
          params.storageLocation = String.format("%s/%s", storagePath, params.storageLocation);
        }
      }
    }
  }

  public static Backup create(UUID customerUUID, BackupTableParams params) {
    Backup backup = new Backup();
    backup.backupUUID = UUID.randomUUID();
    backup.customerUUID = customerUUID;
    backup.state = BackupState.InProgress;
    if (params.scheduleUUID != null) {
      backup.scheduleUUID = params.scheduleUUID;
    }
    if (params.timeBeforeDelete != 0L) {
      backup.expiry = new Date(System.currentTimeMillis() + params.timeBeforeDelete);
    }
    if (params.backupList != null) {
      // In event of universe backup
      for (BackupTableParams childBackup : params.backupList) {
        if (childBackup.storageLocation == null) {
          backup.updateStorageLocation(childBackup);
        }
      }
    } else if (params.storageLocation == null) {
      // We would derive the storage location based on the parameters
      backup.updateStorageLocation(params);
    }
    backup.setBackupInfo(params);
    backup.save();
    return backup;
  }

  // We need to set the taskUUID right after commissioner task is submitted.

  /**
   * @param taskUUID to set if none set previously
   * @return true if the call ends up setting task uuid.
   */
  public synchronized boolean setTaskUUID(UUID taskUUID) {
    if (this.taskUUID == null) {
      this.taskUUID = taskUUID;
      save();
      return true;
    }
    return false;
  }

  public static List<Backup> fetchByUniverseUUID(UUID customerUUID, UUID universeUUID) {
      List<Backup> backupList = find.query().where()
        .eq("customer_uuid", customerUUID)
        .ne("state", BackupState.Deleted)
        .orderBy("create_time desc")
        .findList();
      return backupList.stream()
          .filter(backup -> backup.getBackupInfo().universeUUID.equals(universeUUID))
          .collect(Collectors.toList());
  }

  public static Backup get(UUID customerUUID, UUID backupUUID) {
    return find.query().where()
      .idEq(backupUUID)
      .eq("customer_uuid", customerUUID)
      .findOne();
  }

  public static Backup fetchByTaskUUID(UUID taskUUID) {
    return Backup.find.query().where()
      .eq("task_uuid", taskUUID)
      .findOne();
  }

  public static List<Backup> getExpiredBackups(UUID scheduleUUID) {
    // Get current timestamp.
    Date now = new Date();
    return Backup.find.query().where()
      .eq("schedule_uuid", scheduleUUID)
      .lt("expiry", now)
      .eq("state", BackupState.Completed)
      .findList();
  }

  public void transitionState(BackupState newState) {
    // We only allow state transition from InProgress to a valid state
    // Or completed to deleted state.
    if ((this.state == BackupState.InProgress && this.state != newState) ||
        (this.state == BackupState.Completed && newState == BackupState.Deleted) ||
        // This condition is for the case in which the delete fails and we need
        // to reset the state.
        (this.state == BackupState.Deleted && newState == BackupState.Completed)) {
      this.state = newState;
      save();
    }
  }

  public static boolean existsStorageConfig(UUID customerConfigUUID) {
    List<Backup> backupList = find.query().where()
        .or()
          .eq("state", BackupState.Completed)
          .eq("state", BackupState.InProgress)
        .endOr()
        .findList();
    backupList = backupList.stream()
        .filter(b -> b.getBackupInfo().storageConfigUUID.equals(customerConfigUUID))
        .collect(Collectors.toList());
    return backupList.size() != 0;
  }
}
