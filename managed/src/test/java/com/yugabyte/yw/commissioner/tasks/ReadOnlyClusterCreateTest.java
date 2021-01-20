// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.subtasks.UpdatePlacementInfo.ModifyUniverseConfig;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.NodeManager.NodeCommandType;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import org.yb.client.YBClient;
import org.yb.client.AbstractModifyMasterClusterConfig;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.master.Master;
import play.libs.Json;

import java.util.*;
import java.util.stream.Collectors;

import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Mockito.*;

@RunWith(MockitoJUnitRunner.class)
public class ReadOnlyClusterCreateTest extends CommissionerBaseTest {
  @InjectMocks
  Commissioner commissioner;
  Universe defaultUniverse;
  ShellResponse dummyShellResponse;
  ShellResponse preflightSuccessResponse;
  YBClient mockClient;
  ModifyUniverseConfig modifyUC;
  AbstractModifyMasterClusterConfig amuc;

  @Before
  public void setUp() {
    super.setUp();
    Region region = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    AvailabilityZone.create(region, "az-1", "AZ 1", "subnet-1");
    // create default universe
    UserIntent userIntent = new UserIntent();
    userIntent.numNodes = 3;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.regionList = ImmutableList.of(region.uuid);
    userIntent.instanceType = ApiUtils.UTIL_INST_TYPE;
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(defaultUniverse.universeUUID,
    ApiUtils.mockUniverseUpdater(userIntent, true /* setMasters */));
    mockClient = mock(YBClient.class);
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);
    dummyShellResponse = new ShellResponse();
    dummyShellResponse.message = "true";
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);
    preflightSuccessResponse = new ShellResponse();
    preflightSuccessResponse.message = "{\"test\": true}";
    when(mockNodeManager.nodeCommand(eq(NodeCommandType.Precheck), any()))
      .thenReturn(preflightSuccessResponse);
    // TODO(bogdan): I don't think these mocks of the AbstractModifyMasterClusterConfig are doing
    // anything..
    modifyUC = mock(ModifyUniverseConfig.class);
    amuc = mock(AbstractModifyMasterClusterConfig.class);

    Master.SysClusterConfigEntryPB.Builder configBuilder =
      Master.SysClusterConfigEntryPB.newBuilder().setVersion(1);
    GetMasterClusterConfigResponse mockConfigResponse =
      new GetMasterClusterConfigResponse(1111, "", configBuilder.build(), null);
    ChangeMasterClusterConfigResponse mockChangeConfigResponse =
      new ChangeMasterClusterConfigResponse(1111, "", null);

    try {
      when(mockClient.getMasterClusterConfig()).thenReturn(mockConfigResponse);
      when(mockClient.changeMasterClusterConfig(any())).thenReturn(mockChangeConfigResponse);
    } catch (Exception e) {}
    // WaitForServer mock.
    mockWaits(mockClient);
  }

  private TaskInfo submitTask(UniverseDefinitionTaskParams taskParams) {
    taskParams.expectedUniverseVersion = 2;
    try {
      UUID taskUUID = commissioner.submit(TaskType.ReadOnlyClusterCreate, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  List<TaskType> CLUSTER_CREATE_TASK_SEQUENCE = ImmutableList.of(
      TaskType.AnsibleSetupServer,
      TaskType.AnsibleUpdateNodeInfo,
      TaskType.AnsibleConfigureServers,
      TaskType.AnsibleConfigureServers,
      TaskType.AnsibleClusterServerCtl,
      TaskType.WaitForServer,
      TaskType.SetNodeState,
      TaskType.UpdatePlacementInfo,
      TaskType.SwamperTargetsFileUpdate,
      TaskType.UniverseUpdateSucceeded
  );

  List<JsonNode> CLUSTER_CREATE_TASK_EXPECTED_RESULTS = ImmutableList.of(
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of("process", "tserver", "command", "start")),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of()),
      Json.toJson(ImmutableMap.of())
  );

  private void assertClusterCreateSequence(Map<Integer, List<TaskInfo>> subTasksByPosition,
                                           boolean masterUnderReplicated) {
    int position = 0;
    for (TaskType taskType: CLUSTER_CREATE_TASK_SEQUENCE) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      assertEquals(1, tasks.size());
      assertEquals(taskType, tasks.get(0).getTaskType());
      JsonNode expectedResults = CLUSTER_CREATE_TASK_EXPECTED_RESULTS.get(position);
      List<JsonNode> taskDetails = tasks.stream().map(t -> t.getTaskDetails())
                                                 .collect(Collectors.toList());
      assertJsonEqual(expectedResults, taskDetails.get(0));
      position++;
    }
  }

  @Test
  public void testClusterCreateSuccess() {
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    taskParams.currentClusterType = ClusterType.ASYNC;
    UserIntent userIntent = new UserIntent();
    Region region = Region.create(defaultProvider, "region-2", "Region 2", "yb-image-1");
    AvailabilityZone.create(region, "az-2", "AZ 2", "subnet-2");
    userIntent.numNodes = 1;
    userIntent.replicationFactor = 1;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.regionList = ImmutableList.of(region.uuid);
    userIntent.instanceType = ApiUtils.UTIL_INST_TYPE;
    userIntent.universeName = defaultUniverse.name;
    taskParams.clusters.add(new Cluster(ClusterType.ASYNC, userIntent));
    PlacementInfoUtil.updateUniverseDefinition(taskParams, defaultCustomer.getCustomerId(),
        taskParams.clusters.get(0).uuid, UniverseDefinitionTaskParams.ClusterOperationType.CREATE);
    int iter = 1;
    for (NodeDetails node : taskParams.nodeDetailsSet) {
      node.cloudInfo.private_ip = "10.9.22." + iter;
      node.tserverRpcPort = 3333;
      iter++;
    }
    TaskInfo taskInfo = submitTask(taskParams);
    verify(mockNodeManager, times(7)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(w -> w.getPosition()));
    assertClusterCreateSequence(subTasksByPosition, false);

    UniverseDefinitionTaskParams univUTP =
        Universe.get(defaultUniverse.universeUUID).getUniverseDetails();
    assertEquals(2, univUTP.clusters.size());
  }

  @Test
  public void testClusterCreateFailure() {
    UniverseDefinitionTaskParams univUTP =
      Universe.get(defaultUniverse.universeUUID).getUniverseDetails();
    assertEquals(1, univUTP.clusters.size());
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;
    TaskInfo taskInfo = submitTask(taskParams);
    assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
  }
}
