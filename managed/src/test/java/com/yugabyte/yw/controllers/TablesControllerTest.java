// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.commissioner.Common.CloudType.aws;
import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertErrorNodeValue;
import static com.yugabyte.yw.common.AssertHelper.assertOk;
import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertForbidden;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.AssertHelper.assertAuditEntry;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.hamcrest.core.StringContains.containsString;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertTrue;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyBoolean;
import static org.mockito.Matchers.eq;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static org.mockito.Mockito.spy;
import static org.mockito.Mockito.doReturn;
import static play.inject.Bindings.bind;
import static play.mvc.Http.Status.BAD_REQUEST;
import static play.mvc.Http.Status.OK;
import static play.test.Helpers.contentAsString;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.Arrays;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.protobuf.ByteString;
import com.google.common.collect.ImmutableSet;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.subtasks.DeleteTableFromUniverse;
import com.yugabyte.yw.commissioner.tasks.MultiTableBackup;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.RegexMatcher;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.BulkImportParams;
import com.yugabyte.yw.forms.TableDefinitionTaskParams;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.Users;
import com.yugabyte.yw.models.helpers.ColumnDetails;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Test;
import org.mockito.ArgumentCaptor;
import org.mockito.Matchers;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.ColumnSchema;
import org.yb.Common.TableType;
import org.yb.Schema;
import org.yb.Type;
import org.yb.client.GetTableSchemaResponse;
import org.yb.client.ListTablesResponse;
import org.yb.client.YBClient;
import org.yb.master.Master;
import org.yb.master.Master.ListTablesResponsePB.TableInfo;
import org.yb.master.Master.RelationType;

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Schedule;

import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

import static play.test.Helpers.contextComponents;

public class TablesControllerTest extends FakeDBApplication {
  public static final Logger LOG = LoggerFactory.getLogger(TablesControllerTest.class);
  private YBClientService mockService;
  private TablesController tablesController;
  private YBClient mockClient;
  private ListTablesResponse mockListTablesResponse;
  private GetTableSchemaResponse mockSchemaResponse;

  private Schema getFakeSchema() {
    List<ColumnSchema> columnSchemas = new LinkedList<>();
    columnSchemas.add(new ColumnSchema.ColumnSchemaBuilder("mock_column", Type.INT32)
        .id(1)
        .hashKey(true)
        .build());
    return new Schema(columnSchemas);
  }

  @Before
  public void setUp() throws Exception {
    mockClient = mock(YBClient.class);
    mockService = mock(YBClientService.class);
    mockListTablesResponse = mock(ListTablesResponse.class);
    mockSchemaResponse = mock(GetTableSchemaResponse.class);
    when(mockService.getClient(any(), any())).thenReturn(mockClient);
    tablesController = new TablesController(mockService);
  }

