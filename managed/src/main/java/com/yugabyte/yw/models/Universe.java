// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.google.common.net.HostAndPort;
import com.google.common.base.Joiner;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.ConcurrentModificationException;
import java.util.Date;
import java.util.HashSet;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import javax.persistence.Table;
import javax.persistence.UniqueConstraint;

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.yugabyte.yw.cloud.UniverseResourceDetails;
import com.yugabyte.yw.common.NodeActionType;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.models.helpers.PlacementInfo;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import io.ebean.*;
import io.ebean.annotation.DbJson;
import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.helpers.NodeDetails;

import org.yb.client.YBClient;
import com.yugabyte.yw.common.services.YBClientService;

import play.data.validation.Constraints;
import play.libs.Json;
import play.api.Play;

@Table(
  uniqueConstraints =
  @UniqueConstraint(columnNames = {"name", "customer_id"})
)
@Entity
public class Universe extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(Universe.class);
  public static final String DISABLE_ALERTS_UNTIL = "disableAlertsUntilSecs";
  public static final String TAKE_BACKUPS = "takeBackups";
  public static final String HELM2_LEGACY = "helm2Legacy";

  public enum HelmLegacy {
    V3,
    V2TO3
  }

  // The universe UUID.
  @Id
  public UUID universeUUID;

  // The version number of the object. This is used to synchronize updates from multiple clients.
  @Constraints.Required
  @Column(nullable = false)
  public int version;

  // Tracks when the universe was created.
  @Constraints.Required
  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  public Date creationDate;

  // The universe name.
  public String name;

  // The customer id, needed only to enforce unique universe names for a customer.
  @Constraints.Required
  public Long customerId;

  @DbJson
  @Column(columnDefinition = "TEXT")
  public JsonNode config;

  public void setConfig(Map<String, String> configMap) {
    Map<String, String> currConfig = this.getConfig();
    String currConfigStr = Joiner.on(" ").withKeyValueSeparator("=").join(currConfig);
    LOG.info("Setting config {} on universe {} [ {} ]", currConfigStr, name, universeUUID);
    for (String key : configMap.keySet()) {
      currConfig.put(key, configMap.get(key));
    }
    this.config = Json.toJson(currConfig);
    this.save();
  }

  @JsonIgnore
  public Map<String, String> getConfig() {
    if (this.config == null) {
      return new HashMap();
    } else {
      return Json.fromJson(this.config, Map.class);
    }
  }

  // The Json serialized version of universeDetails. This is used only in read from and writing to
  // the DB.
  @Constraints.Required
  @Column(columnDefinition = "TEXT", nullable = false)
  private String universeDetailsJson;

  private UniverseDefinitionTaskParams universeDetails;
  public void setUniverseDetails(UniverseDefinitionTaskParams details) {
    universeDetails = details;
  }
  public UniverseDefinitionTaskParams getUniverseDetails() {
    return universeDetails;
  }

  public String getDnsName() {
    Provider p = Provider.get(
        UUID.fromString(universeDetails.getPrimaryCluster().userIntent.provider));
    if (p == null) {
      return null;
    }
    String dnsSuffix = p.getAwsHostedZoneName();
    if (dnsSuffix == null) {
      return null;
    }
    return String.format("%s.%s.%s", name, Customer.get(p.customerUUID).code, dnsSuffix);
  }

  public JsonNode toJson() {
    ObjectNode json = Json.newObject()
        .put("universeUUID", universeUUID.toString())
        .put("name", name)
        .put("creationDate", creationDate.toString())
        .put("version", version);
    String dnsName = getDnsName();
    if (dnsName != null) {
      json.put("dnsName", dnsName);
    }
    UniverseDefinitionTaskParams params = getUniverseDetails();
    Collection<NodeDetails> nodes = getNodes();
    try {
      json.set("resources", Json.toJson(UniverseResourceDetails.create(nodes, params)));
    } catch (Exception e) {
      json.set("resources", null);
    }

    ObjectNode universeDetailsJson = (ObjectNode) Json.toJson(params);
    updateNodesDynamicActions(universeDetailsJson, nodes);

    ArrayNode clustersArrayJson = Json.newArray();
    for (Cluster cluster : params.clusters) {
      JsonNode clusterJson = cluster.toJson();
      if (clusterJson != null) {
        clustersArrayJson.add(clusterJson);
      }
    }
    universeDetailsJson.set("clusters", clustersArrayJson);
    json.set("universeDetails", universeDetailsJson);
    json.set("universeConfig", this.config);
    return json;
  }

  /**
   * Modifies lists of allowed for nodes actions depending on the universe state.
   * Actions are added/removed directly into/from the json representation. Initial
   * values of allowed actions are set by NodeDetails.getAllowedActions().
   *
   * @param universeDetailsJson
   * @param nodes
   */
  void updateNodesDynamicActions(ObjectNode universeDetailsJson,
      Collection<NodeDetails> nodes) {
    JsonNode nodeDetailsSet = universeDetailsJson.get("nodeDetailsSet");
    if (nodeDetailsSet == null || nodeDetailsSet.isNull() || !nodeDetailsSet.isArray()) {
      return;
    }

    try {
      // Preparing a node name -> allowed actions map.
      Map<String, ArrayNode> nodeActions = new HashMap<>();
      for (int i = 0; i < nodeDetailsSet.size(); i++) {
        JsonNode jsonNode = nodeDetailsSet.get(i);
        JsonNode allowedActions = jsonNode.get("allowedActions");
        if (allowedActions != null && !allowedActions.isNull() && allowedActions.isArray()) {
          nodeActions.put(jsonNode.get("nodeName").asText(), (ArrayNode) allowedActions);
        }
      }

      for (NodeDetails node : nodes) {
        if (node.state == NodeDetails.NodeState.Live && !node.isMaster
            && Util.areMastersUnderReplicated(node, this)) {
          ArrayNode actions = nodeActions.get(node.nodeName);
          if (actions != null) {
            actions.add(NodeActionType.START_MASTER.name());
          }
        }
      }
    } catch (Exception e) {
      LOG.info("Unable to update allowed actions: " + e);
    }
  }

  public static final Finder<UUID, Universe> find = new Finder<UUID, Universe>(Universe.class) {
  };

  // Prefix added to read only node.
  public static final String READONLY = "-readonly";

  // Prefix added to node Index of each read replica node.
  public static final String NODEIDX_PREFIX = "-n";

  /**
   * Creates an empty universe.
   * @param taskParams: The details that will describe the universe.
   * @param customerId: UUID of the customer creating the universe
   * @return the newly created universe
   */
  public static Universe create(UniverseDefinitionTaskParams taskParams, Long customerId) {
    // Create the universe object.
    Universe universe = new Universe();
    // Generate a new UUID.
    universe.universeUUID = taskParams.universeUUID;
    // Set the version of the object to 1.
    universe.version = 1;
    // Set the creation date.
    universe.creationDate = new Date();
    // Set the universe name.
    universe.name = taskParams.getPrimaryCluster().userIntent.universeName;
    // Set the customer id.
    universe.customerId = customerId;
    // Create the default universe details. This should be updated after creation.
    universe.universeDetails = taskParams;
    universe.universeDetailsJson = Json.stringify(Json.toJson(universe.universeDetails));
    LOG.info("Created db entry for universe {} [{}]", universe.name, universe.universeUUID);
    LOG.debug("Details for universe {} [{}] : [{}].",
        universe.name, universe.universeUUID, universe.universeDetailsJson);
    // Save the object.
    universe.save();
    return universe;
  }

  /**
   * Returns true if Universe exists with given name
   * @param universeName String which contains the name which is to be checked
   * @return true if universe already exists, false otherwise
   */
  public static boolean checkIfUniverseExists(String universeName) {
    return find.query().select("universeUUID").where().eq("name", universeName).findCount() > 0;
  }

  /**
   * Fetch ONLY the universeUUID field for all universes.
   * WARNING: Returns partially filled Universe objects!!
   * @return list of UUIDs of all universes
   */
  public static List<Universe> getAllUuids() {
    return find.query().select("universeUUID").findList();
  }

  /**
   * Returns the Universe object given its uuid.
   *
   * @param universeUUID
   * @return the universe object
   */
  public static Universe get(UUID universeUUID) {
    // Find the universe.
    Universe universe = find.byId(universeUUID);
    if (universe == null) {
      throw new RuntimeException("Cannot find universe " + universeUUID);
    }

    JsonNode detailsJson = Json.parse(universe.universeDetailsJson);
    universe.universeDetails = Json.fromJson(detailsJson, UniverseDefinitionTaskParams.class);

    // For backwards compatibility from {universeDetails: {"userIntent": <foo>, "placementInfo": <bar>}}
    // to {universeDetails: {clusters: [{"userIntent": <foo>, "placementInfo": <bar>},...]}}
    if (detailsJson != null && !detailsJson.isNull() &&
        (!detailsJson.has("clusters") || detailsJson.get("clusters").size() == 0)) {
      UserIntent userIntent = Json.fromJson(detailsJson.get("userIntent"), UserIntent.class);
      PlacementInfo placementInfo = Json.fromJson(detailsJson.get("placementInfo"), PlacementInfo.class);
      universe.universeDetails.upsertPrimaryCluster(userIntent, placementInfo);
    }

    // Return the universe object.
    return universe;
  }

  public static Set<Universe> get(Set<UUID> universeUUIDs) {
    Set<Universe> universes = new HashSet<Universe>();
    for (UUID universeUUID : universeUUIDs) {
      universes.add(Universe.get(universeUUID));
    }
    return universes;
  }

  public static Universe getUniverseByName(String universeName) {
    if (checkIfUniverseExists(universeName)) {
      return find.query().where().eq("name", universeName).findOne();
    }
    return null;
  }

  /**
   * Interface using which we specify a callback to update the universe object. This is passed into
   * the save method.
   */
  public static interface UniverseUpdater {
    void run(Universe universe);
  }

  // Helper api to make an atomic read of universe version, and compare and swap the
  // updated version to disk.
  private static synchronized Universe readModifyWrite(UUID universeUUID,
                                                       UniverseUpdater updater,
                                                       boolean incrementVersion) {
    Universe universe = Universe.get(universeUUID);
    // Update the universe object which is supplied as a lambda function.
    boolean updateSucceeded = false;
    try {
      updater.run(universe);
      updateSucceeded = true;
    } catch(Exception e) {
      LOG.debug("Error running universe updater", e);
      throw e;
    } finally {
      // Save the universe object by doing a compare and swap.
      universe.compareAndSwap(updateSucceeded /* updateDetails */ , incrementVersion);
    }

    return universe;
  }

  /**
   * Updates the details of the universe if possible using the update lambda function.
   *
   * @param universeUUID : the universe UUID that we want to update
   * @param updater      : lambda which updated the details of this universe when invoked.
   * @return the updated version of the object if successful, or throws an exception.
   */
  public static Universe saveDetails(UUID universeUUID, UniverseUpdater updater) {
    return Universe.saveDetails(universeUUID, updater, true);
  }

  public static Universe saveDetails(
    UUID universeUUID,
    UniverseUpdater updater,
    boolean incrementVersion
  ) {
    int numRetriesLeft = 10;
    long sleepTimeMillis = 100;
    // Try the read and update for a few times till it succeeds.
    Universe universe = null;
    while (numRetriesLeft > 0) {
      try {
        universe = readModifyWrite(universeUUID, updater, incrementVersion);
        break;
      } catch (ConcurrentModificationException e) {
        // Decrement retries.
        numRetriesLeft--;
        // If we are out of retries, fail the task.
        if (numRetriesLeft == 0) {
          throw e;
        }
        // If we have more retries left, wait and retry.
        try {
          Thread.sleep(sleepTimeMillis);
        } catch (InterruptedException e1) {
          LOG.error("Error while sleeping", e1);
        }
      }
    }

    return universe;
  }

  /**
   * Deletes the universe entry with the given UUID.
   *
   * @param universeUUID : uuid of the universe.
   */
  public static void delete(UUID universeUUID) {
    // First get the universe.
    Universe universe = Universe.get(universeUUID);
    // Make sure this universe has been locked.
    assert !universe.universeDetails.updateInProgress;
    // Delete the universe.
    LOG.info("Deleting universe " + universe.name + ":" + universeUUID);
    universe.delete();
  }

  /**
   * Returns the list of nodes in the universe.
   *
   * @return a collection of nodes in this universe
   */
  public Collection<NodeDetails> getNodes() {
    return getUniverseDetails().nodeDetailsSet;
  }

  /**
   * Checks if there is any node in a transit state across the universe.
   *
   * @return true if there is any such node.
   */
  public boolean nodesInTransit() {
    return getUniverseDetails().nodeDetailsSet.stream().filter(n -> n.isInTransit()).count() > 0;
  }

  /**
   * Returns details about a single node in the universe.
   *
   * @param nodeName
   * @return details about a node, null if it does not exist.
   */
  public NodeDetails getNode(String nodeName) {
    Collection<NodeDetails> nodes = getNodes();
    for (NodeDetails node : nodes) {
      if (node.nodeName != null && node.nodeName.equals(nodeName)) {
        return node;
      }
    }
    return null;
  }

  /**
   * Returns details about a single node by ip address in the universe.
   *
   * @param nodeIP Private IP address of the node
   * @return details about a node, null if it does not exist.
   */
  public NodeDetails getNodeByPrivateIP(String nodeIP) {
    Collection<NodeDetails> nodes = getNodes();
    for (NodeDetails node : nodes) {
      if (node.cloudInfo.private_ip.equals(nodeIP)) {
        return node;
      }
    }
    return null;
  }

  /**
   * Returns the list of masters for this universe.
   *
   * @return a list of master nodes
   */
  public List<NodeDetails> getMasters() {
    return getServers(ServerType.MASTER);
  }

  /**
   * Return the list of tservers for this universe.
   *
   * @return a list of tserver nodes
   */
  public List<NodeDetails> getTServers() {
    return getServers(ServerType.TSERVER);
  }

  /**
   * Return the list of YQL servers for this universe.
   *
   * @return a list of YQL server nodes
   */
  public List<NodeDetails> getYqlServers() {
    return getServers(ServerType.YQLSERVER);
  }

  /**
   * Return the list of YSQL servers for this universe.
   *
   * @return a list of YSQL server nodes
   */
  public List<NodeDetails> getYsqlServers() {
    return getServers(ServerType.YSQLSERVER);
  }

  /**
   * Return the list of Redis servers for this universe.
   *
   * @return a list of Redis server nodes
   */
  public List<NodeDetails> getRedisServers() {
    return getServers(ServerType.REDISSERVER);
  }

  private class NodeDetailsPrivateIpComparator implements Comparator<NodeDetails> {
    @Override
    public int compare(NodeDetails n1, NodeDetails n2) {
      return n1.cloudInfo.private_ip.compareTo(n2.cloudInfo.private_ip);
    }
  }

  public List<NodeDetails> getServers(ServerType type) {
    List<NodeDetails> servers = new ArrayList<NodeDetails>();
    UniverseDefinitionTaskParams details = getUniverseDetails();
    Set<NodeDetails> filteredNodeDetails = details.nodeDetailsSet.stream()
      .filter(n -> n.cloudInfo.private_ip != null)
      .collect(Collectors.toSet());
    for (NodeDetails nodeDetails : filteredNodeDetails) {
      switch(type) {
      case YQLSERVER:
        if (nodeDetails.isYqlServer && nodeDetails.isTserver) servers.add(nodeDetails); break;
      case YSQLSERVER:
        if (nodeDetails.isYsqlServer && nodeDetails.isTserver) servers.add(nodeDetails); break;
      case TSERVER:
        if (nodeDetails.isTserver) servers.add(nodeDetails); break;
      case MASTER:
        if (nodeDetails.isMaster) servers.add(nodeDetails); break;
      case REDISSERVER:
        if (nodeDetails.isRedisServer && nodeDetails.isTserver) servers.add(nodeDetails); break;
      default:
        throw new IllegalArgumentException("Unexpected server type " + type);
      }
    }
    // Sort by private IP for deterministic behaviour.
    Collections.sort(servers, new NodeDetailsPrivateIpComparator());
    return servers;
  }

  /**
   * Verifies that the provided list of masters is not empty and that each master is in a queryable
   * state. If so, returns true. Otherwise, returns false.
   *
   * @param masters
   * @return true if all masters are queryable, false otherwise.
   */
  public boolean verifyMastersAreQueryable(List<NodeDetails> masters) {
    if (masters == null || masters.isEmpty()) {
      return false;
    }
    for (NodeDetails details : masters) {
      if (!details.isQueryable()) {
        return false;
      }
    }
    return true;
  }

  public String getKubernetesMasterAddresses() {
    return getMasters().stream().map((m)-> m.nodeName).collect(Collectors.joining(","));
  }

  public String getMasterAddresses() {
    return getMasterAddresses(false);
  }

  /**
   * Returns a comma separated list of <privateIp:masterRpcPort> for all nodes that have the
   * isMaster flag set to true in this cluster.
   *
   * @param mastersQueryable Set to true if caller wants masters to be queryable, else false.
   * @return a comma separated string of master 'host:port' or, if masters are not queryable, an
   * empty string.
   */
  public String getMasterAddresses(boolean mastersQueryable) {
    List<NodeDetails> masters = getMasters();
    if (mastersQueryable && !verifyMastersAreQueryable(masters)) {
      return "";
    }
    return getHostPortsString(masters, ServerType.MASTER);
  }

  /**
   * Returns the certificate in case TLS is enabled.
   * @return certificate file if TLS is enabled, null otherwise.
   */
  public String getCertificate() {
    UUID rootCA = this.getUniverseDetails().rootCA;
    if (rootCA == null) {
      return null;
    }
    return CertificateInfo.get(rootCA).certificate;
  }

  /**
   * Returns a comma separated list of <privateIp:yqlRPCPort> for all nodes that have the
   * isYQLServer flag set to true in this cluster.
   *
   * @return a comma separated string of 'host:port'.
   */
  public String getYQLServerAddresses() {
    return getHostPortsString(getYqlServers(), ServerType.YQLSERVER);
  }

  /**
   * Returns a comma separated list of <privateIp:ysqlRPCPort> for all nodes that have the
   * isYSQLServer flag set to true in this cluster.
   *
   * @return a comma separated string of 'host:port'.
   */
  public String getYSQLServerAddresses() {
    return getHostPortsString(getYsqlServers(), ServerType.YSQLSERVER);
  }

  /**
   * Returns a comma separated list of <privateIp:redisRPCPort> for all nodes that have the
   * isRedisServer flag set to true in this cluster.
   *
   * @return a comma separated string of 'host:port'.
   */
  public String getRedisServerAddresses() {
    return getHostPortsString(getRedisServers(), ServerType.REDISSERVER);
  }

  // Helper API to create the based on the server type.
  private String getHostPortsString(List<NodeDetails> serverNodes, ServerType type) {
    StringBuilder servers = new StringBuilder();
    for (NodeDetails node : serverNodes) {
      if (node.cloudInfo.private_ip != null) {
        int port = 0;
        switch (type) {
        case YQLSERVER:
          if (node.isYqlServer) port = node.yqlServerRpcPort; break;
        case YSQLSERVER:
          if (node.isYsqlServer) port = node.ysqlServerRpcPort; break;
        case TSERVER:
          if (node.isTserver) port = node.tserverRpcPort; break;
        case MASTER:
          if (node.isMaster) port = node.masterRpcPort; break;
        case REDISSERVER:
          if (node.isRedisServer) port = node.redisServerRpcPort; break;
        default:
          throw new IllegalArgumentException("Unexpected server type " + type);
        }

        if (servers.length() != 0) {
          servers.append(",");
        }
        servers.append(node.cloudInfo.private_ip).append(":").append(port);
      }
    }
    return servers.toString();
  }

  /**
   * Compares the version of this object with the one in the DB, and updates it if the versions
   * match.
   *
   * @return the new version number after the update if successful, or throws a RuntimeException.
   */
  /**
   * Compares the version of this object with the one in the DB, and updates it if the versions
   * match.
   *
   * @param updateDetails whether to update universe details or not
   * @param incrementVersion whether to increment the version or not
   * @return the current version of the universe metadata
   */
  private int compareAndSwap(boolean updateDetails, boolean incrementVersion) {
    // Update the universe details json.
    universeDetailsJson = Json.stringify(Json.toJson(universeDetails));

    // Create the new version number.
    int newVersion = incrementVersion ? this.version + 1 : this.version;

    // Save the object if the version is the same.
    String updateQuery = updateDetails ? "UPDATE universe " +
      "SET universe_details_json = :universeDetails, version = :newVersion " +
      "WHERE universe_uuid = :universeUUID AND version = :curVersion"
      :
      "UPDATE universe " +
      "SET version = :newVersion " +
      "WHERE universe_uuid = :universeUUID AND version = :curVersion";

    SqlUpdate update = Ebean.createSqlUpdate(updateQuery);
    if (updateDetails) {
      update.setParameter("universeDetails", universeDetailsJson);
    }

    update.setParameter("universeUUID", universeUUID);
    update.setParameter("curVersion", this.version);
    update.setParameter("newVersion", newVersion);
    LOG.trace("Swapped universe {}:{} details to [{}] with new version = {}.",
              universeUUID, this.name, universeDetailsJson, newVersion);
    int modifiedCount = Ebean.execute(update);

    // Check if the save was not successful.
    if (modifiedCount == 0) {
      // Throw an exception as the save was not successful.
      throw new ConcurrentModificationException("Stale version " + this.version);
    } else if (modifiedCount > 1) {
      // Exactly one row should have been modified. Otherwise fatal.
      LOG.error("Running query [" + updateQuery + "] updated " + modifiedCount + " rows");
      System.exit(1);
    }

    // Update and return the current version number.
    this.version = newVersion;
    return this.version;
  }

  /**
   * Returns the list of nodes in a given cluster in the universe.
   *
   * @param clusterUUID UUID of the cluster to get the list of nodes.
   * @return a collection of nodes in a given cluster in this universe.
   */
  public Collection<NodeDetails> getNodesInCluster(UUID clusterUUID) {
    return getUniverseDetails().getNodesInCluster(clusterUUID);
  }

  /**
   * Returns the cluster with the given uuid in the universe.
   *
   * @param clusterUUID UUID of the cluster to check.
   * @return The cluster object if it is in this universe, null otherwise.
   */
  public Cluster getCluster(UUID clusterUUID) {
    return getUniverseDetails().getClusterByUuid(clusterUUID);
  }

  /**
   * Returns the cluster to which this node belongs.
   *
   * @param nodeName name of the node.
   * @param universe universe which can contain the node.
   * @return cluster info from the universe which contains this node.
   */
  public static Cluster getCluster(Universe universe, String nodeName) {
    if (!nodeName.contains(READONLY)) {
      return universe.getUniverseDetails().getPrimaryCluster();
    }

    for (Cluster cluster : universe.getUniverseDetails().getReadOnlyClusters()) {
      if (nodeName.contains(Universe.READONLY + cluster.index + Universe.NODEIDX_PREFIX)) {
        return cluster;
      }
    }

    return null;
  }

  /**
   * Checks if node is allowed to perform the action without under-replicating master nodes in the universe.
   *
   * @return whether the node is allowed to perform the action.
   */
  public boolean isNodeActionAllowed(String nodeName, NodeActionType action) {
    NodeDetails node = getNode(nodeName);
    Cluster curCluster = getCluster(node.placementUuid);

    if (node.isMaster && (action == NodeActionType.STOP || action == NodeActionType.REMOVE)
        && (curCluster.clusterType == ClusterType.PRIMARY)) {
      long numMasterNodesUp = universeDetails.getNodesInCluster(curCluster.uuid).stream()
          .filter((n) -> n.isMaster && n.state == NodeDetails.NodeState.Live).count();
      if (numMasterNodesUp <= (curCluster.userIntent.replicationFactor + 1) / 2) {
        return false;
      }
    }

    if (action == NodeActionType.START_MASTER) {
      return (!node.isMaster && (node.state == NodeDetails.NodeState.Live)
          && Util.areMastersUnderReplicated(node, this));
    }

    return node.getAllowedActions().contains(action);
  }

  /**
   * Find the current master leader in the universe
   *
   * @return the host (private_ip) and port of the current master leader in the universe
   *  or null if not found
   */
  public HostAndPort getMasterLeader() {
    final String masterAddresses = getMasterAddresses();
    final String cert = getCertificate();
    final YBClientService ybService = Play.current().injector().instanceOf(YBClientService.class);
    final YBClient client = ybService.getClient(masterAddresses, cert);
    final HostAndPort leaderMasterHostAndPort = client.getLeaderMasterHostAndPort();
    ybService.closeClient(client, masterAddresses);
    return leaderMasterHostAndPort;
  }

  /**
   * Find the current master leader in the universe
   *
   * @return a String of the private_ip of the current master leader in the universe
   *  or an empty string if not found
   */
  public String getMasterLeaderHostText() {
    final HostAndPort masterLeader = getMasterLeader();
    if (masterLeader == null) return "";
    return masterLeader.getHost();
  }

  public boolean universeIsLocked() {
    return getUniverseDetails().updateInProgress;
  }

  public boolean nodeExists(String host, int port) {
    return getUniverseDetails().nodeDetailsSet.parallelStream().anyMatch(n ->
      n.cloudInfo.private_ip.equals(host) && (
        port == n.masterHttpPort ||
          port == n.tserverHttpPort ||
          port == n.ysqlServerHttpPort ||
          port == n.yqlServerHttpPort ||
          port == n.redisServerHttpPort));
  }

  public void incrementVersion() {
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {}
    };
    Universe.saveDetails(universeUUID, updater);
  }
}
