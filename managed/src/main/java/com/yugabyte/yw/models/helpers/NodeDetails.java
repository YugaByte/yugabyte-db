// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models.helpers;

import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.google.common.collect.ImmutableSet;
import com.yugabyte.yw.common.NodeActionType;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;

/**
 * Represents all the details of a cloud node that are of interest.
 */
@JsonIgnoreProperties(ignoreUnknown = true)
public class NodeDetails {
  // The id of the node. This is usually present in the node name.
  public int nodeIdx = -1;
  // Name of the node.
  public String nodeName;
  // The UUID of the node we are using.
  // TODO: only used for onprem at the moment.
  public UUID nodeUuid;

  // Information about the node that is returned by the cloud provider.
  public CloudSpecificInfo cloudInfo;

  // The AZ UUID (the YB UUID for the AZ) into which the node is deployed.
  public UUID azUuid;

  // The UUID of the cluster that this node belongs to.
  public UUID placementUuid;

  // Possible states in which this node can exist.
  public enum NodeState {
    // Set when a new node needs to be added into a Universe and has not yet been created.
    ToBeAdded,
    // Set when a new node is provisioned and configured but before it is added into
    // the existing cluster.
    ToJoinCluster,
    // Set after the node (without any configuration) is created using the IaaS provider at the
    // end of the provision step.
    Provisioned,
    // Set after the YB software installed and some basic configuration done on a provisioned node.
    SoftwareInstalled,
    // Set after the YB software is upgraded via Rolling Restart.
    UpgradeSoftware,
    // Set after the YB specific GFlags are updated via Rolling Restart.
    UpdateGFlags,
    // Set after all the services (master, tserver, etc) on a node are successfully running.
    Live,

    // Set when node is about to enter the stopped state.
    Stopping,
    // Set when node is about to be set to live state.
    Starting,
    // Set when node has been stopped and no longer has a master or a tserver running.
    Stopped,
    // Set when node is unreachable but has not been Removed from the universe.
    Unreachable,
    // Set when a node is marked for removal. Note that we will wait to get all its data out.
    ToBeRemoved,
    // Set just before sending the request to the IaaS provider to terminate this node.
    Removing,
    // Set after the node has been removed.
    Removed,
    // Set when node is about to enter the Live state from Removed/Decommissioned state.
    Adding,
    // Set when a stopped/removed node is about to enter the Decommissioned state.
    BeingDecommissioned,
    // After a stopped/removed node is returned back to the IaaS.
    Decommissioned,
    // Set when the cert is being updated.
    UpdateCert
  }

  // The current state of the node.
  public NodeState state;

  // True if this node is a master, along with port info.
  public boolean isMaster;
  public int masterHttpPort = 7000;
  public int masterRpcPort = 7100;

  // True if this node is a tserver, along with port info.
  public boolean isTserver = true;
  public int tserverHttpPort = 9000;
  public int tserverRpcPort = 9100;

  // True if this node is a Redis server, along with port info.
  public boolean isRedisServer = true;
  public int redisServerHttpPort = 11000;
  public int redisServerRpcPort = 6379;

  // True if this node is a YSQL server, along with port info.
  public boolean isYqlServer = true;
  public int yqlServerHttpPort = 12000;
  public int yqlServerRpcPort = 9042;

  // True if this node is a YSQL server, along with port info.
  public boolean isYsqlServer = true;
  public int ysqlServerHttpPort = 13000;
  public int ysqlServerRpcPort = 5433;

  // Which port node_exporter is running on.
  public int nodeExporterPort = 9300;

  // True if cronjobs were properly configured for this node.
  public boolean cronsActive = true;

  // List of states which are considered in-transit and ops such as upgrade should not be allowed.
  public static final Set<NodeState> IN_TRANSIT_STATES =
      ImmutableSet.of(NodeState.Removed, NodeState.Stopped, NodeState.Decommissioned);

