// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.common.NodeManager.NodeCommandType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import org.yb.client.YBClient;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.libs.Json;

import java.util.*;
import java.util.function.Consumer;
import java.util.stream.Collectors;

import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.master.Master;
import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyBoolean;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Matchers.anyString;
import static org.mockito.Mockito.*;
import org.yb.client.ModifyMasterClusterConfigBlacklist;

@RunWith(MockitoJUnitRunner.class)
public class AddNodeToUniverseTest extends CommissionerBaseTest {
  public static final Logger LOG = LoggerFactory.getLogger(AddNodeToUniverseTest.class);

  @InjectMocks
  Commissioner commissioner;
  Universe defaultUniverse;
  ShellResponse dummyShellResponse;
  ShellResponse preflightSuccess;
  YBClient mockClient;
  ModifyMasterClusterConfigBlacklist modifyBL;

  private static final String DEFAULT_NODE_NAME = "host-n1";

  private static final String AZ_CODE = "az-1";

  @Before
  public void setUp() {
    super.setUp();
    ChangeMasterClusterConfigResponse ccr = new ChangeMasterClusterConfigResponse(1111, "", null);
    Master.SysClusterConfigEntryPB.Builder configBuilder =
      Master.SysClusterConfigEntryPB.newBuilder().setVersion(1);
    GetMasterClusterConfigResponse mockConfigResponse =
      new GetMasterClusterConfigResponse(1111, "", configBuilder.build(), null);
    Region region = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    AvailabilityZone.create(region, AZ_CODE, "AZ 1", "subnet-1");
    // create default universe
    UserIntent userIntent = new UserIntent();
    userIntent.numNodes = 3;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.regionList = ImmutableList.of(region.uuid);
    userIntent.replicationFactor = 3;
    Map<String, String> gflags = new HashMap<>();
    gflags.put("foo", "bar");
    userIntent.masterGFlags = gflags;
    userIntent.tserverGFlags = gflags;
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    defaultUniverse = Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, true /* setMasters */));

    // Change one of the nodes' state to removed.
    setDefaultNodeState(defaultUniverse, NodeState.Removed, DEFAULT_NODE_NAME);

    mockClient = mock(YBClient.class);
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);
    when(mockClient.waitForLoadBalance(anyLong(), anyInt())).thenReturn(true);
    try {
      when(mockClient.getMasterClusterConfig()).thenReturn(mockConfigResponse);
      when(mockClient.changeMasterClusterConfig(any())).thenReturn(ccr);
      when(mockClient.setFlag(any(), anyString(), anyString(), anyBoolean())).thenReturn(true);
    } catch (Exception e) {}

    mockWaits(mockClient, 4);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    dummyShellResponse = new ShellResponse();
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);
    preflightSuccess = new ShellResponse();
    preflightSuccess.message = "{\"test\": true}";
    when(mockNodeManager.nodeCommand(eq(NodeCommandType.Precheck), any()))
      .thenReturn(preflightSuccess);
    modifyBL = mock(ModifyMasterClusterConfigBlacklist.class);
  }

  // Updates one of the nodes using a passed consumer.
  private Universe.UniverseUpdater getNodeUpdater(String nodeName, Consumer<NodeDetails> consumer) {
    return new Universe.UniverseUpdater() {
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        Set<NodeDetails> nodes = universeDetails.nodeDetailsSet;
        for (NodeDetails node : nodes) {
          if (node.nodeName.equals(nodeName)) {
            consumer.accept(node);
            break;
          }
        }
        universe.setUniverseDetails(universeDetails);
      }
    };
  }

  private void setDefaultNodeState(Universe universe, final NodeState desiredState,
      String nodeName) {
    Universe.saveDetails(universe.universeUUID, getNodeUpdater(nodeName, node -> {
      node.state = desiredState;
    }));
  }

  private TaskInfo submitTask(UUID universeUUID, String nodeName, int version) {
    Universe universe = Universe.get(universeUUID);
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.clusters.addAll(universe.getUniverseDetails().clusters);

    taskParams.expectedUniverseVersion = version;
    taskParams.nodeName = nodeName;
    taskParams.universeUUID = universe.universeUUID;
    taskParams.azUuid = AvailabilityZone.getByCode(AZ_CODE).uuid;
    try {
      UUID taskUUID = commissioner.submit(TaskType.AddNodeToUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  List<TaskType> ADD_NODE_TASK_SEQUENCE = ImmutableList.of(
    TaskType.SetNodeState,
    TaskType.AnsibleConfigureServers,
    TaskType.SetNodeState,
    TaskType.AnsibleConfigureServers,
    TaskType.AnsibleClusterServerCtl,
    TaskType.UpdateNodeProcess,
    TaskType.WaitForServer,
    TaskType.SwamperTargetsFileUpdate,
    TaskType.ModifyBlackList,
    TaskType.WaitForLoadBalance,
    TaskType.SetNodeState,
    TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> ADD_NODE_TASK_EXPECTED_RESULTS = ImmutableList.of(
    Json.toJson(ImmutableMap.of("state", "Adding")),
    Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("state", "ToJoinCluster")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("process", "tserver", "command", "start")),
    Json.toJson(ImmutableMap.of("processType", "TSERVER", "isAdd", true)),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("state", "Live")),
    Json.toJson(ImmutableMap.of())
  );

  List<TaskType> WITH_MASTER_UNDER_REPLICATED = ImmutableList.of(
    TaskType.SetNodeState,
    TaskType.AnsibleConfigureServers,
    TaskType.SetNodeState,
    TaskType.AnsibleConfigureServers,
    TaskType.AnsibleClusterServerCtl,
    TaskType.UpdateNodeProcess,
    TaskType.WaitForServer,
    TaskType.ChangeMasterConfig,
    TaskType.AnsibleConfigureServers,
    TaskType.AnsibleClusterServerCtl,
    TaskType.UpdateNodeProcess,
    TaskType.WaitForServer,
    TaskType.SwamperTargetsFileUpdate,
    TaskType.ModifyBlackList,
    TaskType.WaitForLoadBalance,
    TaskType.AnsibleConfigureServers,
    TaskType.SetFlagInMemory,
    TaskType.AnsibleConfigureServers,
    TaskType.SetFlagInMemory,
    TaskType.SetNodeState,
    TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> WITH_MASTER_UNDER_REPLICATED_RESULTS = ImmutableList.of(
    Json.toJson(ImmutableMap.of("state", "Adding")),
    Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("state", "ToJoinCluster")),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("process", "master", "command", "start")),
    Json.toJson(ImmutableMap.of("processType", "MASTER", "isAdd", true)),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("process", "tserver", "command", "start")),
    Json.toJson(ImmutableMap.of("processType", "TSERVER", "isAdd", true)),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of()),
    Json.toJson(ImmutableMap.of("state", "Live")),
    Json.toJson(ImmutableMap.of())
  );

  private void assertAddNodeSequence(Map<Integer, List<TaskInfo>> subTasksByPosition,
                                     boolean masterUnderReplicated) {
    int position = 0;
    if (masterUnderReplicated) {
      for (TaskType taskType: WITH_MASTER_UNDER_REPLICATED) {
        List<TaskInfo> tasks = subTasksByPosition.get(position);
        assertEquals("At position: " + position, taskType, tasks.get(0).getTaskType());
        JsonNode expectedResults =
            WITH_MASTER_UNDER_REPLICATED_RESULTS.get(position);
        List<JsonNode> taskDetails = tasks.stream()
            .map(t -> t.getTaskDetails())
            .collect(Collectors.toList());
        assertJsonEqual(expectedResults, taskDetails.get(0));
        position++;
      }
    } else {
      for (TaskType taskType: ADD_NODE_TASK_SEQUENCE) {
        List<TaskInfo> tasks = subTasksByPosition.get(position);
        assertEquals(1, tasks.size());
        assertEquals("At position: " + position, taskType, tasks.get(0).getTaskType());
        JsonNode expectedResults =
            ADD_NODE_TASK_EXPECTED_RESULTS.get(position);
        List<JsonNode> taskDetails = tasks.stream()
            .map(t -> t.getTaskDetails())
            .collect(Collectors.toList());
        LOG.info(taskDetails.get(0).toString());
        assertJsonEqual(expectedResults, taskDetails.get(0));
        position++;
      }
    }
  }

  @Test
  public void testAddNodeSuccess() {
    mockWaits(mockClient, 3);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    TaskInfo taskInfo = submitTask(defaultUniverse.universeUUID, DEFAULT_NODE_NAME, 3);
    verify(mockNodeManager, times(5)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertAddNodeSequence(subTasksByPosition, false);
  }

  @Test
  public void testAddNodeWithUnderReplicatedMaster() {
    verify(mockNodeManager, never()).nodeCommand(any(), any());
    Universe.saveDetails(defaultUniverse.universeUUID, getNodeUpdater(DEFAULT_NODE_NAME, node -> {
      node.isMaster = false;
    }));

    TaskInfo taskInfo = submitTask(defaultUniverse.universeUUID, DEFAULT_NODE_NAME, 4);
    // 5 calls for setting up the server and then 6 calls for setting the conf files.
    verify(mockNodeManager, times(13)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertAddNodeSequence(subTasksByPosition, true);
  }

  @Test
  public void testAddUnknownNode() {
    TaskInfo taskInfo = submitTask(defaultUniverse.universeUUID, "host-n9", 3);
    verify(mockNodeManager, times(0)).nodeCommand(any(), any());
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }

  @Test
  public void testAddNodeWithUnderReplicatedMaster_WithReadOnlyCluster_NodeFromPrimary() {
    Universe universe = createUniverse("Demo");
    universe = Universe.saveDetails(universe.universeUUID,
        ApiUtils.mockUniverseUpdaterWithInactiveAndReadReplicaNodes(false, 1));
    setDefaultGFlags(universe);

    // Change one of the nodes' state to removed.
    setDefaultNodeState(universe, NodeState.Removed, DEFAULT_NODE_NAME);

    TaskInfo taskInfo = submitTask(universe.universeUUID, DEFAULT_NODE_NAME, 4);
    verify(mockNodeManager, times(13)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition = subTasks.stream()
        .collect(Collectors.groupingBy(w -> w.getPosition()));
    assertAddNodeSequence(subTasksByPosition, true /* Master start is expected */);
  }

  @Test
  public void testAddNodeWithUnderReplicatedMaster_WithReadOnlyCluster_NodeFromReadReplica() {
    Universe universe = createUniverse("Demo");
    universe = Universe.saveDetails(universe.universeUUID,
        ApiUtils.mockUniverseUpdaterWithInactiveAndReadReplicaNodes(false, 1));
    setDefaultGFlags(universe);

    // Change one of the nodes' state to removed.
    setDefaultNodeState(universe, NodeState.Removed, "yb-tserver-0");

    TaskInfo taskInfo = submitTask(universe.universeUUID, "yb-tserver-0", 4);
    verify(mockNodeManager, times(5)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition = subTasks.stream()
        .collect(Collectors.groupingBy(w -> w.getPosition()));
    assertAddNodeSequence(subTasksByPosition, false /* Master start is unexpected */);
  }

  private void setDefaultGFlags(Universe universe) {
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        Map<String, String> gflags = new HashMap<>();
        gflags.put("foo", "bar");

        Cluster primaryCluster = universeDetails.getPrimaryCluster();
        primaryCluster.userIntent.masterGFlags = gflags;
        primaryCluster.userIntent.tserverGFlags = gflags;

        List<Cluster> readOnlyClusters = universeDetails.getReadOnlyClusters();
        if (readOnlyClusters.size() > 0) {
          readOnlyClusters.get(0).userIntent.masterGFlags = gflags;
          readOnlyClusters.get(0).userIntent.tserverGFlags = gflags;
        }
      }
    };
    Universe.saveDetails(universe.universeUUID, updater);
  }

  @Test
  public void testAddNodeToJoinClusterState() {
    mockWaits(mockClient, 3);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    when(mockClient.waitForLoadBalance(anyLong(), anyInt())).thenReturn(false);
    TaskInfo taskInfo = submitTask(defaultUniverse.universeUUID, DEFAULT_NODE_NAME, 3);
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());

    Universe universe = Universe.get(defaultUniverse.universeUUID);
    assertEquals(NodeDetails.NodeState.ToJoinCluster, universe.getNode(DEFAULT_NODE_NAME).state);
  }
}
