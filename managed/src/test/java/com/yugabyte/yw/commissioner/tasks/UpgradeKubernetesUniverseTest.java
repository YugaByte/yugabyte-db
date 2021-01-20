// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.UpgradeUniverse;
import com.yugabyte.yw.commissioner.tasks.UpgradeUniverse.UpgradeTaskType;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.RegexMatcher;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
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
import org.yb.client.IsServerReadyResponse;
import org.yb.client.YBClient;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.master.Master;
import play.libs.Json;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;

import static com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType.HELM_UPGRADE;
import static com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType.POD_INFO;
import static com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesWaitForPod.CommandType.WAIT_FOR_POD;

import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.models.TaskInfo.State.Success;

import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyBoolean;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
public class UpgradeKubernetesUniverseTest extends CommissionerBaseTest {

  @InjectMocks
  Commissioner commissioner;

  @InjectMocks
  UpgradeKubernetesUniverse upgradeUniverse;

  Universe defaultUniverse;
  YBClient mockClient;
  ShellResponse dummyShellResponse;

  String nodePrefix = "demo-universe";

  Map<String, String> config= new HashMap<String, String>();

  private void setupUniverse(boolean setMasters, UserIntent userIntent) {
    upgradeUniverse.setUserTaskUUID(UUID.randomUUID());
    userIntent.replicationFactor = 3;
    userIntent.masterGFlags = new HashMap<>();
    userIntent.tserverGFlags = new HashMap<>();
    userIntent.universeName = "demo-universe";
    userIntent.ybSoftwareVersion = "old-version";
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    config.put("KUBECONFIG", "test");
    defaultProvider.setConfig(config);
    defaultProvider.save();
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, nodePrefix, setMasters /* setMasters */));
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));

    ShellResponse responseEmpty = new ShellResponse();
    ShellResponse responsePod = new ShellResponse();
    when(mockKubernetesManager.helmUpgrade(any(), any(), any())).thenReturn(responseEmpty);

    Master.SysClusterConfigEntryPB.Builder configBuilder =
      Master.SysClusterConfigEntryPB.newBuilder().setVersion(2);
    GetMasterClusterConfigResponse mockConfigResponse =
      new GetMasterClusterConfigResponse(1111, "", configBuilder.build(), null);

    responsePod.message =
        "{\"status\": { \"phase\": \"Running\", \"conditions\": [{\"status\": \"True\"}]}}";
    when(mockKubernetesManager.getPodStatus(any(), any(), any())).thenReturn(responsePod);

    mockClient = mock(YBClient.class);
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);
    IsServerReadyResponse okReadyResp = new IsServerReadyResponse(0, "", null, 0, 0);
    try {
      when(mockClient.getMasterClusterConfig()).thenReturn(mockConfigResponse);
      when(mockClient.isServerReady(any(), anyBoolean())).thenReturn(okReadyResp);
    } catch (Exception ex) {}
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);

  }

  private void setupUniverseSingleAZ(boolean setMasters) {
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion-1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ-1", "subnet-1");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    setupUniverse(setMasters, userIntent);
    ShellResponse responsePods = new ShellResponse();
    responsePods.message =
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
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(responsePods);
  }

  private void setupUniverseMultiAZ(boolean setMasters) {
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion-1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ-1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ-2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ-3", "subnet-3");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    setupUniverse(setMasters, userIntent);
    ShellResponse responsePods = new ShellResponse();
    responsePods.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.1\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.2\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}]}";
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(responsePods);
  }

  List<TaskType> KUBERNETES_UPGRADE_SOFTWARE_TASKS = ImmutableList.of(
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.LoadBalancerStateChange,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.LoadBalancerStateChange,
      TaskType.UpdateSoftwareVersion,
      TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> KUBERNETES_UPGRADE_SOFTWARE_RESULTS = ImmutableList.of(
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name(),
                                  "ybSoftwareVersion", "new-version")),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name(),
                                  "ybSoftwareVersion", "new-version")),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name(),
                                  "ybSoftwareVersion", "new-version")),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name(),
                                  "ybSoftwareVersion", "new-version")),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name(),
                                  "ybSoftwareVersion", "new-version")),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name(),
                                  "ybSoftwareVersion", "new-version")),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
  );

  List<TaskType> KUBERNETES_UPGRADE_GFLAG_TASKS = ImmutableList.of(
      TaskType.UpdateAndPersistGFlags,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.LoadBalancerStateChange,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesWaitForPod,
      TaskType.WaitForServer,
      TaskType.WaitForServerReady,
      TaskType.LoadBalancerStateChange,
      TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> KUBERNETES_UPGRADE_GFLAG_RESULTS = ImmutableList.of(
      Json.toJson(ImmutableMap.of("masterGFlags", Json.parse("{\"master-flag\":\"m1\"}"),
                                  "tserverGFlags", Json.parse("{\"tserver-flag\":\"t1\"}"))),
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_POD.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
    );

  private void assertTaskSequence(Map<Integer, List<TaskInfo>> subTasksByPosition,
                                  UpgradeTaskType taskType) {
    int position = 0;
    List<TaskType> taskList = taskType == UpgradeTaskType.Software ?
        KUBERNETES_UPGRADE_SOFTWARE_TASKS : KUBERNETES_UPGRADE_GFLAG_TASKS;
    for (TaskType task: taskList) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      assertEquals(1, tasks.size());
      assertEquals(task, tasks.get(0).getTaskType());
      JsonNode expectedResults = taskType == UpgradeTaskType.Software ?
          KUBERNETES_UPGRADE_SOFTWARE_RESULTS.get(position) : KUBERNETES_UPGRADE_GFLAG_RESULTS.get(position);
      List<JsonNode> taskDetails = tasks.stream()
          .map(t -> t.getTaskDetails())
          .collect(Collectors.toList());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;
    }
  }

  private TaskInfo submitTask(UpgradeKubernetesUniverse.Params taskParams,
                              UpgradeTaskType taskType) {
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.taskType = taskType;
    taskParams.clusters = defaultUniverse.getUniverseDetails().clusters;
    taskParams.nodePrefix = nodePrefix;
    taskParams.expectedUniverseVersion = 2;
    // Need not sleep for default 4min in tests.
    taskParams.sleepAfterMasterRestartMillis = 5;
    taskParams.sleepAfterTServerRestartMillis = 5;

    try {
      UUID taskUUID = commissioner.submit(TaskType.UpgradeKubernetesUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  @Test
  public void testSoftwareUpgradeSingleAZ() {
    setupUniverseSingleAZ(/* Create Masters */ false);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPodName = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";

    UpgradeKubernetesUniverse.Params taskParams = new UpgradeKubernetesUniverse.Params();
    taskParams.ybSoftwareVersion = "new-version";
    TaskInfo taskInfo = submitTask(taskParams, UpgradeTaskType.Software);

    verify(mockKubernetesManager, times(6)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(6)).getPodStatus(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedPodName.capture());
    verify(mockKubernetesManager, times(1)).getPodInfos(expectedConfig.capture(), expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertEquals(nodePrefix, expectedNodePrefix.getValue());
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, UpgradeTaskType.Software);
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testGFlagUpgradeSingleAZ() {
    setupUniverseSingleAZ(/* Create Masters */ false);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPodName = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";

    UpgradeKubernetesUniverse.Params taskParams = new UpgradeKubernetesUniverse.Params();
    taskParams.masterGFlags = ImmutableMap.of("master-flag", "m1");
    taskParams.tserverGFlags = ImmutableMap.of("tserver-flag", "t1");
    TaskInfo taskInfo = submitTask(taskParams, UpgradeTaskType.GFlags);

    verify(mockKubernetesManager, times(6)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(6)).getPodStatus(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedPodName.capture());
    verify(mockKubernetesManager, times(1)).getPodInfos(expectedConfig.capture(), expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertEquals(nodePrefix, expectedNodePrefix.getValue());
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, UpgradeTaskType.GFlags);
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testSoftwareUpgradeMultiAZ() {
    setupUniverseMultiAZ(/* Create Masters */ false);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPodName = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";

    UpgradeKubernetesUniverse.Params taskParams = new UpgradeKubernetesUniverse.Params();
    taskParams.ybSoftwareVersion = "new-version";
    TaskInfo taskInfo = submitTask(taskParams, UpgradeTaskType.Software);

    verify(mockKubernetesManager, times(6)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(6)).getPodStatus(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedPodName.capture());
    verify(mockKubernetesManager, times(3)).getPodInfos(expectedConfig.capture(), expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertTrue(expectedNodePrefix.getValue().contains(nodePrefix));
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, UpgradeTaskType.Software);
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testGFlagUpgradeMultiAZ() {
    setupUniverseMultiAZ(/* Create Masters */ false);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPodName = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";

    UpgradeKubernetesUniverse.Params taskParams = new UpgradeKubernetesUniverse.Params();
    taskParams.masterGFlags = ImmutableMap.of("master-flag", "m1");
    taskParams.tserverGFlags = ImmutableMap.of("tserver-flag", "t1");
    TaskInfo taskInfo = submitTask(taskParams, UpgradeTaskType.GFlags);

    verify(mockKubernetesManager, times(6)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(6)).getPodStatus(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedPodName.capture());
    verify(mockKubernetesManager, times(3)).getPodInfos(expectedConfig.capture(), expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertTrue(expectedNodePrefix.getValue().contains(nodePrefix));
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, UpgradeTaskType.GFlags);
    assertEquals(Success, taskInfo.getTaskState());
  }
}
