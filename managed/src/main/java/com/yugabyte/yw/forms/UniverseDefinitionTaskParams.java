// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;

import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.Iterables;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.helpers.DeviceInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;

import play.data.validation.Constraints;
import play.libs.Json;

/**
 * This class captures the user intent for creation of the universe. Note some nuances in the way
 * the intent is specified.
 * <p>
 * Single AZ deployments:
 * Exactly one region should be specified in the 'regionList'.
 * <p>
 * Multi-AZ deployments:
 * 1. There is at least one region specified which has a at least 'replicationFactor' number of AZs.
 * <p>
 * 2. There are multiple regions specified, and the sum total of all AZs in those regions is greater
 * than or equal to 'replicationFactor'. In this case, the preferred region can be specified to
 * hint which region needs to have a majority of the data copies if applicable, as well as
 * serving as the primary leader. Note that we do not currently support ability to place leaders
 * in a preferred region.
 * <p>
 * NOTE #1: The regions can potentially be present in different clouds.
 */
public class UniverseDefinitionTaskParams extends UniverseTaskParams {

  @Constraints.Required()
  @Constraints.MinLength(1)
  public List<Cluster> clusters = new LinkedList<>();

  @JsonIgnore
  // This is set during configure to figure out which cluster type is intended to be modified.
  public ClusterType currentClusterType = ClusterType.PRIMARY;

  public enum ClusterOperationType {
    CREATE, EDIT, DELETE
  }

  @JsonIgnore
  public ClusterOperationType clusterOperation;

  // This should be a globally unique name - it is a combination of the customer id and the universe
  // id. This is used as the prefix of node names in the universe.
  public String nodePrefix = null;

  // The UUID of the rootCA to be used to generate client certificates and facilitate TLS communication.
  public UUID rootCA = null;

  // This flag represents whether user has chosen to provide placement info
  // In Edit Universe if this flag is set we go through the NEW_CONFIG_FROM_PLACEMENT_INFO path
  public boolean userAZSelected = false;

  // Set to true if resetting Universe form (in EDIT mode), false otherwise.
  public boolean resetAZConfig = false;

  // TODO: Add a version number to prevent stale updates.
  // Set to true when an create/edit/destroy intent on the universe is started.
  public boolean updateInProgress = false;

  public boolean backupInProgress = false;

  // This tracks the if latest operation on this universe has successfully completed. This flag is
  // reset each time a new operation on the universe starts, and is set at the very end of that
  // operation.
  public boolean updateSucceeded = true;

  // The next cluster index to be used when a new read-only cluster is added.
  public int nextClusterIndex = 1;

  // Flag to mark if the universe was created with insecure connections allowed.
  // Ideally should be false since we would never want to allow insecure connections,
  // but defaults to true since we want universes created through pre-TLS YW to be
  // unaffected.
  public boolean allowInsecure = true;

  // Development flag to download package from s3 bucket.
  public String itestS3PackagePath = "";

  /**
   * Allowed states for an imported universe.
   */
  public enum ImportedState {
    NONE, // Default, and for non-imported universes.
    STARTED,
    MASTERS_ADDED,
    TSERVERS_ADDED,
    IMPORTED
  }

  // State of the imported universe.
  public ImportedState importedState = ImportedState.NONE;

  /**
   * Type of operations that can be performed on the universe.
   */
  public enum Capability {
    READ_ONLY,
    EDITS_ALLOWED // Default, and for non-imported universes.
  }

  // Capability of the universe.
  public Capability capability = Capability.EDITS_ALLOWED;

  /**
   * Types of Clusters that can make up a universe.
   */
  public enum ClusterType {
    PRIMARY, ASYNC
  }

  /**
   * A wrapper for all the clusters that will make up the universe.
   */
  public static class Cluster {
    public UUID uuid = UUID.randomUUID();
    public void setUuid(UUID uuid) { this.uuid = uuid;}

    // The type of this cluster.
    @Constraints.Required()
    public ClusterType clusterType;

    // The configuration for the universe the user intended.
    @Constraints.Required()
    public UserIntent userIntent;

    // The placement information computed from the user intent.
    public PlacementInfo placementInfo = null;

    // The cluster index by which node names are sorted when shown in UI.
    // This is set internally by the placement util in the server, client should not set it.
    public int index = 0;

    /**
     * Default to PRIMARY.
     */
    private Cluster() {
      this(ClusterType.PRIMARY, new UserIntent());
    }

    /**
     * @param clusterType One of [PRIMARY, ASYNC]
     * @param userIntent  Customized UserIntent describing the desired state of this Cluster.
     */
    public Cluster(ClusterType clusterType, UserIntent userIntent) {
      assert clusterType != null && userIntent != null;
      this.clusterType = clusterType;
      this.userIntent = userIntent;
    }

