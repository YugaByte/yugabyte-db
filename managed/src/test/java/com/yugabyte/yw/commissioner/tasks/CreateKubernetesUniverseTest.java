// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.RegexMatcher;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import org.yb.Common;
import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.client.ListTabletServersResponse;
import org.yb.client.YBClient;
import org.yb.client.YBTable;
import org.yb.master.Master;
import play.libs.Json;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;

import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCommandExecutor.CommandType.CREATE_NAMESPACE;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCommandExecutor.CommandType.APPLY_SECRET;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCommandExecutor.CommandType.HELM_INSTALL;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCommandExecutor.CommandType.POD_INFO;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCheckNumPod.CommandType.WAIT_FOR_PODS;
import static com.yugabyte.yw.commissioner.tasks.subtasks.UpdatePlacementInfo.ModifyUniverseConfig;
import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.models.TaskInfo.State.Failure;
import static com.yugabyte.yw.models.TaskInfo.State.Success;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Mockito.doNothing;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;


@RunWith(MockitoJUnitRunner.class)
public class CreateKubernetesUniverseTest extends CommissionerBaseTest {

  @InjectMocks
  Commissioner commissioner;

  Universe defaultUniverse;

  YBClient mockClient;

  String nodePrefix = "demo-universe";

  Map<String, String> config= new HashMap<String, String>();

