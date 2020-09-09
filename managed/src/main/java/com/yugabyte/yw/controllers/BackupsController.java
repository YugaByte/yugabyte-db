// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;

import java.util.List;
import java.util.UUID;

public class BackupsController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(BackupsController.class);

  @Inject
  FormFactory formFactory;

  @Inject
  Commissioner commissioner;

  public Result list(UUID customerUUID, UUID universeUUID) {
    List<Backup> backups = Backup.fetchByUniverseUUID(customerUUID, universeUUID);
    return ApiResponse.success(backups);
  }

  public Result restore(UUID customerUUID, UUID universeUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      String errMsg = "Invalid Customer UUID: " + customerUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }
    Universe universe;
    try {
      universe = Universe.get(universeUUID);
    } catch (RuntimeException re) {
      String errMsg = "Invalid Universe UUID: " + universeUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    Form<BackupTableParams> formData = formFactory.form(BackupTableParams.class)
        .bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    BackupTableParams taskParams = formData.get();
    // Since we hit the restore endpoint, lets default the action type to RESTORE
    taskParams.actionType = BackupTableParams.ActionType.RESTORE;
    if (taskParams.storageLocation == null && taskParams.backupList == null) {
      String errMsg = "Storage Location is required";
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    // Change the BackupTableParams in list to be "RESTORE" action type
    if (taskParams.backupList != null) {
      for (BackupTableParams subParams: taskParams.backupList) {
        // Override default CREATE action type that we inherited from backup flow
        subParams.actionType = BackupTableParams.ActionType.RESTORE;
        // Assume no renaming of keyspaces or tables
        subParams.tableUUIDList = null;
        subParams.tableNameList = null;
        subParams.tableUUID = null;
        subParams.tableName = null;
        subParams.keyspace = null;
        subParams.parallelism = taskParams.parallelism;;
      }
    }
    CustomerConfig storageConfig = CustomerConfig.get(customerUUID, taskParams.storageConfigUUID);
    if (storageConfig == null) {
      String errMsg = "Invalid StorageConfig UUID: " + taskParams.storageConfigUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }
    if (taskParams.tableName != null && taskParams.keyspace == null) {
      String errMsg = "Restore table request must specify keyspace.";
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    taskParams.universeUUID = universeUUID;

    Backup newBackup = Backup.create(customerUUID, taskParams);
    UUID taskUUID = commissioner.submit(TaskType.BackupUniverse, taskParams);
    LOG.info("Submitted task to restore table backup to {}.{}, task uuid = {}.",
        taskParams.keyspace, taskParams.tableName, taskUUID);
    newBackup.setTaskUUID(taskUUID);
    if (taskParams.tableName != null) {
      CustomerTask.create(customer,
        universeUUID,
        taskUUID,
        CustomerTask.TargetType.Backup,
        CustomerTask.TaskType.Restore,
        taskParams.tableName);
      LOG.info("Saved task uuid {} in customer tasks table for table {}.{}", taskUUID,
        taskParams.keyspace, taskParams.tableName);
    } else if (taskParams.keyspace != null) {
      CustomerTask.create(customer,
        universeUUID,
        taskUUID,
        CustomerTask.TargetType.Backup,
        CustomerTask.TaskType.Restore,
        taskParams.keyspace);
      LOG.info("Saved task uuid {} in customer tasks table for keyspace {}", taskUUID,
        taskParams.keyspace);
    } else {
      CustomerTask.create(customer,
        universeUUID,
        taskUUID,
        CustomerTask.TargetType.Backup,
        CustomerTask.TaskType.Restore,
        universe.name);
      if (taskParams.backupList != null) {
        LOG.info("Saved task uuid {} in customer tasks table for universe backup {}", taskUUID,
          universe.name);
      } else {
        LOG.info("Saved task uuid {} in customer tasks table for restore identical keyspace & tables in universe {}", taskUUID,
          universe.name);
      }
    }

    ObjectNode resultNode = Json.newObject();
    resultNode.put("taskUUID", taskUUID.toString());
    Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()), taskUUID);
    return ApiResponse.success(resultNode);
  }
}