    public JsonNode toJson() {
      if (userIntent == null) {
        return null;
      }
      ObjectNode clusterJson = (ObjectNode) Json.toJson(this);
      if (userIntent.regionList != null && !userIntent.regionList.isEmpty()) {
        List<Region> regions = Region.find.query().where().idIn(userIntent.regionList).findList();
        if (!regions.isEmpty()) {
          clusterJson.set("regions", Json.toJson(regions));
        }
      }
      return clusterJson;
    }

    public boolean equals(Cluster other) {
      return uuid.equals(other.uuid);
    }

    /**
     * Check if instance tags are same as the passed in cluster.
     *
     * @param cluster another cluster to check against.
     * @return true if the tag maps are same for aws provider, false otherwise.
     */
    public boolean areTagsSame(Cluster cluster) {
      if (cluster == null) {
        throw new IllegalArgumentException("Invalid cluster to compare.");
      }

      if (!cluster.userIntent.providerType.equals(userIntent.providerType)) {
        throw new IllegalArgumentException("Mismatched provider types, expected " +
            userIntent.providerType.name() + " but got " + cluster.userIntent.providerType.name());
      }

      // We only deal with AWS instance tags.
      if (!userIntent.providerType.equals(CloudType.aws) ||
          userIntent.instanceTags.equals(cluster.userIntent.instanceTags)) {
        return true;
      }

      return false;
    }
  }

  /**
   * The user defined intent for the universe.
   */
  public static class UserIntent {
    @Constraints.Required()
    // Nice name for the universe.
    public String universeName;

    // The cloud provider UUID.
    public String provider;

    // The cloud provider type as an enum. This is set in the middleware from the provider UUID
    // field above.
    public CloudType providerType = CloudType.unknown;

    // The replication factor.
    @Constraints.Min(1)
    public int replicationFactor = 3;

    // The list of regions that the user wants to place data replicas into.
    public List<UUID> regionList;

    // The regions that the user wants to nominate as the preferred region. This makes sense only for
    // a multi-region setup.
    public UUID preferredRegion;

    // Cloud Instance Type that the user wants
    @Constraints.Required()
    public String instanceType;

    // The number of nodes to provision. These include ones for both masters and tservers.
    @Constraints.Min(1)
    public int numNodes;

    // The software version of YB to install.
    @Constraints.Required()
    public String ybSoftwareVersion;

    @Constraints.Required()
    public String accessKeyCode;

    public DeviceInfo deviceInfo;

    public boolean assignPublicIP = true;

    public boolean useTimeSync = false;

    public boolean enableYSQL = false;

    public boolean enableNodeToNodeEncrypt = false;

    public boolean enableClientToNodeEncrypt = false;

    public boolean enableVolumeEncryption = false;

    public String awsArnString;

    // When this is set to true, YW will setup the universe to communicate by way of hostnames
    // instead of ip addresses. These hostnames will have been provided during on-prem provider
    // setup and will be in-place of privateIP
    public boolean useHostname = false;

    // Info of all the gflags that the user would like to save to the universe. These will be
    // used during edit universe, for example, to set the flags on new nodes to match
    // existing nodes' settings.
    public Map<String, String> masterGFlags = new HashMap<String, String>();
    public Map<String, String> tserverGFlags = new HashMap<String, String>();

    // Instance tags (used for AWS only).
    public Map<String, String> instanceTags = new HashMap<String, String>();

    @Override
    public String toString() {
      return "UserIntent " + "for universe=" + universeName + " type=" +
             instanceType + ", numNodes=" + numNodes + ", prov=" + provider + ", provType=" +
             providerType + ", RF=" + replicationFactor + ", regions=" + regionList + ", pref=" +
             preferredRegion + ", ybVersion=" + ybSoftwareVersion + ", accessKey=" + accessKeyCode +
             ", deviceInfo='" + deviceInfo + "', timeSync=" + useTimeSync + ", publicIP=" +
             assignPublicIP + ", tags=" + instanceTags;
    }

    public UserIntent clone() {
      UserIntent newUserIntent = new UserIntent();
      newUserIntent.universeName = universeName;
      newUserIntent.provider = provider;
      newUserIntent.providerType = providerType;
      newUserIntent.replicationFactor = replicationFactor;
      newUserIntent.regionList = new ArrayList<>(regionList);
      newUserIntent.preferredRegion = preferredRegion;
      newUserIntent.instanceType = instanceType;
      newUserIntent.numNodes = numNodes;
      newUserIntent.ybSoftwareVersion = ybSoftwareVersion;
      newUserIntent.accessKeyCode = accessKeyCode;
      newUserIntent.assignPublicIP = assignPublicIP;
      newUserIntent.masterGFlags = new HashMap<>(masterGFlags);
      newUserIntent.tserverGFlags = new HashMap<>(tserverGFlags);
      newUserIntent.assignPublicIP = assignPublicIP;
      newUserIntent.useTimeSync = useTimeSync;
      newUserIntent.enableYSQL = enableYSQL;
      newUserIntent.enableNodeToNodeEncrypt = enableNodeToNodeEncrypt;
      newUserIntent.enableClientToNodeEncrypt = enableClientToNodeEncrypt;
      newUserIntent.instanceTags = new HashMap<>(instanceTags);
      return newUserIntent;
    }

