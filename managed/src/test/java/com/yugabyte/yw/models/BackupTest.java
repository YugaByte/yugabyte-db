// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.RegexMatcher;
import com.yugabyte.yw.forms.BackupTableParams;
import org.junit.Before;
import org.junit.Test;
import play.libs.Json;

import java.util.Date;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import static com.yugabyte.yw.models.Backup.BackupState.Completed;
import static com.yugabyte.yw.models.Backup.BackupState.Deleted;
import static com.yugabyte.yw.models.Backup.BackupState.Failed;
import static com.yugabyte.yw.models.Backup.BackupState.InProgress;
import static org.junit.Assert.*;


public class BackupTest extends FakeDBApplication {
  private Customer defaultCustomer;
  private CustomerConfig s3StorageConfig;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    s3StorageConfig = ModelFactory.createS3StorageConfig(defaultCustomer);
  }

  @Test
  public void testCreate() {
    UUID universeUUID = UUID.randomUUID();
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        universeUUID, s3StorageConfig.configUUID);
    assertNotNull(b);
    String storageRegex = "s3://foo/univ-" + universeUUID +
        "/backup-\\d{4}-[0-1]\\d-[0-3]\\dT[0-2]\\d:[0-5]\\d:[0-5]\\d\\-\\d+/table-foo.bar-[a-zA-Z0-9]*";
    assertThat(b.getBackupInfo().storageLocation, RegexMatcher.matchesRegex(storageRegex));
    assertEquals(s3StorageConfig.configUUID, b.getBackupInfo().storageConfigUUID);
    assertEquals(InProgress, b.state);
  }

  @Test
  public void testCreateWithoutTableUUID() {
    UUID universeUUID = UUID.randomUUID();
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = s3StorageConfig.configUUID;
    params.universeUUID = universeUUID;
    params.keyspace = "foo";
    params.tableName = "bar";
    Backup b = Backup.create(defaultCustomer.uuid, params);
    String storageRegex = "s3://foo/univ-" + universeUUID +
        "/backup-\\d{4}-[0-1]\\d-[0-3]\\dT[0-2]\\d:[0-5]\\d:[0-5]\\d\\-\\d+/table-foo.bar";
    assertThat(b.getBackupInfo().storageLocation, RegexMatcher.matchesRegex(storageRegex));
    assertEquals(s3StorageConfig.configUUID, b.getBackupInfo().storageConfigUUID);
    assertEquals(InProgress, b.state);
  }

  @Test
  public void testCreateWithNonS3StorageUUID() {
    JsonNode formData = Json.parse("{\"name\": \"FILE\", \"type\": \"STORAGE\", \"data\": \"{}\"}");
    CustomerConfig customerConfig = CustomerConfig.createWithFormData(defaultCustomer.uuid, formData);
    UUID universeUUID = UUID.randomUUID();
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = customerConfig.configUUID;
    params.universeUUID = universeUUID;
    params.keyspace = "foo";
    params.tableName = "bar";
    Backup b = Backup.create(defaultCustomer.uuid, params);
    String storageRegex = "univ-" + universeUUID +
        "/backup-\\d{4}-[0-1]\\d-[0-3]\\dT[0-2]\\d:[0-5]\\d:[0-5]\\d\\-\\d+/table-foo.bar";
    assertThat(b.getBackupInfo().storageLocation, RegexMatcher.matchesRegex(storageRegex));
    assertEquals(customerConfig.configUUID, b.getBackupInfo().storageConfigUUID);
    assertEquals(InProgress, b.state);
  }

  @Test
  public void testFetchByUniverseWithValidUUID() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    List<Backup> backupList = Backup.fetchByUniverseUUID(defaultCustomer.uuid, u.universeUUID);
    assertEquals(1, backupList.size());
  }

  @Test
  public void testFetchByUniverseWithInvalidUUID() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    List<Backup> backupList = Backup.fetchByUniverseUUID(defaultCustomer.uuid, UUID.randomUUID());
    assertEquals(0, backupList.size());
  }


  @Test
  public void testFetchByTaskWithValidUUID() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    UUID taskUUID = UUID.randomUUID();
    b.setTaskUUID(taskUUID);
    Backup fb = Backup.fetchByTaskUUID(taskUUID);
    assertNotNull(fb);
    assertEquals(fb, b);
  }

  @Test
  public void testFetchByTaskWithInvalidBackupUUID() {
    CustomerTask ct = CustomerTask.create(
        defaultCustomer,
        UUID.randomUUID(),
        UUID.randomUUID(),
        CustomerTask.TargetType.Backup,
        CustomerTask.TaskType.Create,
        "Demo Backup");
    Backup fb = Backup.fetchByTaskUUID(ct.getTaskUUID());
    assertNull(fb);
  }

  @Test
  public void testFetchByTaskWithTargetType() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    CustomerTask ct = CustomerTask.create(
        defaultCustomer,
        b.backupUUID,
        UUID.randomUUID(),
        CustomerTask.TargetType.Table,
        CustomerTask.TaskType.Create,
        "Demo Backup");
    Backup fb = Backup.fetchByTaskUUID(ct.getTaskUUID());
    assertNull(fb);
  }

  @Test
  public void testGetWithValidCustomerUUID() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    Backup fb = Backup.get(defaultCustomer.uuid, b.backupUUID);
    assertEquals(fb, b);
  }

  @Test
  public void testGetWithInvalidCustomerUUID() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    Backup fb = Backup.get(UUID.randomUUID(), b.backupUUID);
    assertNull(fb);
  }


  @Test
  public void testTransitionStateValid() throws InterruptedException {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    Date beforeUpdateTime  = b.getUpdateTime();
    assertNotNull(beforeUpdateTime);
    Thread.sleep(1);
    b.transitionState(Backup.BackupState.Completed);
    assertEquals(Completed, b.state);
    assertNotNull(b.getUpdateTime());
    assertNotEquals(beforeUpdateTime, b.getUpdateTime());
    b.transitionState(Deleted);
    assertEquals(Deleted, b.state);
  }

  @Test
  public void testTransitionStateInvalid() throws InterruptedException {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    Date beforeUpdateTime  = b.getUpdateTime();
    assertNotNull(b.getUpdateTime());
    Thread.sleep(1);
    b.transitionState(Backup.BackupState.Completed);
    assertNotNull(b.getUpdateTime());
    assertNotEquals(beforeUpdateTime, b.getUpdateTime());
    b.transitionState(Failed);
    assertNotEquals(Failed, b.state);
  }

  @Test
  public void testSetTaskUUIDWhenNull() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    UUID taskUUID = UUID.randomUUID();
    assertNull(b.taskUUID);
    b.setTaskUUID(taskUUID);
    b.refresh();
    assertEquals(taskUUID, b.taskUUID);
  }

  @Test
  public void testSetTaskUUID() throws InterruptedException {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    UUID taskUUID1 = UUID.randomUUID();
    UUID taskUUID2 = UUID.randomUUID();
    ExecutorService service = Executors.newFixedThreadPool(2);
    AtomicBoolean success1 = new AtomicBoolean();
    AtomicBoolean success2 = new AtomicBoolean();
    service.submit(() -> success1.set(b.setTaskUUID(taskUUID1)));
    service.submit(() -> success2.set(b.setTaskUUID(taskUUID2)));
    service.awaitTermination(3, TimeUnit.SECONDS);
    b.refresh();
    if (success1.get() && !success2.get()) {
      assertEquals(taskUUID1, b.taskUUID);
    } else {
      assertFalse(success1.get());
      assertTrue(success2.get());
      assertEquals(taskUUID2, b.taskUUID);
    }
  }

  @Test
  public void testSetTaskUUIDWhenNotNull() {
    Universe u = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    Backup b = ModelFactory.createBackup(defaultCustomer.uuid,
        u.universeUUID, s3StorageConfig.configUUID);
    b.setTaskUUID(UUID.randomUUID());
    UUID taskUUID = UUID.randomUUID();
    assertNotNull(b.taskUUID);
    b.setTaskUUID(taskUUID);
    b.refresh();
    assertNotEquals(taskUUID, b.taskUUID);
  }
}
