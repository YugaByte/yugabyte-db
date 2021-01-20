// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import com.fasterxml.jackson.annotation.JsonIgnore;
import org.yb.Common.TableType;
import play.data.validation.Constraints;

import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;

public class BackupTableParams extends TableManagerParams {
  public enum ActionType {
    CREATE,
    RESTORE,
    RESTORE_KEYS,
    DELETE
  }

  @Constraints.Required
  public UUID storageConfigUUID;

  public UUID kmsConfigUUID = null;

  // Specifies the backup storage location in case of S3 it would have
  // the S3 url based on universeUUID and timestamp.
  public String storageLocation;

  @Constraints.Required
  public ActionType actionType;

  public TableType backupType;

  public List<String> tableNameList;

  public List<UUID> tableUUIDList;

  // Allows bundling multiple backup params. Used only in the case
  // of backing up an entire universe transactionally
  public List<BackupTableParams> backupList;

  // Specifies the frequency for running the backup in milliseconds.
  public long schedulingFrequency = 0L;

  // Specifies the cron expression in case a recurring backup is expected.
  public String cronExpression = null;

  // Specifies the time in millisecs before deleting the backup from the storage
  // bucket.
  public long timeBeforeDelete = 0L;

  // Should backup script enable verbose logging.
  public boolean enableVerboseLogs = false;

  // Should the backup be transactional across tables
  public boolean transactionalBackup = false;

  // The number of concurrent commands to run on nodes over SSH
  public int parallelism = 8;

  // The associated schedule UUID (if applicable)
  public UUID scheduleUUID = null;

  @JsonIgnore
  public Set<String> getTableNames() {
    Set<String> tableNames = new HashSet<>();
    if (tableUUIDList != null && !tableUUIDList.isEmpty()) {
      tableNames.addAll(tableNameList);
    } else if (tableName != null) {
      tableNames.add(tableName);
    }

    return tableNames;
  }
}
