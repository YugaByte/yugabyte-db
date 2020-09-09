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
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Ignore;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;

import org.yb.client.YBClient;
import org.yb.client.ModifyMasterClusterConfigBlacklist;

import play.libs.Json;

import java.util.*;
import java.util.stream.Collectors;

import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Mockito.*;

@RunWith(MockitoJUnitRunner.class)
public class ReleaseInstanceFromUniverseTest extends CommissionerBaseTest {
  @InjectMocks
  Commissioner commissioner;
  Universe defaultUniverse;
  YBClient mockClient;
  ModifyMasterClusterConfigBlacklist modifyBL;
  ShellProcessHandler.ShellResponse dummyShellResponse;

  static final String DEFAULT_NODE_NAME = "host-n1";

  private void setDefaultNodeState(final NodeState desiredState) {
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        Set<NodeDetails> nodes = universeDetails.nodeDetailsSet;
        for (NodeDetails node : nodes) {
          if (node.nodeName.equals(DEFAULT_NODE_NAME)) {
            node.state = desiredState;
            break;
          }
        }
        universe.setUniverseDetails(universeDetails);
      }
    };
    Universe.saveDetails(defaultUniverse.universeUUID, updater);
  }

  @Before
  public void setUp() {
    super.setUp();
    Region region = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    AvailabilityZone.create(region, "az-1", "AZ 1", "subnet-1");
    UniverseDefinitionTaskParams.UserIntent userIntent;
    // create default universe
    userIntent = new UniverseDefinitionTaskParams.UserIntent();
    userIntent.numNodes = 3;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.replicationFactor = 3;
    userIntent.regionList = ImmutableList.of(region.uuid);
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, false /* setMasters */));

    setDefaultNodeState(NodeState.Removed);

    mockClient = mock(YBClient.class);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    when(mockNodeManager.nodeCommand(any(), any()))
        .thenReturn(new ShellProcessHandler.ShellResponse());
    modifyBL = mock(ModifyMasterClusterConfigBlacklist.class);
    dummyShellResponse =  new ShellProcessHandler.ShellResponse();
    dummyShellResponse.message = "true";
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);
  }

  private TaskInfo submitTask(NodeTaskParams taskParams, String nodeName, int version) {
    taskParams.expectedUniverseVersion = version;
    taskParams.nodeName = nodeName;
    try {
      UUID taskUUID = commissioner.submit(TaskType.ReleaseInstanceFromUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  List<TaskType> RELEASE_INSTANCE_TASK_SEQUENCE = ImmutableList.of(
    TaskType.SetNodeState,
    TaskType.WaitForMasterLeader,
    TaskType.ModifyBlackList,
    TaskType.AnsibleDestroyServer,
    TaskType.SetNodeState,
    TaskType.SwamperTargetsFileUpdate,
    TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> RELEASE_INSTANCE_TASK_EXPECTED_RESULTS = ImmutableList.of(
    Json.toJson(ImmutableMap.of("state", "BeingDecommissioned")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("state", "Decommissioned")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of())
  );

  private void assertReleaseInstanceSequence(Map<Integer, List<TaskInfo>> subTasksByPosition) {
    int position = 0;
    for (TaskType taskType: RELEASE_INSTANCE_TASK_SEQUENCE) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      assertEquals(1, tasks.size());
      assertEquals(taskType, tasks.get(0).getTaskType());
      JsonNode expectedResults = RELEASE_INSTANCE_TASK_EXPECTED_RESULTS.get(position);
      List<JsonNode> taskDetails = tasks.stream()
          .map(t -> t.getTaskDetails())
          .collect(Collectors.toList());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;
    }
  }

  @Test
  public void testReleaseInstanceSuccess() {
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;

    TaskInfo taskInfo = submitTask(taskParams, DEFAULT_NODE_NAME, 3);
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertEquals(subTasksByPosition.size(), RELEASE_INSTANCE_TASK_SEQUENCE.size());
    assertReleaseInstanceSequence(subTasksByPosition);
  }

  @Test
  public void testReleaseStoppedInstanceSuccess() {
    setDefaultNodeState(NodeState.Stopped);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;

    TaskInfo taskInfo = submitTask(taskParams, DEFAULT_NODE_NAME, 4);
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertEquals(subTasksByPosition.size(), RELEASE_INSTANCE_TASK_SEQUENCE.size());
    assertReleaseInstanceSequence(subTasksByPosition);
  }

  @Test
  public void testReleaseUnknownNode() {
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams, "host-n9", 3);
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }
}
