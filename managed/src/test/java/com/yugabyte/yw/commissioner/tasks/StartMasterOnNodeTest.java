// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.junit.Assert.*;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import static org.mockito.Mockito.mock;
import org.mockito.runners.MockitoJUnitRunner;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.*;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;

import org.yb.client.YBClient;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.master.Master;

import play.libs.Json;

@RunWith(MockitoJUnitRunner.class)
public class StartMasterOnNodeTest extends CommissionerBaseTest {

  @InjectMocks
  Commissioner commissioner;
  Universe defaultUniverse;
  ShellResponse dummyShellResponse;
  YBClient mockClient;

  @Before
  public void setUp() {
    super.setUp();
    Region region = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    AvailabilityZone.create(region, "az-1", "AZ 1", "subnet-1");
    // create default universe
    UniverseDefinitionTaskParams.UserIntent userIntent =
        new UniverseDefinitionTaskParams.UserIntent();
    userIntent.numNodes = 3;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.regionList = ImmutableList.of(region.uuid);
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, true /* setMasters */));

    Map<String, String> gflags = new HashMap<>();
    gflags.put("foo", "bar");
    defaultUniverse.getUniverseDetails().getPrimaryCluster().userIntent.masterGFlags = gflags;

    Master.SysClusterConfigEntryPB.Builder configBuilder =
      Master.SysClusterConfigEntryPB.newBuilder().setVersion(2);
    GetMasterClusterConfigResponse mockConfigResponse =
      new GetMasterClusterConfigResponse(1111, "", configBuilder.build(), null);

    mockClient = mock(YBClient.class);
    try {
      when(mockClient.getMasterClusterConfig()).thenReturn(mockConfigResponse);
    } catch (Exception e) {}

    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);

    dummyShellResponse = new ShellResponse();
    dummyShellResponse.message = "true";
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);
  }

  private TaskInfo submitTask(NodeTaskParams taskParams, String nodeName) {
    taskParams.clusters.addAll(Universe.get(taskParams.universeUUID).getUniverseDetails().clusters);
    taskParams.expectedUniverseVersion = 2;
    taskParams.nodeName = nodeName;
    try {
      UUID taskUUID = commissioner.submit(TaskType.StartMasterOnNode, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  // @formatter:off
  List<TaskType> START_MASTER_TASK_SEQUENCE = ImmutableList.of(
          TaskType.SetNodeState,
          TaskType.AnsibleConfigureServers,
          TaskType.AnsibleClusterServerCtl,
          TaskType.UpdateNodeProcess,
          TaskType.WaitForServer,
          TaskType.ChangeMasterConfig,
          TaskType.AnsibleConfigureServers,
          TaskType.SetFlagInMemory,
          TaskType.AnsibleConfigureServers,
          TaskType.SetFlagInMemory,
          TaskType.SetNodeState,
          TaskType.SwamperTargetsFileUpdate,
          TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> START_MASTER_TASK_EXPECTED_RESULTS = ImmutableList.of(
          Json.toJson(ImmutableMap.of("state", "Starting")),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of("process", "master", "command", "start")),
          Json.toJson(ImmutableMap.of("processType", "MASTER", "isAdd", true)),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of("state", "Live")),
          Json.toJson(ImmutableMap.of()),
          Json.toJson(ImmutableMap.of()));
  // @formatter:on

  private void assertStartMasterSequence(Map<Integer, List<TaskInfo>> subTasksByPosition) {
    int position = 0;
    for (TaskType taskType : START_MASTER_TASK_SEQUENCE) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      // assertEquals(1, tasks.size());
      assertEquals("At position: " + position, taskType, tasks.get(0).getTaskType());
      JsonNode expectedResults = START_MASTER_TASK_EXPECTED_RESULTS.get(position);
      List<JsonNode> taskDetails = tasks.stream().map(t -> t.getTaskDetails())
          .collect(Collectors.toList());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;
    }
  }

  @Test
  public void testStartMasterOnNodeIfUnderReplicatedMasterAndNodeIsLive() {
    Universe universe = createUniverse("Demo");
    universe = Universe.saveDetails(universe.universeUUID,
        ApiUtils.mockUniverseUpdaterWithInactiveNodes());
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = universe.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams, "host-n2");
    verify(mockNodeManager, times(3)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition = subTasks.stream()
        .collect(Collectors.groupingBy(w -> w.getPosition()));
    assertEquals(START_MASTER_TASK_SEQUENCE.size(), subTasksByPosition.size());
    assertStartMasterSequence(subTasksByPosition);
  }

  @Test
  public void testStartMasterOnNodeIfNodeIsUnknown() {
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams, "host-n9");
    verify(mockNodeManager, times(0)).nodeCommand(any(), any());
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }

  @Test
  public void testStartMasterOnNodeIfAlreadyMaster() {
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    // one nodeCommand invocation is made from instanceExists()
    verify(mockNodeManager, times(1)).nodeCommand(any(), any());
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }

  @Test
  public void testStartMasterOnNodeIfUnderReplicatedMasterAndNodeIsRemoved() {
    Universe universe = createUniverse("DemoX");
    universe = Universe.saveDetails(universe.universeUUID,
        ApiUtils.mockUniverseUpdaterWithInactiveNodes());
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = universe.universeUUID;
    // Node "host-n4" is in Removed state already.
    TaskInfo taskInfo = submitTask(taskParams, "host-n4");
    // one nodeCommand invocation is made from instanceExists()
    verify(mockNodeManager, times(1)).nodeCommand(any(), any());
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }

  @Test
  public void testStartMasterOnNodeIfNodeInReadOnlyCluster() {
    Universe universe = createUniverse("DemoX");
    universe = Universe.saveDetails(universe.universeUUID,
        ApiUtils.mockUniverseUpdaterWithInactiveAndReadReplicaNodes(false, 3));

    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = universe.universeUUID;

    // Node "yb-tserver-0" is in Read Only cluster.
    TaskInfo taskInfo = submitTask(taskParams, "yb-tserver-0");
    // one nodeCommand invocation is made from instanceExists()
    verify(mockNodeManager, times(1)).nodeCommand(any(), any());
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }
}
