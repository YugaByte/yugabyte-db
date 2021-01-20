// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import java.io.IOException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.google.common.net.HostAndPort;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.RegexMatcher;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.CloudSpecificInfo;
import com.yugabyte.yw.models.helpers.TaskType;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.commissioner.tasks.subtasks.WaitForServerReady;
import org.junit.Test;
import org.junit.Ignore;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import org.yb.Common;
import org.yb.client.YBClient;
import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.client.IsServerReadyResponse;
import org.yb.client.GetLoadMovePercentResponse;
import play.libs.Json;

import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.HashSet;
import java.util.Map;
import java.util.UUID;
import java.util.Set;
import java.util.stream.Collectors;

import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCommandExecutor.CommandType.HELM_UPGRADE;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCommandExecutor.CommandType.POD_INFO;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesCheckNumPod.CommandType.WAIT_FOR_PODS;
import static com.yugabyte.yw.commissioner.tasks.subtasks
                 .KubernetesWaitForPod.CommandType.WAIT_FOR_POD;
import static com.yugabyte.yw.commissioner.tasks.subtasks.UpdatePlacementInfo.ModifyUniverseConfig;

import static com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType.MASTER;
import static com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType.TSERVER;

import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.models.TaskInfo.State.Failure;
import static com.yugabyte.yw.models.TaskInfo.State.Success;

import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyBoolean;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Matchers.anyInt;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import org.yb.client.ModifyMasterClusterConfigBlacklist;


@RunWith(MockitoJUnitRunner.class)
public class EditKubernetesUniverseTest extends CommissionerBaseTest {

  @InjectMocks
  Commissioner commissioner;

  @InjectMocks
  EditKubernetesUniverse editUniverse;

  Universe defaultUniverse;
  YBClient mockClient;
  ShellResponse dummyShellResponse;

  String nodePrefix = "demo-universe";

  Map<String, String> config= new HashMap<String, String>();

