// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;


import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
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
import play.libs.Json;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;

import static com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType.HELM_DELETE;
import static com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType.VOLUME_DELETE;
import static com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType.NAMESPACE_DELETE;
import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.models.TaskInfo.State.Failure;
import static com.yugabyte.yw.models.TaskInfo.State.Success;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.mockito.Matchers.any;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
public class DestroyKubernetesUniverseTest extends CommissionerBaseTest {

  @InjectMocks
  Commissioner commissioner;

  Universe defaultUniverse;
  String nodePrefix = "demo-universe";

  Map<String, String> config= new HashMap<String, String>();

  private void setupUniverse(boolean updateInProgress) {
    config.put("KUBECONFIG", "test");
    defaultProvider.setConfig(config);
    defaultProvider.save();
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams.UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    userIntent.replicationFactor = 3;
    userIntent.masterGFlags = new HashMap<>();
    userIntent.tserverGFlags = new HashMap<>();
    userIntent.universeName = "demo-universe";

    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, nodePrefix, true /* setMasters */, updateInProgress));
  }

  private void setupUniverseMultiAZ(boolean updateInProgress) {
    config.put("KUBECONFIG", "test");
    defaultProvider.setConfig(config);
    defaultProvider.save();
    Region r = Region.create(defaultProvider, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i = InstanceType.upsert(defaultProvider.code, "c3.xlarge",
        10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams.UserIntent userIntent = getTestUserIntent(r, defaultProvider, i, 3);
    userIntent.replicationFactor = 3;
    userIntent.masterGFlags = new HashMap<>();
    userIntent.tserverGFlags = new HashMap<>();
    userIntent.universeName = "demo-universe";

    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, nodePrefix, true /* setMasters */, updateInProgress));
  }


  List<TaskType> KUBERNETES_DESTROY_UNIVERSE_TASKS = ImmutableList.of(
      TaskType.DestroyEncryptionAtRest,
      TaskType.KubernetesCommandExecutor,
      TaskType.KubernetesCommandExecutor,
      TaskType.RemoveUniverseEntry,
      TaskType.SwamperTargetsFileUpdate);


  List<JsonNode> KUBERNETES_DESTROY_UNIVERSE_EXPECTED_RESULTS = ImmutableList.of(
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("commandType", HELM_DELETE.name())),
      Json.toJson(ImmutableMap.of("commandType", NAMESPACE_DELETE.name())),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
  );

  private void assertTaskSequence(Map<Integer, List<TaskInfo>> subTasksByPosition, int numTasks) {
    int position = 0;
    for (TaskType taskType: KUBERNETES_DESTROY_UNIVERSE_TASKS) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      if (taskType != TaskType.RemoveUniverseEntry &&
          taskType != TaskType.SwamperTargetsFileUpdate &&
          taskType != TaskType.DestroyEncryptionAtRest) {
        assertEquals(numTasks, tasks.size());
      } else {
        assertEquals(1, tasks.size());
      }
      assertEquals(taskType, tasks.get(0).getTaskType());
      JsonNode expectedResults =
          KUBERNETES_DESTROY_UNIVERSE_EXPECTED_RESULTS.get(position);
      List<JsonNode> taskDetails = tasks.stream()
          .map(t -> t.getTaskDetails())
          .collect(Collectors.toList());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;
    }
  }

  private TaskInfo submitTask(DestroyUniverse.Params taskParams) {
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 2;
    try {
      UUID taskUUID = commissioner.submit(TaskType.DestroyKubernetesUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  @Test
  public void testDestroyKubernetesUniverseSuccess() {
    setupUniverse(false);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
    ShellResponse response = new ShellResponse();
    when(mockKubernetesManager.helmDelete(any(), any())).thenReturn(response);
    DestroyUniverse.Params taskParams = new DestroyUniverse.Params();
    taskParams.isForceDelete = false;
    taskParams.customerUUID = defaultCustomer.uuid;
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams);
    verify(mockKubernetesManager, times(1)).helmDelete(config, nodePrefix);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix);
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, 1);
    assertEquals(Success, taskInfo.getTaskState());
    assertFalse(defaultCustomer.getUniverseUUIDs().contains(defaultUniverse.universeUUID));
  }

  @Test
  public void testDestoryKubernetesUniverseWithUpdateInProgress() {
    setupUniverse(true);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
    DestroyUniverse.Params taskParams = new DestroyUniverse.Params();
    taskParams.isForceDelete = false;
    taskParams.customerUUID = defaultCustomer.uuid;
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams);
    assertEquals(Failure, taskInfo.getTaskState());
  }

  @Test
  public void testForceDestroyKubernetesUniverseWithUpdateInProgress() {
    setupUniverse(true);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
    DestroyUniverse.Params taskParams = new DestroyUniverse.Params();
    taskParams.isForceDelete = true;
    taskParams.customerUUID = defaultCustomer.uuid;
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams);
    verify(mockKubernetesManager, times(1)).helmDelete(config, nodePrefix);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix);
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, 1);
    assertEquals(Success, taskInfo.getTaskState());
    assertFalse(defaultCustomer.getUniverseUUIDs().contains(defaultUniverse.universeUUID));
  }

  @Test
  public void testDestroyKubernetesUniverseSuccessMultiAZ() {
    setupUniverseMultiAZ(false);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
    ShellResponse response = new ShellResponse();
    when(mockKubernetesManager.helmDelete(any(), any())).thenReturn(response);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);

    DestroyUniverse.Params taskParams = new DestroyUniverse.Params();
    taskParams.isForceDelete = false;
    taskParams.customerUUID = defaultCustomer.uuid;
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams);
    String nodePrefix1 = String.format("%s-%s", nodePrefix, "az-1");
    String nodePrefix2 = String.format("%s-%s", nodePrefix, "az-2");
    String nodePrefix3 = String.format("%s-%s", nodePrefix, "az-3");
    verify(mockKubernetesManager, times(1)).helmDelete(config, nodePrefix1);
    verify(mockKubernetesManager, times(1)).helmDelete(config, nodePrefix2);
    verify(mockKubernetesManager, times(1)).helmDelete(config, nodePrefix3);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix1);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix2);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix3);
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, 3);
    assertEquals(Success, taskInfo.getTaskState());
    assertFalse(defaultCustomer.getUniverseUUIDs().contains(defaultUniverse.universeUUID));
  }

  @Test
  public void testDestroyKubernetesHelm2UniverseSuccess() {
    setupUniverseMultiAZ(false);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V2TO3.toString()));
    ShellResponse response = new ShellResponse();
    when(mockKubernetesManager.helmDelete(any(), any())).thenReturn(response);

    ArgumentCaptor<UUID> expectedUniverseUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);

    DestroyUniverse.Params taskParams = new DestroyUniverse.Params();
    taskParams.isForceDelete = false;
    taskParams.customerUUID = defaultCustomer.uuid;
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams);
    String nodePrefix1 = String.format("%s-%s", nodePrefix, "az-1");
    String nodePrefix2 = String.format("%s-%s", nodePrefix, "az-2");
    String nodePrefix3 = String.format("%s-%s", nodePrefix, "az-3");
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix1);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix2);
    verify(mockKubernetesManager, times(1)).deleteNamespace(config, nodePrefix3);
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertTaskSequence(subTasksByPosition, 3);
    assertEquals(Success, taskInfo.getTaskState());
    assertFalse(defaultCustomer.getUniverseUUIDs().contains(defaultUniverse.universeUUID));
  }
}