  @Test
  public void testListTablesFromYbClient() throws Exception {
    List<TableInfo> tableInfoList = new ArrayList<TableInfo>();
    Set<String> tableNames = new HashSet<String>();
    tableNames.add("Table1");
    tableNames.add("Table2");
    TableInfo ti1 = TableInfo.newBuilder()
        .setName("Table1")
        .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("$$$Default"))
        .setId(ByteString.copyFromUtf8(UUID.randomUUID().toString()))
        .setTableType(TableType.REDIS_TABLE_TYPE)
        .build();
    TableInfo ti2 = TableInfo.newBuilder()
        .setName("Table2")
        .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("$$$Default"))
        .setId(ByteString.copyFromUtf8(UUID.randomUUID().toString()))
        .setTableType(TableType.YQL_TABLE_TYPE)
        .build();
    // Create System type table, this will not be returned in response
    TableInfo ti3 = TableInfo.newBuilder()
        .setName("Table3")
        .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("system"))
        .setId(ByteString.copyFromUtf8(UUID.randomUUID().toString()))
        .setTableType(TableType.YQL_TABLE_TYPE)
        .build();
    tableInfoList.add(ti1);
    tableInfoList.add(ti2);
    tableInfoList.add(ti3);
    when(mockListTablesResponse.getTableInfoList()).thenReturn(tableInfoList);
    when(mockClient.getTablesList()).thenReturn(mockListTablesResponse);
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe u1 = createUniverse(customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(u1.universeUUID);
    customer.save();

    LOG.info("Created customer " + customer.uuid + " with universe " + u1.universeUUID);
    Result r = tablesController.universeList(customer.uuid, u1.universeUUID);
    JsonNode json = Json.parse(contentAsString(r));
    LOG.info("Fetched table list from universe, response: " + contentAsString(r));
    assertEquals(OK, r.status());
    assertTrue(json.isArray());
    Iterator<JsonNode> it = json.elements();
    int numTables = 0;
    while (it.hasNext()) {
      JsonNode table = it.next();
      String tableName = table.get("tableName").asText();
      String tableType = table.get("tableType").asText();
      String tableKeySpace = table.get("keySpace") != null ? table.get("keySpace").asText() : null;
      // Display table only if table is redis type or table is CQL type but not of system keyspace
      if (tableType.equals("REDIS_TABLE_TYPE") ||
          (!tableKeySpace.toLowerCase().equals("system") &&
          !tableKeySpace.toLowerCase().equals("system_schema") &&
          !tableKeySpace.toLowerCase().equals("system_auth"))) {
        numTables++;
      }
      LOG.info("Table name: " + tableName + ", table type: " + tableType);
      assertTrue(tableNames.contains(tableName));
      if (tableName.equals("Table1")) {
        assertEquals(TableType.REDIS_TABLE_TYPE.toString(), tableType);
        assertEquals("$$$Default", tableKeySpace);
      } else if (tableName.equals("Table2")) {
        assertEquals(TableType.YQL_TABLE_TYPE.toString(), tableType);
        assertEquals("$$$Default", tableKeySpace);
      }
      assertFalse(table.get("isIndexTable").asBoolean());
    }
    LOG.info("Processed " + numTables + " tables");
    assertEquals(numTables, tableNames.size());
    assertAuditEntry(0, customer.uuid);
 }

  @Test
  public void testUniverseListMastersNotQueryable() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe u1 = createUniverse("Universe-1", customer.getCustomerId());
    customer.addUniverseUUID(u1.universeUUID);
    customer.save();

    Result r = tablesController.universeList(customer.uuid, u1.universeUUID);
    assertEquals(200, r.status());
    assertEquals("Expected error. Masters are not currently queryable.", contentAsString(r));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateCassandraTableWithInvalidUUID() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    String authToken = user.createAuthToken();
    customer.save();

    UUID badUUID = UUID.randomUUID();
    String method = "POST";
    String url = "/api/customers/" + customer.uuid + "/universes/" + badUUID + "/tables";
    ObjectNode emptyJson = Json.newObject();

    Result r = FakeApiHelper.doRequestWithAuthTokenAndBody(method, url, authToken, emptyJson);
    assertEquals(BAD_REQUEST, r.status());
    String errMsg = "Cannot find universe " + badUUID;
    assertThat(Json.parse(contentAsString(r)).get("error").asText(), containsString(errMsg));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateCassandraTableWithInvalidParams() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    String authToken = user.createAuthToken();
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    String method = "POST";
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables";
    ObjectNode emptyJson = Json.newObject();
    String errorString = "NullPointerException";

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody(method, url, authToken, emptyJson);
    assertEquals(BAD_REQUEST, result.status());
    assertThat(contentAsString(result), containsString(errorString));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateCassandraTableWithValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
        Matchers.any(TableDefinitionTaskParams.class))).thenReturn(fakeTaskUUID);
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    String authToken = user.createAuthToken();
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    String method = "POST";
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables";
    JsonNode topJson = Json.parse(
        "{" +
          "\"cloud\":\"aws\"," +
          "\"universeUUID\":\"" + universe.universeUUID.toString() + "\"," +
          "\"expectedUniverseVersion\":-1," +
          "\"tableUUID\":null," +
          "\"tableType\":\"YQL_TABLE_TYPE\"," +
          "\"isIndexTable\":false," +
          "\"tableDetails\":{" +
            "\"tableName\":\"test_table\"," +
            "\"keyspace\":\"test_ks\"," +
            "\"columns\":[" +
              "{" +
                "\"columnOrder\":1," +
                "\"name\":\"k\"," +
                "\"type\":\"INT\"," +
                "\"isPartitionKey\":true," +
                "\"isClusteringKey\":false" +
              "},{" +
                "\"columnOrder\":2," +
                "\"name\":\"v1\"," +
                "\"type\":\"VARCHAR\"," +
                "\"isPartitionKey\":false," +
                "\"isClusteringKey\":false" +
              "},{" +
                "\"columnOrder\":3," +
                "\"name\":\"v2\"," +
                "\"type\":\"SET\"," +
                "\"keyType\":\"INT\"," +
                "\"isPartitionKey\":false," +
                "\"isClusteringKey\":false" +
              "},{" +
                "\"columnOrder\":4," +
                "\"name\":\"v3\"," +
                "\"type\":\"MAP\"," +
                "\"keyType\":\"UUID\"," +
                "\"valueType\":\"VARCHAR\"," +
                "\"isPartitionKey\":false," +
                "\"isClusteringKey\":false" +
              "}" +
            "]" +
          "}" +
        "}");

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody(method, url, authToken, topJson);
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(json.get("taskUUID").asText(), fakeTaskUUID.toString());

    CustomerTask task = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(task);
    assertThat(task.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(task.getTargetName(), allOf(notNullValue(), equalTo("test_table")));
    assertThat(task.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Create)));
    // TODO: Ideally i think the targetUUID for tables should be tableUUID, but currently
    // we don't control the UUID generation for tables from middleware side.
    assertThat(task.getTargetUUID(), allOf(notNullValue(), equalTo(universe.universeUUID)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testDescribeTableSuccess() throws Exception {
    when(mockClient.getTableSchemaByUUID(any(String.class))).thenReturn(mockSchemaResponse);

    // Creating a fake table
    Schema schema = getFakeSchema();
    UUID tableUUID = UUID.randomUUID();
    when(mockSchemaResponse.getSchema()).thenReturn(schema);
    when(mockSchemaResponse.getTableName()).thenReturn("mock_table");
    when(mockSchemaResponse.getNamespace()).thenReturn("mock_ks");
    when(mockSchemaResponse.getTableType()).thenReturn(TableType.YQL_TABLE_TYPE);
    when(mockSchemaResponse.getTableId()).thenReturn(tableUUID.toString().replace("-", ""));

    // Creating fake authentication
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    Result result = tablesController.describe(customer.uuid, universe.universeUUID, tableUUID);
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(tableUUID.toString(), json.get("tableUUID").asText());
    assertEquals("YQL_TABLE_TYPE", json.get("tableType").asText());
    assertEquals("mock_table", json.at("/tableDetails/tableName").asText());
    assertEquals("mock_ks", json.at("/tableDetails/keyspace").asText());
    assertEquals("mock_column", json.at("/tableDetails/columns/0/name").asText());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testDescribeTableFailure() throws Exception {
    // Creating a fake table
    String mockTableUUID1 = UUID.randomUUID().toString().replace("-", "");
    UUID mockTableUUID2 = UUID.randomUUID();
    when(mockSchemaResponse.getTableId()).thenReturn(mockTableUUID1);
    when(mockClient.getTablesList()).thenReturn(mockListTablesResponse);
    when(mockClient.getTableSchemaByUUID(any(String.class))).thenReturn(mockSchemaResponse);

    // Creating fake authentication
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    Result result = tablesController.describe(customer.uuid, universe.universeUUID, mockTableUUID2);
    assertEquals(BAD_REQUEST, result.status());
    //String errMsg = "Invalid Universe UUID: " + universe.universeUUID;
    String errMsg = "UUID of table in schema (" + mockTableUUID2.toString().replace("-", "") +
        ") did not match UUID of table in request (" + mockTableUUID1 + ").";
    assertEquals(errMsg, Json.parse(contentAsString(result)).get("error").asText());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testGetColumnTypes() {
    Result result = FakeApiHelper.doRequest("GET", "/api/metadata/column_types");
    Set<ColumnDetails.YQLDataType> types = ImmutableSet.copyOf(ColumnDetails.YQLDataType.values());
    assertEquals(OK, result.status());
    JsonNode resultContent = Json.parse(contentAsString(result));
    assertThat(resultContent, notNullValue());
    JsonNode primitives = resultContent.get("primitives");
    JsonNode collections = resultContent.get("collections");
    Set<ColumnDetails.YQLDataType> resultTypes = new HashSet<>();

    // Check primitives
    for (int i = 0; i < primitives.size(); ++i) {
      String primitive = primitives.get(i).asText();
      ColumnDetails.YQLDataType type = ColumnDetails.YQLDataType.valueOf(primitive);
      assertFalse(type.isCollection());
      resultTypes.add(type);
    }

    // Check collections
    for (int i = 0; i < collections.size(); ++i) {
      String collection = collections.get(i).asText();
      ColumnDetails.YQLDataType type = ColumnDetails.YQLDataType.valueOf(collection);
      assertTrue(type.isCollection());
      resultTypes.add(type);
    }

    // Check all
    assertTrue(resultTypes.containsAll(types));
  }

  @Test
  public void testBulkImportWithValidParams() throws Exception {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
        Matchers.any(BulkImportParams.class))).thenReturn(fakeTaskUUID);

    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    ModelFactory.awsProvider(customer);
    String authToken = user.createAuthToken();
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater(aws));
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    String method = "PUT";
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + UUID.randomUUID() + "/bulk_import";
    ObjectNode topJson = Json.newObject();
    topJson.put("s3Bucket", "s3://foo.bar.com/bulkload");
    topJson.put("keyspace", "mock_ks");
    topJson.put("tableName", "mock_table");

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody(method, url, authToken, topJson);
    assertEquals(OK, result.status());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testBulkImportWithInvalidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
        Matchers.any(BulkImportParams.class))).thenReturn(fakeTaskUUID);
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    ModelFactory.awsProvider(customer);
    String authToken = user.createAuthToken();
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater(aws));
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    String method = "PUT";
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + UUID.randomUUID() + "/bulk_import";
    ObjectNode topJson = Json.newObject();
    topJson.put("s3Bucket", "foobar");
    topJson.put("keyspace", "mock_ks");
    topJson.put("tableName", "mock_table");

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody(method, url, authToken, topJson);
    assertEquals(BAD_REQUEST, result.status());
    assertThat(contentAsString(result), containsString("Invalid S3 Bucket provided: foobar"));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateBackupWithInvalidParams() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + UUID.randomUUID() + "/create_backup";
    ObjectNode bodyJson = Json.newObject();

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertEquals(BAD_REQUEST, result.status());
    assertErrorNodeValue(resultJson, "storageConfigUUID", "This field is required");
    assertErrorNodeValue(resultJson, "actionType", "This field is required");
    assertAuditEntry(0, customer.uuid);
  }
  @Test
  public void testCreateBackupWithInvalidStorageConfig() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + UUID.randomUUID() + "/create_backup";
    ObjectNode bodyJson = Json.newObject();
    UUID randomUUID = UUID.randomUUID();
    bodyJson.put("keyspace", "foo");
    bodyJson.put("tableName", "bar");
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", randomUUID.toString());

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    assertBadRequest(result, "Invalid StorageConfig UUID: " + randomUUID);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateBackupWithReadOnlyUser() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer, Users.Role.ReadOnly);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + UUID.randomUUID() + "/create_backup";
    ObjectNode bodyJson = Json.newObject();
    UUID randomUUID = UUID.randomUUID();
    bodyJson.put("keyspace", "foo");
    bodyJson.put("tableName", "bar");
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", randomUUID.toString());

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    assertForbidden(result, "User doesn't have access");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateBackupWithBackupAdminUser() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer, Users.Role.BackupAdmin);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    UUID tableUUID = UUID.randomUUID();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + tableUUID + "/create_backup";
    ObjectNode bodyJson = Json.newObject();
    UUID randomUUID = UUID.randomUUID();
    bodyJson.put("keyspace", "foo");
    bodyJson.put("tableName", "bar");
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());

    ArgumentCaptor<TaskType> taskType = ArgumentCaptor.forClass(TaskType.class);;
    ArgumentCaptor<BackupTableParams> taskParams =
        ArgumentCaptor.forClass(BackupTableParams.class);
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    System.out.println(result);
    verify(mockCommissioner, times(1)).submit(taskType.capture(), taskParams.capture());
    assertEquals(TaskType.BackupUniverse, taskType.getValue());
    String storageRegex = "s3://foo/univ-" + universe.universeUUID + "/backup-"+
        "\\d{4}-[0-1]\\d-[0-3]\\dT[0-2]\\d:[0-5]\\d:[0-5]\\d\\-\\d+/table-foo.bar-[a-zA-Z0-9]*";
    assertThat(taskParams.getValue().storageLocation, RegexMatcher.matchesRegex(storageRegex));
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "taskUUID", fakeTaskUUID.toString());
    CustomerTask ct = CustomerTask.findByTaskUUID(fakeTaskUUID);
    assertNotNull(ct);
    Backup backup = Backup.fetchByTaskUUID(fakeTaskUUID);
    assertNotNull(backup);
    assertEquals(tableUUID, backup.getBackupInfo().tableUUID);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateBackupWithValidParams() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    UUID tableUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + tableUUID + "/create_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("keyspace", "foo");
    bodyJson.put("tableName", "bar");
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());

    ArgumentCaptor<TaskType> taskType = ArgumentCaptor.forClass(TaskType.class);;
    ArgumentCaptor<BackupTableParams> taskParams =  ArgumentCaptor.forClass(BackupTableParams.class);;
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    verify(mockCommissioner, times(1)).submit(taskType.capture(), taskParams.capture());
    assertEquals(TaskType.BackupUniverse, taskType.getValue());
    String storageRegex = "s3://foo/univ-" + universe.universeUUID + "/backup-\\d{4}-[0-1]\\d-[0-3]\\dT[0-2]\\d:[0-5]\\d:[0-5]\\d\\-\\d+/table-foo.bar-[a-zA-Z0-9]*";
    assertThat(taskParams.getValue().storageLocation, RegexMatcher.matchesRegex(storageRegex));
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "taskUUID", fakeTaskUUID.toString());
    CustomerTask ct = CustomerTask.findByTaskUUID(fakeTaskUUID);
    assertNotNull(ct);
    Backup backup = Backup.fetchByTaskUUID(fakeTaskUUID);
    assertNotNull(backup);
    assertEquals(tableUUID, backup.getBackupInfo().tableUUID);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateBackupOnDisabledTableFails() throws Exception {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    TablesController mockTablesController = spy(tablesController);

    doReturn(true).when(mockTablesController).disableBackupOnTables(any(), any());
    UUID uuid = UUID.randomUUID();
    Result r = mockTablesController.createBackup(customer.uuid, universe.universeUUID, uuid);

    assertBadRequest(r, "Invalid Table UUID: " + uuid + ". Cannot backup index or YSQL table.");
  }

  @Test
  public void testCreateBackupFailureInProgress() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    UUID tableUUID = UUID.randomUUID();
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID,
                                    ApiUtils.mockUniverseUpdater("host", null, true));
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + tableUUID + "/create_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("keyspace", "foo");
    bodyJson.put("tableName", "bar");
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);

    String errMsg = String.format("Cannot run Backup task since the " +
                                  "universe %s is currently in a locked state.",
                                  universe.universeUUID.toString());
    assertBadRequest(result, errMsg);
  }

  @Test
  public void testCreateBackupCronExpression() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    UUID tableUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/tables/" + tableUUID + "/create_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("keyspace", "foo");
    bodyJson.put("tableName", "bar");
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());
    bodyJson.put("cronExpression", "5 * * * *");
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    UUID scheduleUUID = UUID.fromString(resultJson.path("scheduleUUID").asText());
    Schedule schedule = Schedule.get(scheduleUUID);
    assertNotNull(schedule);
    assertEquals(schedule.getCronExpression(), "5 * * * *");
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateMultiBackup() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    UUID tableUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/multi_table_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());

    ArgumentCaptor<TaskType> taskType = ArgumentCaptor.forClass(TaskType.class);;
    ArgumentCaptor<MultiTableBackup.Params> taskParams =
        ArgumentCaptor.forClass(MultiTableBackup.Params.class);;
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    verify(mockCommissioner, times(1)).submit(taskType.capture(), taskParams.capture());
    assertEquals(TaskType.MultiTableBackup, taskType.getValue());
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "taskUUID", fakeTaskUUID.toString());
    CustomerTask ct = CustomerTask.findByTaskUUID(fakeTaskUUID);
    assertNotNull(ct);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateMultiBackupFailureInProgress() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID,
                                    ApiUtils.mockUniverseUpdater("host", null, true));
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/multi_table_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    String errMsg = String.format("Cannot run Backup task since the " +
                                  "universe %s is currently in a locked state.",
                                  universe.universeUUID.toString());
    assertBadRequest(result, errMsg);
  }

  @Test
  public void testCreateMultiBackupScheduleCron() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    UUID tableUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/multi_table_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());
    bodyJson.put("cronExpression", "5 * * * *");

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    UUID scheduleUUID = UUID.fromString(resultJson.path("scheduleUUID").asText());
    Schedule schedule = Schedule.get(scheduleUUID);
    assertNotNull(schedule);
    assertEquals(schedule.getCronExpression(), "5 * * * *");
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateMultiBackupScheduleFrequency() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = ModelFactory.createUniverse(customer.getCustomerId());
    UUID tableUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universe.universeUUID +
        "/multi_table_backup";
    ObjectNode bodyJson = Json.newObject();
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(customer);
    bodyJson.put("actionType", "CREATE");
    bodyJson.put("storageConfigUUID", customerConfig.configUUID.toString());
    bodyJson.put("schedulingFrequency", "6000");

    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("PUT", url,
        user.createAuthToken(), bodyJson);
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    UUID scheduleUUID = UUID.fromString(resultJson.path("scheduleUUID").asText());
    Schedule schedule = Schedule.get(scheduleUUID);
    assertNotNull(schedule);
    assertEquals(schedule.getFrequency(), 6000);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testDeleteTableWithValidParams() throws Exception {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Map<String, String> flashData = Collections.emptyMap();
    Map<String, Object> argData = ImmutableMap.of("user", user);
    Http.Request request = mock(Http.Request.class);
    Long id = 2L;
    play.api.mvc.RequestHeader header = mock(play.api.mvc.RequestHeader.class);
    Http.Context context = new Http.Context(
      id, header, request, flashData, flashData, argData, contextComponents()
    );
    Http.Context.current.set(context);
    tablesController.commissioner = mockCommissioner;
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);

    // Creating a fake table
    Schema schema = getFakeSchema();
    UUID tableUUID = UUID.randomUUID();
    when(mockClient.getTableSchemaByUUID(eq(tableUUID.toString().replace("-", ""))))
      .thenReturn(mockSchemaResponse);
    when(mockSchemaResponse.getSchema()).thenReturn(schema);
    when(mockSchemaResponse.getTableName()).thenReturn("mock_table");
    when(mockSchemaResponse.getNamespace()).thenReturn("mock_ks");
    when(mockSchemaResponse.getTableType()).thenReturn(TableType.YQL_TABLE_TYPE);
    when(mockSchemaResponse.getTableId()).thenReturn(tableUUID.toString().replace("-", ""));
    when(request.method()).thenReturn("DELETE");
    when(request.path()).thenReturn("/api/customer/test/universe/test");

    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    Result result = tablesController.drop(customer.uuid, universe.universeUUID, tableUUID);
    assertEquals(OK, result.status());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testDeleteTableWithInvalidparams() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    Universe universe = createUniverse(customer.getCustomerId());
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    UUID badTableUUID = UUID.randomUUID();
    String errorString = "No table for UUID: " + badTableUUID;

    Result result = tablesController.drop(customer.uuid, universe.universeUUID, badTableUUID);
    assertEquals(BAD_REQUEST, result.status());
    assertThat(contentAsString(result), containsString(errorString));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testDisallowBackup() throws Exception {
    List<TableInfo> tableInfoList = new ArrayList<TableInfo>();
    UUID table1Uuid = UUID.randomUUID();
    UUID table2Uuid = UUID.randomUUID();
    UUID indexUuid = UUID.randomUUID();
    UUID ysqlUuid = UUID.randomUUID();
    TableInfo ti1 = TableInfo.newBuilder()
            .setName("Table1")
            .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("$$$Default"))
            .setId(ByteString.copyFromUtf8(table1Uuid.toString()))
            .setTableType(TableType.YQL_TABLE_TYPE)
            .build();
    TableInfo ti2 = TableInfo.newBuilder()
            .setName("Table2")
            .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("$$$Default"))
            .setId(ByteString.copyFromUtf8(table2Uuid.toString()))
            .setTableType(TableType.YQL_TABLE_TYPE)
            .build();
    TableInfo ti3 = TableInfo.newBuilder()
            .setName("TableIndex")
            .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("$$$Default"))
            .setId(ByteString.copyFromUtf8(indexUuid.toString()))
            .setTableType(TableType.YQL_TABLE_TYPE)
            .setRelationType(RelationType.INDEX_TABLE_RELATION)
            .build();
    TableInfo ti4 = TableInfo.newBuilder()
            .setName("TableYsql")
            .setNamespace(Master.NamespaceIdentifierPB.newBuilder().setName("$$$Default"))
            .setId(ByteString.copyFromUtf8(ysqlUuid.toString()))
            .setTableType(TableType.PGSQL_TABLE_TYPE)
            .build();


    tableInfoList.add(ti1);
    tableInfoList.add(ti2);
    tableInfoList.add(ti3);
    tableInfoList.add(ti4);

    when(mockListTablesResponse.getTableInfoList()).thenReturn(tableInfoList);
    when(mockClient.getTablesList()).thenReturn(mockListTablesResponse);
    Universe universe = mock(Universe.class);
    when(universe.getMasterAddresses(anyBoolean())).thenReturn("fake_address");
    when(universe.getCertificate()).thenReturn("fake_certificate");


    // Disallow on Index Table.
    List<UUID> uuids = Arrays.asList(table1Uuid, table2Uuid, indexUuid);
    assertTrue(tablesController.disableBackupOnTables(uuids, universe));


    // Disallow on YSQL table.
    uuids = Arrays.asList(table1Uuid, table2Uuid, ysqlUuid);
    assertTrue(tablesController.disableBackupOnTables(uuids, universe));


    // Allow on YCQL tables and empty list.
    uuids = Arrays.asList(table1Uuid, table2Uuid);
    assertFalse(tablesController.disableBackupOnTables(uuids, universe));

    assertFalse(tablesController.disableBackupOnTables(new ArrayList<UUID>(), universe));
  }
}
