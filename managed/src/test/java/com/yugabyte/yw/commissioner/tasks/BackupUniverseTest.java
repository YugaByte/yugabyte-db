// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import static org.mockito.Mockito.mock;
import org.yb.client.YBClient;
import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.master.Master;

import java.util.UUID;
import java.util.HashMap;
import java.util.Map;

import static com.yugabyte.yw.models.Backup.BackupState.Completed;
import static com.yugabyte.yw.models.Backup.BackupState.Failed;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.mockito.Matchers.any;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;


@RunWith(MockitoJUnitRunner.class)
public class BackupUniverseTest extends CommissionerBaseTest {

  @InjectMocks
  Commissioner commissioner;

  Universe defaultUniverse;

  private YBClient mockClient;

  @Before
  public void setUp() {
    mockClient = mock(YBClient.class);
    Master.SysClusterConfigEntryPB.Builder configBuilder =
      Master.SysClusterConfigEntryPB.newBuilder().setVersion(1);
    GetMasterClusterConfigResponse mockConfigResponse =
      new GetMasterClusterConfigResponse(0, "", configBuilder.build(), null);
    ChangeMasterClusterConfigResponse mockChangeConfigResponse =
      new ChangeMasterClusterConfigResponse(0, "", null);
    try {
      when(mockClient.getMasterClusterConfig()).thenReturn(mockConfigResponse);
      when(mockClient.changeMasterClusterConfig(any())).thenReturn(mockChangeConfigResponse);
    } catch (Exception e) {}
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    defaultCustomer = ModelFactory.testCustomer();
    defaultUniverse = ModelFactory.createUniverse();
    Map<String, String> config = new HashMap<>();
    config.put(Universe.TAKE_BACKUPS, "true");
    defaultUniverse.setConfig(config);
  }

  private TaskInfo submitTask(BackupTableParams.ActionType actionType, boolean enableVerboseLogs) {
    BackupTableParams backupTableParams = new BackupTableParams();
    backupTableParams.universeUUID = defaultUniverse.universeUUID;
    backupTableParams.tableName = "bar";
    backupTableParams.keyspace = "foo";
    backupTableParams.tableUUID = UUID.randomUUID();
    backupTableParams.storageConfigUUID = UUID.randomUUID();
    backupTableParams.actionType = actionType;
    backupTableParams.enableVerboseLogs = enableVerboseLogs;
    try {
      Backup backup = Backup.create(defaultCustomer.uuid, backupTableParams);
      UUID taskUUID = commissioner.submit(TaskType.BackupUniverse, backupTableParams);
      backup.setTaskUUID(taskUUID);
      CustomerTask.create(defaultCustomer, defaultUniverse.universeUUID, taskUUID,
          CustomerTask.TargetType.Backup, CustomerTask.TaskType.Create,
          "bar");
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  @Test
  public void testBackupTableCreateAction() {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"success\": true}";
    shellResponse.code = 0;
    when(mockTableManager.createBackup(any())).thenReturn(shellResponse);

    TaskInfo taskInfo = submitTask(BackupTableParams.ActionType.CREATE, false);
    verify(mockTableManager, times(1)).createBackup(any());
    assertEquals(TaskInfo.State.Success, taskInfo.getTaskState());
    Backup backup = Backup.fetchByTaskUUID(taskInfo.getTaskUUID());
    assertNotNull(backup);
    assertEquals(Completed, backup.state);
  }

  @Test
  public void testBackupTableError() {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"error\": true}";
    shellResponse.code = 0;
    when(mockTableManager.createBackup(any())).thenReturn(shellResponse);

    TaskInfo taskInfo = submitTask(BackupTableParams.ActionType.CREATE, true);
    verify(mockTableManager, times(1)).createBackup(any());
    Backup backup = Backup.fetchByTaskUUID(taskInfo.getTaskUUID());
    assertNotNull(backup);
    assertEquals(Failed, backup.state);
  }

  @Test
  public void testBackupTableFatal() {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"error\": true}";
    shellResponse.code = 99;
    when(mockTableManager.createBackup(any())).thenReturn(shellResponse);
    TaskInfo taskInfo = submitTask(BackupTableParams.ActionType.CREATE, true);
    verify(mockTableManager, times(1)).createBackup(any());
    Backup backup = Backup.fetchByTaskUUID(taskInfo.getTaskUUID());
    assertNotNull(backup);
    assertEquals(Failed, backup.state);
  }

  @Test
  public void testBackupTableRestoreAction() {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"success\": true}";
    shellResponse.code = 0;
    when(mockTableManager.createBackup(any())).thenReturn(shellResponse);

    TaskInfo taskInfo = submitTask(BackupTableParams.ActionType.RESTORE, false);
    verify(mockTableManager, times(1)).createBackup(any());
    assertEquals(TaskInfo.State.Success, taskInfo.getTaskState());
    Backup backup = Backup.fetchByTaskUUID(taskInfo.getTaskUUID());
    assertNotNull(backup);
    assertEquals(Completed, backup.state);
  }

  @Test
  public void testBackupTableRestoreVerbose() {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"snapshot_url\": \"s3://random\", \"skipthis\": \"INFO\"}";
    shellResponse.code = 0;
    when(mockTableManager.createBackup(any())).thenReturn(shellResponse);

    TaskInfo taskInfo = submitTask(BackupTableParams.ActionType.RESTORE, true);
    verify(mockTableManager, times(1)).createBackup(any());
    assertEquals(TaskInfo.State.Success, taskInfo.getTaskState());
    Backup backup = Backup.fetchByTaskUUID(taskInfo.getTaskUUID());
    assertNotNull(backup);
    assertEquals(Completed, backup.state);
  }

  @Test
  public void testBackupTableInvalidAction() {
    TaskInfo taskInfo = submitTask(null, false);
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
    verify(mockTableManager, times(0)).createBackup(any());
  }
}