  private void setup() {
    ShellResponse responseEmpty = new ShellResponse();
    ShellResponse responsePod = new ShellResponse();
    when(mockKubernetesManager.helmUpgrade(any(), any(), any())).thenReturn(responseEmpty);
    responsePod.message =
        "{\"status\": { \"phase\": \"Running\", \"conditions\": [{\"status\": \"True\"}]}}";
    when(mockKubernetesManager.getPodStatus(any(), any(), any())).thenReturn(responsePod);

    mockClient = mock(YBClient.class);
    IsServerReadyResponse okReadyResp = new IsServerReadyResponse(0, "", null, 0, 0);
    ChangeMasterClusterConfigResponse ccr = new ChangeMasterClusterConfigResponse(1111, "", null);
    try {
      when(mockClient.changeMasterClusterConfig(any())).thenReturn(ccr);
      when(mockClient.isServerReady(any(), anyBoolean())).thenReturn(okReadyResp);
    } catch (Exception ex) {}
    mockWaits(mockClient, 3);
    GetLoadMovePercentResponse gpr = new GetLoadMovePercentResponse(0, "", 100.0, 0, 0, null);
    try {
      when(mockClient.getLoadMoveCompletion()).thenReturn(gpr);
    } catch (Exception e) {}
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);
    when(mockClient.waitForLoadBalance(anyLong(), anyInt())).thenReturn(true);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
  }

  private void setupUniverseSingleAZ(boolean setMasters) {
    setup();
    config.put("KUBECONFIG", "test");
    defaultProvider.setConfig(config);
    defaultProvider.save();
    editUniverse.setUserTaskUUID(UUID.randomUUID());
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams.UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    userIntent.replicationFactor = 1;
    userIntent.masterGFlags = new HashMap<>();
    userIntent.tserverGFlags = new HashMap<>();
    userIntent.universeName = "demo-universe";
    userIntent.ybSoftwareVersion = "old-version";
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, nodePrefix, setMasters /* setMasters */));
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdaterWithActivePods(1, 3));
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
  }

  List<TaskType> KUBERNETES_ADD_POD_TASKS = ImmutableList.of(
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCheckNumPod,
      TaskType.KubernetesCommandExecutor,
      TaskType.WaitForServer,
      TaskType.UpdatePlacementInfo,
      TaskType.WaitForLoadBalance,
      TaskType.KubernetesCommandExecutor,
      TaskType.SwamperTargetsFileUpdate,
      TaskType.UniverseUpdateSucceeded);

  List<JsonNode> getExpectedAddPodTaskResults() {
    return ImmutableList.of(
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_PODS.name())),
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
    );
  }

  List<TaskType> KUBERNETES_REMOVE_POD_TASKS = ImmutableList.of(
      TaskType.UpdatePlacementInfo,
      TaskType.WaitForDataMove,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCheckNumPod,
      TaskType.ModifyBlackList,
      TaskType.KubernetesCommandExecutor,
      TaskType.SwamperTargetsFileUpdate,
      TaskType.UniverseUpdateSucceeded);

  List<JsonNode> getExpectedRemovePodTaskResults() {
    return ImmutableList.of(
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_UPGRADE.name())),
      Json.toJson(ImmutableMap.of("commandType", WAIT_FOR_PODS.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
    );
  }

  List<TaskType> KUBERNETES_CHANGE_INSTANCE_TYPE_TASKS = ImmutableList.of(
      TaskType.UpdatePlacementInfo,
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
      TaskType.KubernetesCommandExecutor,
      TaskType.SwamperTargetsFileUpdate,
      TaskType.UniverseUpdateSucceeded);

  List<JsonNode> getExpectedChangeInstaceTypeResults() {
    return ImmutableList.of(
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
      Json.toJson(ImmutableMap.of("commandType", POD_INFO.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
    );
  }

  private void assertTaskSequence(Map<Integer, List<TaskInfo>> subTasksByPosition,
                                  List<TaskType> taskList,
                                  List<JsonNode> resultList,
                                  String type) {
    int position = 0;
    // Since we create two empty subGroupTasks for add (namespace create and apply secret)
    // they need to skipped over.
    if (type.equals("add")) {
      position = 2;
    }
    for (TaskType task: taskList) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      // In the case of adding and wait for server, we need to ensure that
      // there are two waits queued (one for each added server).
      if (task == TaskType.WaitForServer && type.equals("add")) {
        assertEquals(2, tasks.size());
      } else {
        assertEquals(1, tasks.size());
      }

      assertEquals(task, tasks.get(0).getTaskType());
      JsonNode expectedResults = resultList.get(position);
      List<JsonNode> taskDetails = tasks.stream()
          .map(TaskInfo::getTaskDetails)
          .collect(Collectors.toList());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;

      // Similar to add, we expect two empty subGroupTasks (namespace and volume delete).
      if (position == 3 && type.equals("remove")) {
        position += 2;
      }
    }
  }

  private TaskInfo submitTask(UniverseDefinitionTaskParams taskParams,
                              UniverseDefinitionTaskParams.UserIntent userIntent,
                              PlacementInfo pi) {
    taskParams.upsertPrimaryCluster(userIntent, pi);
    taskParams.nodePrefix = nodePrefix;

    try {
      UUID taskUUID = commissioner.submit(TaskType.EditKubernetesUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  public JsonNode parseShellResponseAsJson(ShellResponse response) {
    ObjectMapper mapper = new ObjectMapper();
    try {
      return mapper.readTree(response.message);
    } catch (IOException e) {
      throw new RuntimeException("Shell Response message is not a valid Json.");
    }
  }

  @Test
  public void testAddNode() {
    setupUniverseSingleAZ(/* Create Masters */ true);

    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";

    // After changing to 5 tservers.
    ShellResponse responsePods = new ShellResponse();
    responsePods.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.1\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.2\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.3\"}, \"spec\": {\"hostname\": \"yb-tserver-1\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.4\"}, \"spec\": {\"hostname\": \"yb-tserver-2\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.5\"}, \"spec\": {\"hostname\": \"yb-tserver-3\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.6\"}, \"spec\": {\"hostname\": \"yb-tserver-4\"}}]}";
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(responsePods);

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    taskParams.nodeDetailsSet = defaultUniverse.getUniverseDetails().nodeDetailsSet;
    UniverseDefinitionTaskParams.UserIntent newUserIntent =
        defaultUniverse.getUniverseDetails().getPrimaryCluster().userIntent.clone();
    newUserIntent.numNodes = 5;
    PlacementInfo pi = defaultUniverse.getUniverseDetails().getPrimaryCluster().placementInfo;
    pi.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ = 5;
    TaskInfo taskInfo = submitTask(taskParams, newUserIntent, pi);

    verify(mockKubernetesManager, times(1)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(3)).getPodInfos(expectedConfig.capture(),
        expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertEquals(nodePrefix, expectedNodePrefix.getValue());
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, KUBERNETES_ADD_POD_TASKS,
        getExpectedAddPodTaskResults(), "add");
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testRemoveNode() {
    setupUniverseSingleAZ(/* Create Masters */ true);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPodName = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    ShellResponse responsePods = new ShellResponse();
    responsePods.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.1\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.2\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.3\"}, \"spec\": {\"hostname\": \"yb-tserver-1\"}}]}";
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(responsePods);

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    taskParams.nodeDetailsSet = defaultUniverse.getUniverseDetails().nodeDetailsSet;
    UniverseDefinitionTaskParams.UserIntent newUserIntent =
        defaultUniverse.getUniverseDetails().getPrimaryCluster().userIntent.clone();
    newUserIntent.numNodes = 2;
    PlacementInfo pi = defaultUniverse.getUniverseDetails().getPrimaryCluster().placementInfo;
    pi.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ = 2;
    TaskInfo taskInfo = submitTask(taskParams, newUserIntent, pi);

    verify(mockKubernetesManager, times(1)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(2)).getPodInfos(expectedConfig.capture(),
                                                        expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertEquals(nodePrefix, expectedNodePrefix.getValue());
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, KUBERNETES_REMOVE_POD_TASKS,
        getExpectedRemovePodTaskResults(), "remove");
    assertEquals(Success, taskInfo.getTaskState());
  }

  @Test
  public void testChangeInstanceType() {
    setupUniverseSingleAZ(/* Create Masters */ true);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedPodName = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);

    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    ShellResponse responsePods = new ShellResponse();
    responsePods.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.1\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.2\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.3\"}, \"spec\": {\"hostname\": \"yb-tserver-1\"}}," +
            "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"1.2.3.4\"}, \"spec\": {\"hostname\": \"yb-tserver-2\"}}]}";
    when(mockKubernetesManager.getPodInfos(any(), any())).thenReturn(responsePods);

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    taskParams.nodeDetailsSet = defaultUniverse.getUniverseDetails().nodeDetailsSet;
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c5.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams.UserIntent newUserIntent =
        defaultUniverse.getUniverseDetails().getPrimaryCluster().userIntent.clone();
    newUserIntent.instanceType = "c5.xlarge";
    PlacementInfo pi = defaultUniverse.getUniverseDetails().getPrimaryCluster().placementInfo;
    TaskInfo taskInfo = submitTask(taskParams, newUserIntent, pi);

    verify(mockKubernetesManager, times(3)).helmUpgrade(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedOverrideFile.capture());
    verify(mockKubernetesManager, times(3)).getPodStatus(expectedConfig.capture(),
        expectedNodePrefix.capture(), expectedPodName.capture());
    verify(mockKubernetesManager, times(1)).getPodInfos(expectedConfig.capture(),
                                                        expectedNodePrefix.capture());

    assertEquals(config, expectedConfig.getValue());
    assertEquals(nodePrefix, expectedNodePrefix.getValue());
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, KUBERNETES_CHANGE_INSTANCE_TYPE_TASKS,
        getExpectedChangeInstaceTypeResults(), "change");
    assertEquals(Success, taskInfo.getTaskState());
  }
}