    // NOTE: If new fields are checked, please add them to the toString() as well.
    public boolean equals(UserIntent other) {
      if (universeName.equals(other.universeName) &&
          provider.equals(other.provider) &&
          providerType == other.providerType &&
          replicationFactor == other.replicationFactor &&
          compareRegionLists(regionList, other.regionList) &&
          Objects.equals(preferredRegion, other.preferredRegion) &&
          instanceType.equals(other.instanceType) &&
          numNodes == other.numNodes &&
          ybSoftwareVersion.equals(other.ybSoftwareVersion) &&
          (accessKeyCode == null || accessKeyCode.equals(other.accessKeyCode)) &&
          assignPublicIP == other.assignPublicIP &&
          useTimeSync == other.useTimeSync) {
        return true;
      }
      return false;
    }

    public boolean onlyRegionsChanged(UserIntent other) {
      if (universeName.equals(other.universeName) &&
          provider.equals(other.provider) &&
          providerType == other.providerType &&
          replicationFactor == other.replicationFactor &&
          newRegionsAdded(regionList, other.regionList) &&
          Objects.equals(preferredRegion, other.preferredRegion) &&
          instanceType.equals(other.instanceType) &&
          numNodes == other.numNodes &&
          ybSoftwareVersion.equals(other.ybSoftwareVersion) &&
          (accessKeyCode == null || accessKeyCode.equals(other.accessKeyCode)) &&
          assignPublicIP == other.assignPublicIP &&
          useTimeSync == other.useTimeSync) {
        return true;
      }
      return false;
    }

    private static boolean newRegionsAdded(List<UUID> left, List<UUID> right) {
      return (new HashSet<>(left)).containsAll(new HashSet<>(right));
    }
    /**
     * Helper API to check if the set of regions is the same in two lists. Does not validate that
     * the UUIDs correspond to actual, existing Regions.
     *
     * @param left  First list of Region UUIDs.
     * @param right Second list of Region UUIDs.
     * @return true if the unordered, unique set of UUIDs is the same in both lists, else false.
     */
    private static boolean compareRegionLists(List<UUID> left, List<UUID> right) {
      return (new HashSet<>(left)).equals(new HashSet<>(right));
    }

    /**
     * Helper API to send the tags to be used for any instance level operation. The existing map is
     * cloned and modifications are performed on the clone. Currently it removes just the node name,
     * which should not be duplicated in ybcloud commands. Used only for AWS now.
     *
     * @return A map of tags to use.
     */
    @JsonIgnore
    public Map<String, String> getInstanceTagsForInstanceOps() {
      Map<String, String> retTags = new HashMap<String, String>();
      if (!providerType.equals(Common.CloudType.aws)) {
        return retTags;
      }

      retTags.putAll(instanceTags);
      retTags.remove(UniverseDefinitionTaskBase.NODE_NAME_KEY);

      return retTags;
    }
  }

  @JsonIgnore
  // Returns true if universe object is in any known import related state.
  public boolean isImportedUniverse() {
    return importedState != ImportedState.NONE;
  }

  @JsonIgnore
  // Returns true if universe is allowed edits or node-actions.
  public boolean isUniverseEditable() {
    return capability == Capability.EDITS_ALLOWED;
  }

  /**
   * Helper API to remove node from nodeDetailSet
   *
   * @param nodeName Name of a particular node to remove from the containing nodeDetailsSet.
   */
  public void removeNode(String nodeName) {
    if (nodeDetailsSet != null) {
      nodeDetailsSet.removeIf(node -> node.nodeName.equals(nodeName));
    }
  }

  /**
   * Add a primary cluster with the specified UserIntent and PlacementInfo to the list of clusters
   * if one does not already exist. Otherwise, update the existing primary cluster with the
   * specified UserIntent and PlacementInfo.
   *
   * @param userIntent UserIntent describing the primary cluster.
   * @param placementInfo PlacementInfo describing the placement of the primary cluster.
   * @return the updated/inserted primary cluster.
   */
  public Cluster upsertPrimaryCluster(UserIntent userIntent, PlacementInfo placementInfo) {
    Cluster primaryCluster = getPrimaryCluster();
    if (primaryCluster != null) {
      if (userIntent != null) {
        primaryCluster.userIntent = userIntent;
      }
      if (placementInfo != null) {
        primaryCluster.placementInfo = placementInfo;
      }
    } else {
      primaryCluster = new Cluster(ClusterType.PRIMARY, (userIntent == null) ? new UserIntent() : userIntent);
      primaryCluster.placementInfo = placementInfo;
      clusters.add(primaryCluster);
    }
    return primaryCluster;
  }

