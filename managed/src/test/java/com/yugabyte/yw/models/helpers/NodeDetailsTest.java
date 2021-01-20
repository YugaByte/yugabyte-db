// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models.helpers;

import com.google.common.collect.ImmutableSet;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.NodeActionType;
import org.junit.Before;
import org.junit.Test;

import java.util.HashSet;
import java.util.Set;

import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.hamcrest.core.AllOf.allOf;
import static org.junit.Assert.*;

public class NodeDetailsTest {
  private NodeDetails nd;

  @Before
  public void setUp() {
    nd = ApiUtils.getDummyNodeDetails(1, NodeDetails.NodeState.Live);
  }

  @Test
  public void testToString() {
    assertThat(nd.toString(), allOf(notNullValue(),
                                    equalTo("name: host-n1, cloudInfo: az-1.test-region.aws, type: "
                                            + ApiUtils.UTIL_INST_TYPE + ", ip: host-n1, " +
                                            "isMaster: false, isTserver: true, state: Live, " +
                                            "azUuid: null, placementUuid: null")));
  }

  @Test
  public void testIsActive() {
    Set<NodeDetails.NodeState> activeStates = new HashSet<>();
    activeStates.add(NodeDetails.NodeState.ToBeAdded);
    activeStates.add(NodeDetails.NodeState.ToJoinCluster);
    activeStates.add(NodeDetails.NodeState.Provisioned);
    activeStates.add(NodeDetails.NodeState.SoftwareInstalled);
    activeStates.add(NodeDetails.NodeState.UpgradeSoftware);
    activeStates.add(NodeDetails.NodeState.UpdateGFlags);
    activeStates.add(NodeDetails.NodeState.UpdateCert);
    activeStates.add(NodeDetails.NodeState.Live);
    activeStates.add(NodeDetails.NodeState.Stopping);
    for (NodeDetails.NodeState state : NodeDetails.NodeState.values()) {
      nd.state = state;
      if (activeStates.contains(state)) {
        assertTrue(nd.isActive());
      } else {
        assertFalse(nd.isActive());
      }
    }
  }

  @Test
  public void testIsQueryable() {
    Set<NodeDetails.NodeState> queryableStates = new HashSet<>();
    queryableStates.add(NodeDetails.NodeState.UpgradeSoftware);
    queryableStates.add(NodeDetails.NodeState.UpdateGFlags);
    queryableStates.add(NodeDetails.NodeState.UpdateCert);
    queryableStates.add(NodeDetails.NodeState.Live);
    queryableStates.add(NodeDetails.NodeState.ToBeRemoved);
    queryableStates.add(NodeDetails.NodeState.Removing);
    queryableStates.add(NodeDetails.NodeState.Stopping);
    for (NodeDetails.NodeState state : NodeDetails.NodeState.values()) {
      nd.state = state;
      if (queryableStates.contains(state)) {
        assertTrue(nd.isQueryable());
      } else {
        assertFalse(nd.isQueryable());
      }
    }
  }

  @Test
  public void testGetAllowedActions() {
    for (NodeDetails.NodeState nodeState : NodeDetails.NodeState.values()) {
      nd.state = nodeState;
      if (nodeState == NodeDetails.NodeState.ToBeAdded) {
        assertEquals(ImmutableSet.of(NodeActionType.DELETE), nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.Adding) {
        assertEquals(ImmutableSet.of(NodeActionType.DELETE), nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.ToJoinCluster) {
        assertEquals(ImmutableSet.of(NodeActionType.REMOVE), nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.SoftwareInstalled) {
        assertEquals(ImmutableSet.of(NodeActionType.START, NodeActionType.DELETE),
            nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.ToBeRemoved) {
        assertEquals(ImmutableSet.of(NodeActionType.REMOVE), nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.Live) {
        assertEquals(ImmutableSet.of(NodeActionType.STOP, NodeActionType.REMOVE),
            nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.Stopped) {
        assertEquals(ImmutableSet.of(NodeActionType.START, NodeActionType.RELEASE),
            nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.Removed) {
        assertEquals(
            ImmutableSet.of(NodeActionType.ADD, NodeActionType.RELEASE, NodeActionType.DELETE),
            nd.getAllowedActions());
      } else if (nodeState == NodeDetails.NodeState.Decommissioned) {
        assertEquals(ImmutableSet.of(NodeActionType.ADD, NodeActionType.DELETE),
            nd.getAllowedActions());
      } else {
        assertTrue(nd.getAllowedActions().isEmpty());
      }
    }
  }

  @Test
  public void testGetAllowedActions_AllDeletesAllowed() {
    for (NodeDetails.NodeState nodeState : NodeDetails.NodeState.values()) {
      nd.state = nodeState;
      Set<NodeActionType> actions = nd.getAllowedActions();
      assertEquals(nd.isRemovable(), actions.contains(NodeActionType.DELETE));
    }
  }
}
