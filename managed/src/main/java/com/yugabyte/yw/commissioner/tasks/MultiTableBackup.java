/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Universe;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.HashMap;

import static org.yb.Common.TableType;

import org.yb.client.GetTableSchemaResponse;
import org.yb.client.ListTablesResponse;
import org.yb.client.YBClient;
import org.yb.master.Master.ListTablesResponsePB.TableInfo;
import org.yb.master.Master.RelationType;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.api.Play;

import static com.yugabyte.yw.common.Util.getUUIDRepresentation;

public class MultiTableBackup extends UniverseTaskBase {

  public static final Logger LOG = LoggerFactory.getLogger(MultiTableBackup.class);

  public YBClientService ybService;

  public static class Params extends BackupTableParams {
    public UUID customerUUID;
    public List<UUID> tableUUIDList = new ArrayList<>();
  }

  public Params params() {
    return (Params)taskParams;
  }

  @Override
  public void initialize(ITaskParams params) {
    super.initialize(params);
    ybService = Play.current().injector().instanceOf(YBClientService.class);
  }

  @Override
  public void run() {
    try {
      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      // Update the universe DB with the update to be performed and set the 'updateInProgress' flag
      // to prevent other updates from happening.
      lockUniverse(-1 /* expectedUniverseVersion */);

      Universe universe = Universe.get(params().universeUUID);
      String masterAddresses = universe.getMasterAddresses(true);
      String certificate = universe.getCertificate();

      YBClient client = null;
      Set<UUID> tableSet = new HashSet<UUID>(params().tableUUIDList);
      BackupTableParams tableBackupParams = new BackupTableParams();
      List<BackupTableParams> backupParamsList = new ArrayList<>();
      try {
        client = ybService.getClient(masterAddresses, certificate);
        // If user specified the list of tables, only get info for those tables.
        if (tableSet.size() != 0) {
          for (UUID tableUUID : tableSet) {
            GetTableSchemaResponse tableSchema = client.getTableSchemaByUUID(
              tableUUID.toString().replace("-", ""));
            // If table is not REDIS or YCQL, ignore.
            if (tableSchema.getTableType() == TableType.PGSQL_TABLE_TYPE ||
              tableSchema.getTableType() == TableType.TRANSACTION_STATUS_TABLE_TYPE) {
              LOG.info("Skipping backup of table with UUID: " + tableUUID);
              continue;
            }
            if (params().transactionalBackup) {
              populateBackupParams(tableBackupParams,
                tableSchema.getNamespace(),
                tableSchema.getTableName(),
                tableUUID);
            } else {
              BackupTableParams backupParams = createBackupParams(tableSchema.getNamespace(),
                tableSchema.getTableName(),
                tableUUID);
              backupParamsList.add(backupParams);
            }
            LOG.info("Queuing backup for table {}:{}", tableSchema.getNamespace(),
              tableSchema.getTableName());
          }
        }
        // If user did not specify tables, that means we need to backup all tables.
        else {
          // AC: This API call does not work when retrieving YSQL tables in specified
          // namespace, so we have to filter explicitly below
          ListTablesResponse response = client.getTablesList(null, true, null);
          List<TableInfo> tableInfoList = response.getTableInfoList();
          HashMap<String, BackupTableParams> keyspaceMap = new HashMap<>();
          for (TableInfo table : tableInfoList) {
            TableType tableType = table.getTableType();
            String tableKeySpace = table.getNamespace().getName();
            String tableUUIDString = table.getId().toStringUtf8();
            UUID tableUUID = getUUIDRepresentation(tableUUIDString);
            // If table is not REDIS or YCQL, ignore.
            if (tableType == TableType.TRANSACTION_STATUS_TABLE_TYPE ||
              table.getRelationType() == RelationType.INDEX_TABLE_RELATION ||
              (params().keyspace != null && !params().keyspace.equals(tableKeySpace))) {
              LOG.info("Skipping keyspace/universe backup of table " + tableUUID +
                       ". Expected keyspace is " + params().keyspace +
                       "; actual keyspace is " + tableKeySpace);
              continue;
            }

            if (tableType == TableType.PGSQL_TABLE_TYPE && !keyspaceMap.containsKey(tableKeySpace)) {
              // YSQL keyspaces must have prefix in front
              if (params().keyspace != null) {
                populateBackupParams(tableBackupParams, "ysql." + tableKeySpace);
              } else {
                // Backing up entire universe
                BackupTableParams backupParams = createBackupParams("ysql." + tableKeySpace);
                backupParamsList.add(backupParams);
                keyspaceMap.put(tableKeySpace, backupParams);
              }
            } else if (tableType == TableType.YQL_TABLE_TYPE || tableType == TableType.REDIS_TABLE_TYPE) {
              if (params().transactionalBackup && params().keyspace != null) {
                populateBackupParams(tableBackupParams,
                  tableKeySpace,
                  table.getName(),
                  tableUUID);
              } else if (params().transactionalBackup && params().keyspace == null) {
                // Backing up universe as transaction
                if (keyspaceMap.containsKey(tableKeySpace)) {
                  BackupTableParams currentBackup = keyspaceMap.get(tableKeySpace);
                  populateBackupParams(currentBackup,
                    tableKeySpace,
                    table.getName(),
                    tableUUID);
                } else {
                  // Current keyspace not in list, add new BackupTableParams
                  BackupTableParams backupParams = createBackupParams(tableKeySpace,
                                                                      table.getName(),
                                                                      tableUUID);
                  backupParamsList.add(backupParams);
                  keyspaceMap.put(tableKeySpace, backupParams);
                }
              } else {
                BackupTableParams backupParams = createBackupParams(tableKeySpace,
                                                                    table.getName(),
                                                                    tableUUID);
                backupParamsList.add(backupParams);
              }
            } else {
              LOG.error("Unrecognized table type {} for {}:{}", tableType, tableKeySpace,
                table.getName());
            }

            LOG.info("Queuing backup for table {}:{}", tableKeySpace,
              table.getName());
          }
        }
        ybService.closeClient(client, masterAddresses);
      } catch (Exception e) {
        LOG.error("Failed to get list of tables in universe " + params().universeUUID, e);
        ybService.closeClient(client, masterAddresses);
        unlockUniverseForUpdate();
        throw new RuntimeException(e);
      }

      updateBackupState(true);

      subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

      LOG.info("Successfully started scheduled backup of tables.");
      if (params().transactionalBackup) {
        if (params().keyspace != null) {
          Backup backup = Backup.create(params().customerUUID, tableBackupParams);
          createEncryptedUniverseKeyBackupTask(backup.getBackupInfo()).setSubTaskGroupType(
            UserTaskDetails.SubTaskGroupType.CreatingTableBackup
          );
          createTableBackupTask(tableBackupParams, backup).setSubTaskGroupType(
            UserTaskDetails.SubTaskGroupType.CreatingTableBackup);
        } else {
          // Handles case of full universe backup
          tableBackupParams.backupList = backupParamsList;
          tableBackupParams.storageConfigUUID = params().storageConfigUUID;
          tableBackupParams.actionType = BackupTableParams.ActionType.CREATE;
          tableBackupParams.storageConfigUUID = params().storageConfigUUID;
          tableBackupParams.universeUUID = params().universeUUID;
          tableBackupParams.sse = params().sse;
          tableBackupParams.parallelism = params().parallelism;
          tableBackupParams.transactionalBackup = params().transactionalBackup;
          Backup backup = Backup.create(params().customerUUID, tableBackupParams);

          for (BackupTableParams backupParams : backupParamsList) {
            createEncryptedUniverseKeyBackupTask(backupParams).setSubTaskGroupType(
              UserTaskDetails.SubTaskGroupType.CreatingTableBackup
            );
          }

          createTableBackupTask(tableBackupParams, backup).setSubTaskGroupType(
            UserTaskDetails.SubTaskGroupType.CreatingTableBackup);
        }
      } else {
        for (BackupTableParams tableParams : backupParamsList) {
          Backup backup = Backup.create(params().customerUUID, tableParams);
          createEncryptedUniverseKeyBackupTask(tableParams).setSubTaskGroupType(
            UserTaskDetails.SubTaskGroupType.CreatingTableBackup
          );
          createTableBackupTask(tableParams, backup).setSubTaskGroupType(
            UserTaskDetails.SubTaskGroupType.CreatingTableBackup);
        }
      }

      // Marks the update of this universe as a success only if all the tasks before it succeeded.
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);

      unlockUniverseForUpdate();

      subTaskGroupQueue.run();

    } catch (Throwable t) {
      LOG.error("Error executing task {} with error='{}'.", getName(), t.getMessage(), t);
      // Run an unlock in case the task failed before getting to the unlock. It is okay if it
      // errors out.
      unlockUniverseForUpdate();
      throw t;
    } finally {
      updateBackupState(false);
    }
    LOG.info("Finished {} task.", getName());
  }

  // Helper method to update passed in reference object
  private void populateBackupParams(BackupTableParams backupParams,
                                    String tableKeySpace,
                                    String tableName,
                                    UUID tableUUID) {

    backupParams.actionType = BackupTableParams.ActionType.CREATE;
    backupParams.storageConfigUUID = params().storageConfigUUID;
    backupParams.universeUUID = params().universeUUID;
    backupParams.sse = params().sse;
    backupParams.parallelism = params().parallelism;
    backupParams.keyspace = tableKeySpace;
    backupParams.transactionalBackup = params().transactionalBackup;

    if (tableName != null && tableUUID != null) {
      if (backupParams.tableNameList == null) {
        backupParams.tableNameList = new ArrayList<String>();
        backupParams.tableUUIDList = new ArrayList<UUID>();
        if (backupParams.tableName != null && backupParams.tableUUID != null) {
          // Clear singular fields and add to lists
          backupParams.tableNameList.add(backupParams.tableName);
          backupParams.tableUUIDList.add(backupParams.tableUUID);
          backupParams.tableName = null;
          backupParams.tableUUID = null;
        }
      }
      backupParams.tableNameList.add(tableName);
      backupParams.tableUUIDList.add(tableUUID);
    }
  }

  private void populateBackupParams(BackupTableParams backupParams,
                                    String tableKeySpace) {
    populateBackupParams(backupParams, tableKeySpace, null, null);
  }

  private BackupTableParams createBackupParams(String tableKeySpace) {
    return createBackupParams(tableKeySpace, null, null);
  }

  private BackupTableParams createBackupParams(String tableKeySpace, String tableName,
                                                 UUID tableUUID) {
    BackupTableParams backupParams = new BackupTableParams();
    backupParams.keyspace = tableKeySpace;
    if (tableUUID != null && tableName != null) {
      backupParams.tableUUID = tableUUID;
      backupParams.tableName = tableName;
    }
    backupParams.actionType = BackupTableParams.ActionType.CREATE;
    backupParams.storageConfigUUID = params().storageConfigUUID;
    backupParams.universeUUID = params().universeUUID;
    backupParams.sse = params().sse;
    backupParams.parallelism = params().parallelism;
    return backupParams;
  }
}