  /**
   * Add a cluster with the specified UserIntent, PlacementInfo, and uuid to the list of clusters
   * if one does not already exist. Otherwise, update the existing cluster with the
   * specified UserIntent and PlacementInfo.
   *
   * @param userIntent UserIntent describing the cluster.
   * @param placementInfo PlacementInfo describing the placement of the cluster.
   * @param clusterUuid uuid of the cluster we want to change.
   * @return the updated/inserted cluster.
   */
  public Cluster upsertCluster(UserIntent userIntent, PlacementInfo placementInfo,
                               UUID clusterUuid) {
    Cluster cluster = getClusterByUuid(clusterUuid);
    if (cluster != null) {
      if (userIntent != null) {
        cluster.userIntent = userIntent;
      }
      if (placementInfo != null) {
        cluster.placementInfo = placementInfo;
      }
    } else {
      cluster = new Cluster(ClusterType.ASYNC, userIntent == null ? new UserIntent() : userIntent);
      cluster.placementInfo = placementInfo;
      clusters.add(cluster);
    }
    cluster.setUuid(clusterUuid);
    return cluster;
  }

  /**
   * Delete a cluster with the specified uuid from the list of clusters.
   *
   * @param clusterUuid the uuid of the cluster we are deleting.
   */
  public void deleteCluster(UUID clusterUuid) {
    Cluster cluster = getClusterByUuid(clusterUuid);
    if (cluster == null) {
      throw new IllegalArgumentException("UUID " + clusterUuid + " not found in universe " +
                                         universeUUID);
    }

    clusters.remove(cluster);
  }

  /**
   * Helper API to retrieve the Primary Cluster in the Universe represented by these Params.
   *
   * @return the Primary Cluster in the Universe represented by these Params or null if none exists.
   */
  @JsonIgnore
  public Cluster getPrimaryCluster() {
     List<Cluster> foundClusters = clusters.stream()
                                           .filter(c -> c.clusterType.equals(ClusterType.PRIMARY))
                                           .collect(Collectors.toList());
     if (foundClusters.size() > 1) {
       throw new RuntimeException("Multiple primary clusters found in params for universe " +
                                  universeUUID.toString());
     }
     return Iterables.getOnlyElement(foundClusters, null);
  }

  @JsonIgnore
  public Set<NodeDetails> getTServers() {
    Set<NodeDetails> Tservers = new HashSet<>();
    for (NodeDetails n : nodeDetailsSet) {
      if (n.isTserver){
        Tservers.add(n);
      }
    }
    return Tservers;
  }

  /**
   * Helper API to retrieve all ReadOnly Clusters in the Universe represented by these Params.
   *
   * @return a list of all ReadOnly Clusters in the Universe represented by these Params.
   */
  @JsonIgnore
  public List<Cluster> getReadOnlyClusters() {
    return clusters.stream()
                   .filter(c -> c.clusterType.equals(ClusterType.ASYNC))
                   .collect(Collectors.toList());
  }

  /**
   * Helper API to retrieve the Cluster in the Universe corresponding to the given UUID.
   *
   * @param uuid UUID of the Cluster to retrieve.
   * @return The Cluster corresponding to the provided UUID or null if none exists.
   */
  @JsonIgnore
  public Cluster getClusterByUuid(UUID uuid) {
    if (uuid == null) {
      return getPrimaryCluster();
    }
    List<Cluster> foundClusters =  clusters.stream()
                                           .filter(c -> c.uuid.equals(uuid))
                                           .collect(Collectors.toList());
    if (foundClusters.size() > 1) {
      throw new RuntimeException("Multiple clusters with uuid " + uuid.toString() +
          " found in params for universe " + universeUUID.toString());
    }
    return Iterables.getOnlyElement(foundClusters, null);
  }

  /**
   * Helper API to retrieve nodes that are in a specified cluster.
   * @param uuid UUID of the cluster that we want nodes from.
   * @return A Set of NodeDetails that are in the specified cluster.
   */
  @JsonIgnore
  public Set<NodeDetails> getNodesInCluster(UUID uuid) {
    if (nodeDetailsSet == null) return null;
    return nodeDetailsSet.stream()
                         .filter(n -> n.isInPlacement(uuid))
                         .collect(Collectors.toSet());
  }
}