  private void setupUniverseMultiAZ(boolean setMasters) {
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion-1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ-1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ-2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ-3", "subnet-3");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams.UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    userIntent.replicationFactor = 3;
    userIntent.masterGFlags = new HashMap<>();
    userIntent.tserverGFlags = new HashMap<>();
    userIntent.universeName = "demo-universe";
    userIntent.ybSoftwareVersion = "1.0.0";
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, nodePrefix, setMasters /* setMasters */));
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
  }

  private void setupUniverse(boolean setMasters) {
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion-1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ-1", "subnet-1");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams.UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    userIntent.replicationFactor = 3;
    userIntent.masterGFlags = new HashMap<>();
    userIntent.tserverGFlags = new HashMap<>();
    userIntent.universeName = "demo-universe";
    userIntent.ybSoftwareVersion = "1.0.0";
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, nodePrefix, setMasters /* setMasters */));
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
  }

  private void setupCommon() {
    config.put("KUBECONFIG", "test");
    defaultProvider.setConfig(config);
    defaultProvider.save();
    ShellProcessHandler.ShellResponse response = new ShellProcessHandler.ShellResponse();
    when(mockKubernetesManager.createNamespace(anyMap(), any())).thenReturn(response);
    when(mockKubernetesManager.helmInstall(anyMap(), any(), any(), any())).thenReturn(response);
    // Table RPCs.
    mockClient = mock(YBClient.class);
    // WaitForTServerHeartBeats mock.
    ListTabletServersResponse mockResponse = mock(ListTabletServersResponse.class);
    when(mockResponse.getTabletServersCount()).thenReturn(3);
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    YBTable mockTable = mock(YBTable.class);
    when(mockTable.getName()).thenReturn("redis");
    when(mockTable.getTableType()).thenReturn(Common.TableType.REDIS_TABLE_TYPE);
    ChangeMasterClusterConfigResponse ccr = new ChangeMasterClusterConfigResponse(1111, "", null);
    try {
      when(mockClient.changeMasterClusterConfig(any())).thenReturn(ccr);
      when(mockClient.listTabletServers()).thenReturn(mockResponse);
      when(mockClient.createRedisTable(any())).thenReturn(mockTable);
    } catch (Exception e) {}
    // WaitForServer mock.
    mockWaits(mockClient);
  }

  List<TaskType> KUBERNETES_CREATE_UNIVERSE_TASKS = ImmutableList.of(
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCheckNumPod,
      TaskType.KubernetesCommandExecutor,
      TaskType.WaitForServer,
      TaskType.WaitForMasterLeader,
      TaskType.UpdatePlacementInfo,
      TaskType.WaitForTServerHeartBeats,
      TaskType.SwamperTargetsFileUpdate,
      TaskType.CreateTable,
      TaskType.UniverseUpdateSucceeded);

  // Cannot use defaultUniverse.universeUUID in a class field.
  List<JsonNode> getExpectedCreateUniverseTaskResults() {
    return ImmutableList.of(
      Json.toJson(ImmutableMap.of("commandType", CREATE_NAMESPACE.name())),
      Json.toJson(ImmutableMap.of("commandType", APPLY_SECRET.name())),
      Json.toJson(ImmutableMap.of("commandType", HELM_INSTALL.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_PODS.name())),
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("removeFile", false)),
      Json.toJson(ImmutableMap.of("tableType", "REDIS_TABLE_TYPE",
                                  "tableName", "redis")),
      Json.toJson(ImmutableMap.of())
    );
  }

  private void assertTaskSequence(Map<Integer, List<TaskInfo>> subTasksByPosition, int numTasks) {
    int position = 0;
    for (TaskType taskType: KUBERNETES_CREATE_UNIVERSE_TASKS) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      if (taskType == TaskType.KubernetesCheckNumPod) {
        position++;
        continue;
      }
      JsonNode expectedResults =
          getExpectedCreateUniverseTaskResults().get(position);
      List<JsonNode> taskDetails = tasks.stream()
          .map(t -> t.getTaskDetails())
          .collect(Collectors.toList());
      List<JsonNode> parallelTasks = ImmutableList.of(
        Json.toJson(ImmutableMap.of("commandType", CREATE_NAMESPACE.name())),
        Json.toJson(ImmutableMap.of("commandType", APPLY_SECRET.name())),
        Json.toJson(ImmutableMap.of("commandType", HELM_INSTALL.name())));
      if (parallelTasks.contains(expectedResults)) {
        assertEquals(numTasks, tasks.size());
      } else if (taskType == TaskType.WaitForServer) {
        assertEquals(3, tasks.size());
      } else {
        assertEquals(1, tasks.size());
      }
      assertEquals(taskType, tasks.get(0).getTaskType());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;
    }
  }


  private TaskInfo submitTask(UniverseDefinitionTaskParams taskParams) {
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.nodePrefix = "demo-universe";
    taskParams.expectedUniverseVersion = 2;
    taskParams.clusters = defaultUniverse.getUniverseDetails().clusters;
    taskParams.nodeDetailsSet = defaultUniverse.getUniverseDetails().nodeDetailsSet;

    try {
      UUID taskUUID = commissioner.submit(TaskType.CreateKubernetesUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  @Test
  public void testCreateKubernetesUniverseSuccessMultiAZ() {
    setupUniverseMultiAZ(/* Create Masters */ false);
    setupCommon();
    ShellProcessHandler.ShellResponse response = new ShellProcessHandler.ShellResponse();
    response.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.1\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.2\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}]}";
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(response);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPullSecretFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    TaskInfo taskInfo = submitTask(taskParams);

    verify(mockKubernetesManager, times(3)).createNamespace(expectedConfig.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-1"));
    verify(mockKubernetesManager, times(3)).createNamespace(expectedConfig.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-2"));
    verify(mockKubernetesManager, times(3)).createNamespace(expectedConfig.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-3"));

    verify(mockKubernetesManager, times(3)).helmInstall(expectedConfig.capture(), expectedUniverseUUID.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-1"), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(3)).helmInstall(expectedConfig.capture(), expectedUniverseUUID.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-2"), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(3)).helmInstall(expectedConfig.capture(), expectedUniverseUUID.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-3"), expectedOverrideFile.capture());

    assertEquals(defaultProvider.uuid, expectedUniverseUUID.getValue());
    assertTrue(expectedNodePrefix.getValue().contains(nodePrefix));
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    verify(mockKubernetesManager, times(3)).getPodInfos(expectedConfig.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-1"));
    verify(mockKubernetesManager, times(3)).getPodInfos(expectedConfig.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-2"));
    verify(mockKubernetesManager, times(3)).getPodInfos(expectedConfig.capture(),
        String.format("%s-%s", expectedNodePrefix.capture(), "az-3"));
    verify(mockSwamperHelper, times(1)).writeUniverseTargetJson(defaultUniverse.universeUUID);

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, 3);
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testCreateKubernetesUniverseSuccessSingleAZ() {
    setupUniverse(/* Create Masters */ false);
    setupCommon();
    ShellProcessHandler.ShellResponse response = new ShellProcessHandler.ShellResponse();
    response.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.1\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.2\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.3\"}, \"spec\": {\"hostname\": \"yb-master-1\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.4\"}, \"spec\": {\"hostname\": \"yb-tserver-1\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.5\"}, \"spec\": {\"hostname\": \"yb-master-2\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.6\"}, \"spec\": {\"hostname\": \"yb-tserver-2\"}}]}";
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(response);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPullSecretFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    TaskInfo taskInfo = submitTask(taskParams);

    verify(mockKubernetesManager, times(1)).createNamespace(expectedConfig.capture(),
        expectedNodePrefix.capture());

    verify(mockKubernetesManager, times(1)).helmInstall(expectedConfig.capture(),
        expectedUniverseUUID.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());

    assertEquals(defaultProvider.uuid, expectedUniverseUUID.getValue());
    assertEquals(expectedNodePrefix.getValue(), nodePrefix);
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    verify(mockKubernetesManager, times(1)).getPodInfos(expectedConfig.capture(),
        expectedNodePrefix.capture());
    verify(mockSwamperHelper, times(1)).writeUniverseTargetJson(defaultUniverse.universeUUID);

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, 1);
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testCreateKubernetesUniverseFailure() {
    setupUniverse(/* Create Masters */ true);
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    TaskInfo taskInfo = submitTask(taskParams);
    assertEquals(Failure, taskInfo.getTaskState());
  }
}