  @Override
  public NodeDetails clone() {
    NodeDetails clone = new NodeDetails();
    clone.isMaster = this.isMaster;
    clone.isTserver = this.isTserver;
    clone.isRedisServer = this.isRedisServer;
    clone.isYqlServer = this.isYqlServer;
    clone.isYsqlServer = this.isYsqlServer;
    clone.state = this.state;
    clone.azUuid = this.azUuid;
    clone.cloudInfo = this.cloudInfo.clone();
    clone.nodeName = this.nodeName;
    clone.nodeIdx = this.nodeIdx;
    clone.nodeUuid = this.nodeUuid;
    clone.placementUuid = this.placementUuid;
    return clone;
  }

  @Override
  public String toString() {
    StringBuilder sb = new StringBuilder();
    sb.append("name: ").append(nodeName).append(", ")
      .append(cloudInfo.toString())
      .append(", isMaster: ").append(isMaster)
      .append(", isTserver: ").append(isTserver)
      .append(", state: ").append(state)
      .append(", azUuid: ").append(azUuid)
      .append(", placementUuid: ").append(placementUuid);
    return sb.toString();
  }

  @JsonIgnore
  public boolean isActive() {
    return !(state == NodeState.Unreachable ||
      state == NodeState.ToBeRemoved ||
      state == NodeState.Removing ||
      state == NodeState.Removed ||
      state == NodeState.Starting ||
      state == NodeState.Stopped ||
      state == NodeState.Adding ||
      state == NodeState.BeingDecommissioned ||
      state == NodeState.Decommissioned);
  }

  @JsonIgnore
  public boolean isQueryable() {
    return (state == NodeState.UpgradeSoftware ||
      state == NodeState.UpdateGFlags ||
      state == NodeState.Live ||
      state == NodeState.ToBeRemoved ||
      state == NodeState.Removing ||
      state == NodeState.Stopping ||
      state == NodeState.UpdateCert);
  }

  @JsonIgnore
  public boolean isInTransit() {
    return IN_TRANSIT_STATES.contains(state);
  }

  public Set<NodeActionType> getAllowedActions() {
    if (state == null) {
      return new HashSet<>();
    }
    switch (state) {
      // Unexpected/abnormal states.
      case ToBeAdded:
        return new HashSet<>(Arrays.asList(NodeActionType.DELETE));
      case Adding:
        return new HashSet<>(Arrays.asList(NodeActionType.DELETE));
      case ToJoinCluster:
        return new HashSet<>(Arrays.asList(NodeActionType.REMOVE));
      case SoftwareInstalled:
        return new HashSet<>(Arrays.asList(NodeActionType.START, NodeActionType.DELETE));
      case ToBeRemoved:
        return new HashSet<>(Arrays.asList(NodeActionType.REMOVE));

      // Expected/normal states.
      case Live:
        return new HashSet<>(Arrays.asList(NodeActionType.STOP, NodeActionType.REMOVE));
      case Stopped:
        return new HashSet<>(Arrays.asList(NodeActionType.START, NodeActionType.RELEASE));
      case Removed:
        return new HashSet<>(
            Arrays.asList(NodeActionType.ADD, NodeActionType.RELEASE, NodeActionType.DELETE));
      case Decommissioned:
        return new HashSet<>(Arrays.asList(NodeActionType.ADD, NodeActionType.DELETE));
      default:
        return new HashSet<>();
    }
  }

  @JsonIgnore
  public boolean isRemovable() {
    return state == NodeState.ToBeAdded || state == NodeState.Adding
        || state == NodeState.SoftwareInstalled || state == NodeState.Removed
        || state == NodeState.Decommissioned;
  }

  @JsonIgnore
  public boolean isInPlacement(UUID placementUuid) {
    return this.placementUuid != null && this.placementUuid.equals(placementUuid);
  }

  @JsonIgnore
  public String getRegion() {
    return this.cloudInfo.region;
  }

  @JsonIgnore
  public String getZone() {
    return this.cloudInfo.az;
  }

  public int getNodeIdx() {
    return this.nodeIdx;
  }
}
