// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import java.util.List;
import java.util.UUID;
import java.util.Arrays;
import java.util.Map;
import java.util.HashMap;

import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.MultiTableBackup;
import com.yugabyte.yw.commissioner.tasks.subtasks.DeleteTableFromUniverse;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.BulkImportParams;
import com.yugabyte.yw.forms.TableDefinitionTaskParams;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.models.helpers.ColumnDetails;
import com.yugabyte.yw.models.helpers.TableDetails;
import com.yugabyte.yw.models.helpers.TaskType;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.GetTableSchemaResponse;
import org.yb.client.ListTablesResponse;
import org.yb.client.YBClient;
import org.yb.master.Master.ListTablesResponsePB.TableInfo;
import org.yb.master.Master.RelationType;
import org.yb.Common.TableType;

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.services.YBClientService;

import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;
import play.mvc.Results;

import static com.yugabyte.yw.commissioner.Common.CloudType.aws;
import static com.yugabyte.yw.common.Util.getUUIDRepresentation;
import static com.yugabyte.yw.forms.TableDefinitionTaskParams.createFromResponse;

public class TablesController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(TablesController.class);

  @Inject
  FormFactory formFactory;

  @Inject
  Commissioner commissioner;

  // The YB client to use.
  public YBClientService ybService;

  @Inject
  public TablesController(YBClientService service) { this.ybService = service; }

  public Result create(UUID customerUUID, UUID universeUUID) {

    try {

      // Validate customer UUID and universe UUID
      Customer customer = Customer.get(customerUUID);
      if (customer == null) {
        String errMsg = "Invalid Customer UUID: " + customerUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      Universe universe = Universe.get(universeUUID);

      Form<TableDefinitionTaskParams> formData = formFactory.form(TableDefinitionTaskParams.class)
        .bindFromRequest();
      TableDefinitionTaskParams taskParams = formData.get();

      // Submit the task to create the table.
      TableDetails tableDetails = taskParams.tableDetails;
      UUID taskUUID = commissioner.submit(TaskType.CreateCassandraTable, taskParams);
      LOG.info("Submitted create table for {}:{}, task uuid = {}.",
        taskParams.tableUUID, tableDetails.tableName, taskUUID);

      // Add this task uuid to the user universe.
      // TODO: check as to why we aren't populating the tableUUID from middleware
      // Which means all the log statements above and below are basically logging null?
      CustomerTask.create(customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Table,
        CustomerTask.TaskType.Create,
        tableDetails.tableName);
      LOG.info("Saved task uuid {} in customer tasks table for table {}:{}.{}", taskUUID,
          taskParams.tableUUID, tableDetails.keyspace, tableDetails.tableName);

      ObjectNode resultNode = Json.newObject();
      resultNode.put("taskUUID", taskUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return Results.status(OK, resultNode);
    } catch (NullPointerException e) {
      LOG.error("Error creating table", e);
      // This error isn't useful at all, why send a NullPointerException as api response?
      return ApiResponse.error(BAD_REQUEST, "NullPointerException");
    } catch (RuntimeException e) {
      LOG.error("Error creating table", e);
      return ApiResponse.error(BAD_REQUEST, e.getMessage());
    } catch (Exception e) {
      LOG.error("Error creating table", e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  public Result alter(UUID cUUID, UUID uniUUID, UUID tableUUID) {
    return play.mvc.Results.TODO;
  }

  public Result drop(UUID customerUUID, UUID universeUUID, UUID tableUUID) {
    // Validate customer UUID
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      String errMsg = "Invalid Customer UUID: " + customerUUID;
      LOG.error(errMsg);
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    // Validate universe UUID and retrieve master addresses
    Universe universe = Universe.get(universeUUID);
    if (universe == null) {
      String errMsg = "Invalid Universe UUID: " + universeUUID;
      LOG.error(errMsg);
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }
    final String masterAddresses = universe.getMasterAddresses(true);
    if (masterAddresses.isEmpty()) {
      String errMsg = "Expected error. Masters are not currently queryable.";
      LOG.warn(errMsg);
      return ApiResponse.success(errMsg);
    }
    String certificate = universe.getCertificate();
    YBClient client = null;
    try {
      client = ybService.getClient(masterAddresses, certificate);
      GetTableSchemaResponse schemaResponse = client.getTableSchemaByUUID(
          tableUUID.toString().replace("-", ""));
      ybService.closeClient(client, masterAddresses);
      if (schemaResponse == null) {
        String errMsg = "No table for UUID: " + tableUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }

      DeleteTableFromUniverse.Params taskParams = new DeleteTableFromUniverse.Params();
      taskParams.universeUUID = universeUUID;
      taskParams.expectedUniverseVersion = -1;
      taskParams.tableUUID = tableUUID;
      taskParams.tableName = schemaResponse.getTableName();
      taskParams.keyspace = schemaResponse.getNamespace();
      taskParams.masterAddresses = masterAddresses;

      UUID taskUUID = commissioner.submit(TaskType.DeleteTable, taskParams);
      LOG.info("Submitted delete table for {}:{}, task uuid = {}.", taskParams.tableUUID,
          taskParams.getFullName(), taskUUID);

      CustomerTask.create(customer,
          universeUUID,
          taskUUID,
          CustomerTask.TargetType.Table,
          CustomerTask.TaskType.Delete,
          taskParams.getFullName());
      LOG.info("Saved task uuid {} in customer tasks table for table {}:{}", taskUUID,
          taskParams.tableUUID, taskParams.getFullName());

      ObjectNode resultNode = Json.newObject();
      resultNode.put("taskUUID", taskUUID.toString());
      Audit.createAuditEntry(ctx(), request(), taskUUID);
      return ok(resultNode);
    } catch (Exception e) {
      LOG.error("Failed to get list of tables in universe " + universeUUID, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    } finally {
      ybService.closeClient(client, masterAddresses);
    }
  }

  public Result getColumnTypes() {
    ColumnDetails.YQLDataType[] dataTypes = ColumnDetails.YQLDataType.values();
    ObjectNode result = Json.newObject();
    ArrayNode primitives = Json.newArray();
    ArrayNode collections = Json.newArray();
    for (ColumnDetails.YQLDataType dataType : dataTypes) {
      if (dataType.isCollection()) {
        collections.add(dataType.name());
      } else {
        primitives.add(dataType.name());
      }
    }
    result.put("primitives", primitives);
    result.put("collections", collections);
    return ok(result);
  }

  public Result universeList(UUID customerUUID, UUID universeUUID) {

    // Validate customer UUID
    if (Customer.get(customerUUID) == null) {
      String errMsg = "Invalid Customer UUID: " + customerUUID;
      LOG.error(errMsg);
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    // Validate universe UUID and retrieve master addresses
    Universe universe = Universe.get(universeUUID);
    final String masterAddresses = universe.getMasterAddresses(true);
    if (masterAddresses.isEmpty()) {
      String errMsg = "Expected error. Masters are not currently queryable.";
      LOG.warn(errMsg);
      return ok(errMsg);
    }
    String certificate = universe.getCertificate();
    YBClient client = null;
    try {
      client = ybService.getClient(masterAddresses, certificate);
      ListTablesResponse response = client.getTablesList();
      List<TableInfo> tableInfoList = response.getTableInfoList();
      ArrayNode resultNode = Json.newArray();
      for (TableInfo table : tableInfoList) {
        String tableKeySpace = table.getNamespace().getName().toString();
        if (!tableKeySpace.toLowerCase().equals("system") &&
            !tableKeySpace.toLowerCase().equals("system_schema") &&
            !tableKeySpace.toLowerCase().equals("system_auth") &&
            !tableKeySpace.toLowerCase().equals("system_platform")) {
          ObjectNode node = Json.newObject();
          node.put("keySpace", tableKeySpace);
          node.put("tableType", table.getTableType().toString());
          node.put("tableName", table.getName());
          String tableUUID = table.getId().toStringUtf8();
          node.put("tableUUID", String.valueOf(getUUIDRepresentation(tableUUID)));
          node.put("isIndexTable", table.getRelationType() == RelationType.INDEX_TABLE_RELATION);
          resultNode.add(node);
        }
      }
      return ok(resultNode);
    } catch (Exception e) {
      LOG.error("Failed to get list of tables in universe " + universeUUID, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    } finally {
      ybService.closeClient(client, masterAddresses);
    }
  }

  /**
   * This API will describe a single table.
   *
   * @param customerUUID UUID of the customer owning the table.
   * @param universeUUID UUID of the universe in which the table resides.
   * @param tableUUID UUID of the table to describe.
   * @return json-serialized description of the table.
   */
  public Result describe(UUID customerUUID, UUID universeUUID, UUID tableUUID) {
    YBClient client = null;
    final String masterAddresses = Universe.get(universeUUID).getMasterAddresses(true);
    try {
      // Validate customer UUID and universe UUID
      if (Customer.get(customerUUID) == null) {
        String errMsg = "Invalid Customer UUID: " + customerUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      Universe universe = Universe.get(universeUUID);
      String certificate = universe.getCertificate();
      if (masterAddresses.isEmpty()) {
        LOG.warn("Expected error. Masters are not currently queryable.");
        return ok("Expected error. Masters are not currently queryable.");
      }
      client = ybService.getClient(masterAddresses, certificate);
      GetTableSchemaResponse response = client.getTableSchemaByUUID(
        tableUUID.toString().replace("-", ""));

      return ok(Json.toJson(createFromResponse(universe, tableUUID, response)));
    } catch (IllegalArgumentException e) {
      LOG.error("Failed to get schema of table " + tableUUID + " in universe " + universeUUID, e);
      return ApiResponse.error(BAD_REQUEST, e.getMessage());
    } catch (Exception e) {
      LOG.error("Failed to get schema of table " + tableUUID + " in universe " + universeUUID, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    } finally {
      ybService.closeClient(client, masterAddresses);
    }
  }

  public Result createMultiTableBackup(UUID customerUUID, UUID universeUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      String errMsg = "Invalid Customer UUID: " + customerUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    Universe universe = Universe.get(universeUUID);
    Form<MultiTableBackup.Params> formData = formFactory
        .form(MultiTableBackup.Params.class)
        .bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    MultiTableBackup.Params taskParams = formData.get();
    if (taskParams.storageConfigUUID == null) {
      String errMsg = "Missing StorageConfig UUID: " + taskParams.storageConfigUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }
    CustomerConfig storageConfig = CustomerConfig.get(customerUUID, taskParams.storageConfigUUID);
    if (storageConfig == null) {
      String errMsg = "Invalid StorageConfig UUID: " + taskParams.storageConfigUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    taskParams.universeUUID = universeUUID;
    taskParams.customerUUID = customerUUID;

    if (disableBackupOnTables(taskParams.tableUUIDList, universe)) {
      String errMsg = "Invalid Table List, found index or YSQL table.";
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }
    ObjectNode resultNode = Json.newObject();
    if (taskParams.schedulingFrequency != 0L || taskParams.cronExpression != null) {
      Schedule schedule = Schedule.create(customerUUID, taskParams,
          TaskType.MultiTableBackup, taskParams.schedulingFrequency,
          taskParams.cronExpression);
      UUID scheduleUUID = schedule.getScheduleUUID();
      LOG.info("Submitted universe backup to be scheduled {}, schedule uuid = {}.",
          universeUUID, scheduleUUID);
      resultNode.put("scheduleUUID", scheduleUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
    } else {
      UUID taskUUID = commissioner.submit(TaskType.MultiTableBackup, taskParams);
      LOG.info("Submitted task to universe {}, task uuid = {}.",
          universe.name, taskUUID);
      CustomerTask.create(customer,
          customerUUID,
          taskUUID,
          CustomerTask.TargetType.Universe,
          CustomerTask.TaskType.Backup,
          universe.name);
      LOG.info("Saved task uuid {} in customer tasks for universe {}", taskUUID,
          universe.name);
      resultNode.put("taskUUID", taskUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()), taskUUID);
    }
    return ApiResponse.success(resultNode);
  }

  public Result createBackup(UUID customerUUID, UUID universeUUID, UUID tableUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      String errMsg = "Invalid Customer UUID: " + customerUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }
    Universe universe = Universe.get(universeUUID);

    if (disableBackupOnTables(Arrays.asList(tableUUID), universe)) {
      String errMsg = "Invalid Table UUID: " + tableUUID + ". Cannot backup index or YSQL table.";
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    Form<BackupTableParams> formData = formFactory.form(BackupTableParams.class)
        .bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    BackupTableParams taskParams = formData.get();
    CustomerConfig storageConfig = CustomerConfig.get(customerUUID, taskParams.storageConfigUUID);
    if (storageConfig == null) {
      String errMsg = "Invalid StorageConfig UUID: " + taskParams.storageConfigUUID;
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    taskParams.universeUUID = universeUUID;
    taskParams.tableUUID = tableUUID;

    ObjectNode resultNode = Json.newObject();
    if (taskParams.schedulingFrequency != 0L || taskParams.cronExpression != null) {
      Schedule schedule = Schedule.create(customerUUID, taskParams,
          TaskType.BackupUniverse, taskParams.schedulingFrequency,
          taskParams.cronExpression);
      UUID scheduleUUID = schedule.getScheduleUUID();
      LOG.info("Submitted backup to be scheduled {}:{}, schedule uuid = {}.",
          tableUUID, taskParams.tableName, scheduleUUID);
      resultNode.put("scheduleUUID", scheduleUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
    } else {
      Backup backup = Backup.create(customerUUID, taskParams);
      UUID taskUUID = commissioner.submit(TaskType.BackupUniverse, taskParams);
      LOG.info("Submitted task to backup table {}:{}, task uuid = {}.",
          tableUUID, taskParams.tableName, taskUUID);
      backup.setTaskUUID(taskUUID);
      CustomerTask.create(customer,
          taskParams.universeUUID,
          taskUUID,
          CustomerTask.TargetType.Table,
          CustomerTask.TaskType.Backup,
          taskParams.tableName);
      LOG.info("Saved task uuid {} in customer tasks table for table {}:{}.{}", taskUUID,
          tableUUID, taskParams.keyspace, taskParams.tableName);
      resultNode.put("taskUUID", taskUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()), taskUUID);
    }
    return ApiResponse.success(resultNode);
  }

  /**
   * This API will allow a customer to bulk import data into a table.
   *
   * @param customerUUID UUID of the customer owning the table.
   * @param universeUUID UUID of the universe in which the table resides.
   * @param tableUUID UUID of the table to describe.
   */
  public Result bulkImport(UUID customerUUID, UUID universeUUID, UUID tableUUID) {
    try {

      // Validate customer UUID and universe UUID and AWS provider.
      Customer customer = Customer.get(customerUUID);
      if (customer == null) {
        String errMsg = "Invalid Customer UUID: " + customerUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      Universe universe = Universe.get(universeUUID);
      if (universe == null) {
        String errMsg = "Invalid Universe UUID: " + universeUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }

      if (disableBackupOnTables(Arrays.asList(tableUUID), universe)) {
        String errMsg = "Invalid Table UUID: " + tableUUID + ". Cannot backup index or YSQL table.";
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }

      // TODO: undo hardcode to AWS (required right now due to using EMR).
      Common.CloudType cloudType = universe.getUniverseDetails().getPrimaryCluster().userIntent.providerType;
      if (cloudType != aws) {
        String errMsg = "Bulk Import is currently only supported for AWS.";
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      Provider provider = Provider.get(customerUUID, cloudType);
      if (provider == null) {
        String errMsg = "Could not find Provider aws for customer UUID: " + customerUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }

      // Get form data and validate it.
      Form<BulkImportParams> formData = formFactory.form(BulkImportParams.class)
          .bindFromRequest();
      BulkImportParams taskParams = formData.get();
      if (taskParams.s3Bucket == null || !taskParams.s3Bucket.startsWith("s3://")) {
        String errMsg = "Invalid S3 Bucket provided: " + taskParams.s3Bucket;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      taskParams.universeUUID = universeUUID;

      UUID taskUUID = commissioner.submit(TaskType.ImportIntoTable, taskParams);
      LOG.info("Submitted import into table for {}:{}, task uuid = {}.",
          tableUUID, taskParams.tableName, taskUUID);

      CustomerTask.create(customer,
          universe.universeUUID,
          taskUUID,
          CustomerTask.TargetType.Table,
          CustomerTask.TaskType.BulkImportData,
          taskParams.tableName);
      LOG.info("Saved task uuid {} in customer tasks table for table {}:{}.{}", taskUUID,
          tableUUID, taskParams.keyspace, taskParams.tableName);

      ObjectNode resultNode = Json.newObject();
      resultNode.put("taskUUID", taskUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()), taskUUID);
      return ApiResponse.success(resultNode);
    } catch (Exception e) {
      String errMsg = "Failed to bulk import data into table " + tableUUID + " in universe "
          + universeUUID;
      LOG.error(errMsg, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  public boolean disableBackupOnTables(List<UUID> tableUuids, Universe universe) {
    if (tableUuids.isEmpty()) {
      return false;
    }

    final String masterAddresses = universe.getMasterAddresses(true);
    if (masterAddresses.isEmpty()) {
      String errMsg = "Masters are not currently queryable.";
      LOG.warn(errMsg);
      return false;
    }
    String certificate = universe.getCertificate();
    YBClient client = null;

    try {
      client = ybService.getClient(masterAddresses, certificate);
      ListTablesResponse response = client.getTablesList();
      List<TableInfo> tableInfoList = response.getTableInfoList();
      // Match if the table is an index or ysql table.
      return tableInfoList.stream().anyMatch(tableInfo ->
              tableUuids.contains(
                      getUUIDRepresentation(tableInfo.getId().toStringUtf8().replace("-", ""))) &&
                      ((tableInfo.hasRelationType() && tableInfo.getRelationType() ==
                              RelationType.INDEX_TABLE_RELATION) ||
                      (tableInfo.hasTableType() && tableInfo.getTableType() ==
                              TableType.PGSQL_TABLE_TYPE)));
    } catch (Exception e) {
      LOG.warn(e.toString());
      return false;
    } finally {
      ybService.closeClient(client, masterAddresses);
    }
  }
}
