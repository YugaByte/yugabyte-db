// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Test;
import org.mockito.ArgumentCaptor;
import play.libs.Json;
import play.mvc.Result;

import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.*;
import static com.yugabyte.yw.models.CustomerTask.TaskType.Restore;
import static org.junit.Assert.*;
import static org.mockito.Mockito.*;
import static play.mvc.Http.Status.BAD_REQUEST;
import static play.mvc.Http.Status.FORBIDDEN;
import static play.test.Helpers.contentAsString;

public class BackupsControllerTest extends FakeDBApplication {

  private Universe defaultUniverse;
  private Users defaultUser;
  private Customer defaultCustomer;
  private Backup defaultBackup;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultUser = ModelFactory.testUser(defaultCustomer);
    defaultUniverse = ModelFactory.createUniverse(defaultCustomer.getCustomerId());

    BackupTableParams backupTableParams = new BackupTableParams();
    backupTableParams.universeUUID = defaultUniverse.universeUUID;
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(defaultCustomer);
    backupTableParams.storageConfigUUID = customerConfig.configUUID;
    defaultBackup = Backup.create(defaultCustomer.uuid, backupTableParams);
  }

  private JsonNode listBackups(UUID universeUUID) {
    String authToken = defaultUser.createAuthToken();
    String method = "GET";
    String url = "/api/customers/" + defaultCustomer.uuid + "/universes/" + universeUUID +
      "/backups";

    Result r = FakeApiHelper.doRequestWithAuthToken(method, url, authToken);
    assertOk(r);
    return Json.parse(contentAsString(r));
  }

  @Test
  public void testListWithValidUniverse() {
    JsonNode resultJson = listBackups(defaultUniverse.universeUUID);
    assertEquals(1, resultJson.size());
    assertValues(resultJson, "backupUUID", ImmutableList.of(defaultBackup.backupUUID.toString()));
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testListWithInvalidUniverse() {
    JsonNode resultJson = listBackups(UUID.randomUUID());
    assertEquals(0, resultJson.size());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  private Result restoreBackup(UUID universeUUID, JsonNode bodyJson, Users user) {
    String authToken = defaultUser.createAuthToken();
    if (user != null) {
      authToken = user.createAuthToken();
    }
    String method = "POST";
    String url = "/api/customers/" + defaultCustomer.uuid +
      "/universes/" + universeUUID + "/backups/restore";
    return FakeApiHelper.doRequestWithAuthTokenAndBody(method, url, authToken, bodyJson);
  }

  @Test
  public void testRestoreBackupWithInvalidUniverseUUID() {
    UUID universeUUID = UUID.randomUUID();
    JsonNode bodyJson = Json.newObject();

    Result result = restoreBackup(universeUUID, bodyJson, null);
    assertEquals(BAD_REQUEST, result.status());
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "error", "Invalid Universe UUID: " + universeUUID);
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testRestoreBackupWithInvalidParams() {
    BackupTableParams bp = new BackupTableParams();
    bp.storageConfigUUID = UUID.randomUUID();
    Backup.create(defaultCustomer.uuid, bp);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("actionType", "RESTORE");
    Result result = restoreBackup(defaultUniverse.universeUUID, bodyJson, null);
    assertEquals(BAD_REQUEST, result.status());
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertErrorNodeValue(resultJson, "storageConfigUUID", "This field is required");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testRestoreBackupWithoutStorageLocation() {
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(defaultCustomer);
    BackupTableParams bp = new BackupTableParams();
    bp.storageConfigUUID = customerConfig.configUUID;
    Backup.create(defaultCustomer.uuid, bp);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("keyspace", "mock_ks");
    bodyJson.put("tableName", "mock_table");
    bodyJson.put("actionType", "RESTORE");
    bodyJson.put("storageConfigUUID", bp.storageConfigUUID.toString());
    Result result = restoreBackup(defaultUniverse.universeUUID, bodyJson, null);
    assertEquals(BAD_REQUEST, result.status());
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "error", "Storage Location is required");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testRestoreBackupWithInvalidStorageUUID() {
    BackupTableParams bp = new BackupTableParams();
    bp.storageConfigUUID = UUID.randomUUID();
    Backup b = Backup.create(defaultCustomer.uuid, bp);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("keyspace", "mock_ks");
    bodyJson.put("tableName", "mock_table");
    bodyJson.put("actionType", "RESTORE");
    bodyJson.put("storageConfigUUID", bp.storageConfigUUID.toString());
    bodyJson.put("storageLocation", b.getBackupInfo().storageLocation);
    Result result = restoreBackup(defaultUniverse.universeUUID, bodyJson, null);
    assertEquals(BAD_REQUEST, result.status());
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "error", "Invalid StorageConfig UUID: " + bp.storageConfigUUID);
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testRestoreBackupWithReadOnlyUser() {
    Users user = ModelFactory.testUser(defaultCustomer, "tc@test.com",
      Users.Role.ReadOnly);
    BackupTableParams bp = new BackupTableParams();
    bp.storageConfigUUID = UUID.randomUUID();
    Backup b = Backup.create(defaultCustomer.uuid, bp);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("keyspace", "mock_ks");
    bodyJson.put("tableName", "mock_table");
    bodyJson.put("actionType", "RESTORE");
    bodyJson.put("storageConfigUUID", bp.storageConfigUUID.toString());
    bodyJson.put("storageLocation", b.getBackupInfo().storageLocation);
    Result result = restoreBackup(defaultUniverse.universeUUID, bodyJson, user);
    assertEquals(FORBIDDEN, result.status());
    assertEquals("User doesn't have access", contentAsString(result));
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testRestoreBackupWithValidParams() {
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(defaultCustomer);
    BackupTableParams bp = new BackupTableParams();
    bp.storageConfigUUID = customerConfig.configUUID;
    Backup b = Backup.create(defaultCustomer.uuid, bp);
    ObjectNode bodyJson = Json.newObject();

    long maxReqSizeInBytes =
      app.config().getMemorySize("play.http.parser.maxMemoryBuffer").toBytes();

    // minus 1000 so as to leave some room for other fields and headers etc.
    int keyspaceSz = (int) (maxReqSizeInBytes - 1000);

    // Intentionally use large keyspace field approaching (but not exceeding) 500k (which is
    // now a default for play.http.parser.maxMemoryBuffer)
    String largeKeyspace = new String(new char[keyspaceSz])
      .replace("\0", "#");
    bodyJson.put("keyspace", largeKeyspace);
    bodyJson.put("tableName", "mock_table");
    bodyJson.put("actionType", "RESTORE");
    bodyJson.put("storageConfigUUID", bp.storageConfigUUID.toString());
    bodyJson.put("storageLocation", "s3://foo/bar");

    ArgumentCaptor<TaskType> taskType = ArgumentCaptor.forClass(TaskType.class);
    ArgumentCaptor<BackupTableParams> taskParams = ArgumentCaptor.forClass(BackupTableParams.class);

    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);
    Result result = restoreBackup(defaultUniverse.universeUUID, bodyJson, null);
    verify(mockCommissioner, times(1)).submit(taskType.capture(), taskParams.capture());
    assertEquals(TaskType.BackupUniverse, taskType.getValue());
    assertOk(result);
    JsonNode resultJson = Json.parse(contentAsString(result));
    assertValue(resultJson, "taskUUID", fakeTaskUUID.toString());
    CustomerTask ct = CustomerTask.findByTaskUUID(fakeTaskUUID);
    assertNotNull(ct);
    assertEquals(Restore, ct.getType());
    Backup backup = Backup.fetchByTaskUUID(fakeTaskUUID);
    assertNotEquals(b.backupUUID, backup.backupUUID);
    assertNotNull(backup);
    assertValue(backup.backupInfo, "actionType", "RESTORE");
    assertValue(backup.backupInfo, "storageLocation", "s3://foo/bar");
    assertAuditEntry(1, defaultCustomer.uuid);
  }

  // For security reasons, performance reasons and DOS protection we should continue to
  // impose some limit on request size. Here we test that sending request larger that 500K will
  // cause us to return
  @Test
  public void testRestoreBackupRequestTooLarge() {
    CustomerConfig customerConfig = ModelFactory.createS3StorageConfig(defaultCustomer);
    BackupTableParams bp = new BackupTableParams();
    bp.storageConfigUUID = customerConfig.configUUID;
    Backup.create(defaultCustomer.uuid, bp);
    ObjectNode bodyJson = Json.newObject();

    long maxReqSizeInBytes =
      app.config().getMemorySize("play.http.parser.maxMemoryBuffer").toBytes();
    String largeKeyspace = new String(new char[(int) (maxReqSizeInBytes)])
      .replace("\0", "#");
    bodyJson.put("keyspace", largeKeyspace);
    bodyJson.put("tableName", "mock_table");
    bodyJson.put("actionType", "RESTORE");
    bodyJson.put("storageConfigUUID", bp.storageConfigUUID.toString());
    bodyJson.put("storageLocation", "s3://foo/bar");

    int aproxPayloadLength = bodyJson.toString().length();
    assertTrue("Actual (approx) payload size " + aproxPayloadLength,
      aproxPayloadLength > maxReqSizeInBytes && aproxPayloadLength < maxReqSizeInBytes + 1000);
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);
    Result result = restoreBackup(defaultUniverse.universeUUID, bodyJson, null);
    assertEquals(413, result.status());
    verify(mockCommissioner, never()).submit(any(), any());
  }
}
