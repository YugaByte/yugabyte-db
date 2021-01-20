// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.google.common.net.HostAndPort;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Ignore;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.YBClient;

import play.libs.Json;

import java.util.*;
import java.util.stream.Collectors;

import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.commissioner.tasks.subtasks.UpdatePlacementInfo.ModifyUniverseConfig;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Mockito.*;

@RunWith(MockitoJUnitRunner.class)
public class RemoveNodeFromUniverseTest extends CommissionerBaseTest {
  public static final Logger LOG = LoggerFactory.getLogger(AddNodeToUniverseTest.class);

  UniverseDefinitionTaskParams.UserIntent userIntent;

  @InjectMocks
  Commissioner commissioner;
  Universe defaultUniverse;
  YBClient mockClient;
  ShellResponse dummyShellResponse;

  public void setUp(boolean withMaster, int numNodes, int replicationFactor, boolean multiZone) {
    super.setUp();
    Region region = Region.create(defaultProvider, "test-region", "Region 1", "yb-image-1");
    AvailabilityZone.create(region, "az-1", "az-1", "subnet-1");
    if (multiZone) {
      AvailabilityZone.create(region, "az-2", "az-2", "subnet-2");
      AvailabilityZone.create(region, "az-3", "az-3", "subnet-3");
    }
    // create default universe
    userIntent = new UniverseDefinitionTaskParams.UserIntent();
    userIntent.numNodes = numNodes;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.replicationFactor = replicationFactor;
    userIntent.regionList = ImmutableList.of(region.uuid);
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, withMaster /* setMasters */));
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);

    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        Set<NodeDetails> nodes = universeDetails.nodeDetailsSet;
        int count = 0;
        int numZones = 1;
        if (multiZone) {
          numZones = 3;
        }
        for (NodeDetails node : nodes) {
          node.cloudInfo.az = "az-" + (count % numZones + 1);
          nodes.add(node);
        }
        universeDetails.nodeDetailsSet = nodes;
        universe.setUniverseDetails(universeDetails);
      }
    };
    Universe.saveDetails(defaultUniverse.universeUUID, updater);
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);

    mockClient = mock(YBClient.class);
    ShellResponse dummyShellResponse = new ShellResponse();
    dummyShellResponse.message = "true";
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);

    try {
      // WaitForTServerHeartBeats mock.
      doNothing().when(mockClient).waitForMasterLeader(anyLong());
    } catch (Exception e) {}


    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    mockWaits(mockClient, 3);
  }

  private TaskInfo submitTask(NodeTaskParams taskParams, String nodeName) {
    taskParams.nodeName = nodeName;
    try {
      UUID taskUUID = commissioner.submit(TaskType.RemoveNodeFromUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  List<TaskType> REMOVE_NODE_TASK_SEQUENCE = ImmutableList.of(
    TaskType.SetNodeState,
    TaskType.UpdatePlacementInfo,
    TaskType.WaitForDataMove,
    TaskType.AnsibleClusterServerCtl,
    TaskType.UpdateNodeProcess,
    TaskType.UpdateNodeProcess,
    TaskType.SetNodeState,
    TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> REMOVE_NODE_TASK_EXPECTED_RESULTS = ImmutableList.of(
    Json.toJson(ImmutableMap.of("state", "Removing")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("process", "tserver", "command", "stop")),
    Json.toJson(ImmutableMap.of("processType", "MASTER", "isAdd", false)),
    Json.toJson(ImmutableMap.of("processType", "TSERVER", "isAdd", false)),
    Json.toJson(ImmutableMap.of("state", "Removed")),
    Json.toJson(ImmutableMap.of())
  );

  List<TaskType> REMOVE_NODE_WITH_MASTER = ImmutableList.of(
    TaskType.SetNodeState,
    TaskType.WaitForMasterLeader,
    TaskType.ChangeMasterConfig,
    TaskType.AnsibleClusterServerCtl,
    TaskType.WaitForMasterLeader,
    TaskType.UpdatePlacementInfo,
    TaskType.WaitForDataMove,
    TaskType.AnsibleClusterServerCtl,
    TaskType.UpdateNodeProcess,
    TaskType.UpdateNodeProcess,
    TaskType.SetNodeState,
    TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> REMOVE_NODE_WITH_MASTER_RESULTS = ImmutableList.of(
    Json.toJson(ImmutableMap.of("state", "Removing")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("process", "master", "command", "stop")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("process", "tserver", "command", "stop")),
    Json.toJson(ImmutableMap.of("processType", "MASTER", "isAdd", false)),
    Json.toJson(ImmutableMap.of("processType", "TSERVER", "isAdd", false)),
    Json.toJson(ImmutableMap.of("state", "Removed")),
    Json.toJson(ImmutableMap.of())
  );

  List<TaskType> REMOVE_NOT_EXISTS_NODE_TASK_SEQUENCE = ImmutableList.of(
    TaskType.SetNodeState,
    TaskType.UpdatePlacementInfo,
    TaskType.UpdateNodeProcess,
    TaskType.UpdateNodeProcess,
    TaskType.SetNodeState,
    TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> REMOVE_NOT_EXISTS_NODE_TASK_EXPECTED_RESULTS = ImmutableList.of(
    Json.toJson(ImmutableMap.of("state", "Removing")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("processType", "MASTER", "isAdd", false)),
    Json.toJson(ImmutableMap.of("processType", "TSERVER", "isAdd", false)),
    Json.toJson(ImmutableMap.of("state", "Removed")),
    Json.toJson(ImmutableMap.of())
  );

  private enum RemoveType {
    WITH_MASTER,
    ONLY_TSERVER,
    NOT_EXISTS
  };

  private void assertRemoveNodeSequence(Map<Integer, List<TaskInfo>> subTasksByPosition,
                                        RemoveType type, boolean moveData) {
    int position = 0;
    int taskPosition = 0;
    switch (type) {
      case WITH_MASTER:
        for (TaskType taskType : REMOVE_NODE_WITH_MASTER) {
          if (taskType.equals(TaskType.WaitForDataMove) && !moveData) {
            position++;
            continue;
          }

          List<TaskInfo> tasks = subTasksByPosition.get(taskPosition);
          assertEquals(1, tasks.size());
          assertEquals(taskType, tasks.get(0).getTaskType());
          JsonNode expectedResults =
              REMOVE_NODE_WITH_MASTER_RESULTS.get(position);
          List<JsonNode> taskDetails = tasks.stream()
              .map(t -> t.getTaskDetails())
              .collect(Collectors.toList());
          assertJsonEqual(expectedResults, taskDetails.get(0));
          position++;
          taskPosition++;
        }
      break;
      case ONLY_TSERVER:
        for (TaskType taskType: REMOVE_NODE_TASK_SEQUENCE) {
          List<TaskInfo> tasks = subTasksByPosition.get(position);
          assertEquals(1, tasks.size());
          assertEquals(taskType, tasks.get(0).getTaskType());
          JsonNode expectedResults =
              REMOVE_NODE_TASK_EXPECTED_RESULTS.get(position);
          List<JsonNode> taskDetails = tasks.stream()
              .map(t -> t.getTaskDetails())
              .collect(Collectors.toList());
          assertJsonEqual(expectedResults, taskDetails.get(0));
          position++;
          taskPosition++;
        }
        break;
      case NOT_EXISTS:
        for (TaskType taskType: REMOVE_NOT_EXISTS_NODE_TASK_SEQUENCE) {
          List<TaskInfo> tasks = subTasksByPosition.get(position);
          assertEquals(1, tasks.size());
          assertEquals(taskType, tasks.get(0).getTaskType());
          JsonNode expectedResults =
              REMOVE_NOT_EXISTS_NODE_TASK_EXPECTED_RESULTS.get(position);
          List<JsonNode> taskDetails = tasks.stream()
              .map(t -> t.getTaskDetails())
              .collect(Collectors.toList());
          assertJsonEqual(expectedResults, taskDetails.get(0));
          position++;
          taskPosition++;
        }
        break;
    }
  }

  @Test
  public void testRemoveNodeSuccess() {
    setUp(false, 4, 3, false);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;

    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertRemoveNodeSequence(subTasksByPosition, RemoveType.ONLY_TSERVER, true);
  }

  @Test
  public void testRemoveNodeWithMaster() {
    setUp(true, 4, 3, false);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertRemoveNodeSequence(subTasksByPosition, RemoveType.WITH_MASTER, true);
  }

  @Test
  public void testRemoveUnknownNode() {
    setUp(false, 4, 3, false);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    TaskInfo taskInfo = submitTask(taskParams, "host-n9");
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }

  @Test
  public void testRemoveNonExistentNode() {
    setUp(false, 4, 3, false);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    dummyShellResponse = new ShellResponse();
    dummyShellResponse.message = null;
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);
    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertRemoveNodeSequence(subTasksByPosition, RemoveType.NOT_EXISTS, true);
  }

  @Test
  public void testRemoveNodeWithNoDataMove() {
    setUp(true, 3, 3, false);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertRemoveNodeSequence(subTasksByPosition, RemoveType.WITH_MASTER, false);
  }

  @Test
  public void testRemoveNodeWithNoDataMoveRF5() {
    setUp(true, 5, 5, true);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertRemoveNodeSequence(subTasksByPosition, RemoveType.WITH_MASTER, false);
  }

  @Test
  public void testRemoveNodeRF5() {
    setUp(true, 6, 5, true);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.expectedUniverseVersion = 3;
    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertRemoveNodeSequence(subTasksByPosition, RemoveType.WITH_MASTER, true);
  }
}
