// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
import java.util.Set;
import java.util.Queue;
import java.util.UUID;
import java.util.stream.Collectors;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.base.Joiner;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import org.joda.time.DateTime;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.common.utils.Pair;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterOperationType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.CloudSpecificInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementAZ;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementCloud;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementRegion;
import play.libs.Json;

import static java.util.stream.Collectors.groupingBy;
import static java.util.stream.Collectors.toCollection;

import static com.yugabyte.yw.common.Util.toBeAddedAzUuidToNumNodes;
import static com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType.ASYNC;
import static com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType.PRIMARY;


public class PlacementInfoUtil {
  public static final Logger LOG = LoggerFactory.getLogger(PlacementInfoUtil.class);

  // This is the maximum number of subnets that the masters can be placed across, and need to be an
  // odd number for consensus to work.
  private static final int maxMasterSubnets = 3;

  // List of replication factors supported currently for primary cluster.
  private static final List<Integer> supportedRFs = ImmutableList.of(1, 3, 5, 7);

  // List of replication factors supported currently for read only clusters.
  private static final List<Integer> supportedReadOnlyRFs = ImmutableList.of(1, 2, 3, 4, 5, 6, 7);

  // Constants used to determine tserver, master, and node liveness.
  public static final String UNIVERSE_ALIVE_METRIC = "node_up";

  // Maximum number of zones that are used for placement, for display with UI. Independent of
  // replication factor for now.
  private static final int MAX_ZONES = 5;


  // Mode of node distribution across the given AZ configuration.
  enum ConfigureNodesMode {
    NEW_CONFIG,                       // Round robin nodes across server chosen AZ placement.
    UPDATE_CONFIG_FROM_USER_INTENT,   // Use numNodes from userIntent with user chosen AZ's.
    UPDATE_CONFIG_FROM_PLACEMENT_INFO, // Use the placementInfo as per user chosen AZ distribution.
    NEW_CONFIG_FROM_PLACEMENT_INFO   // Create a move configuration using placement info
  }

  /**
   * Method to check whether the affinitized leaders info changed between the old and new
   * placements. If an AZ is present in the new placement but not the old or vice versa,
   * returns false.
   *
   * @param oldPlacementInfo Placement for the previous state of the Cluster.
   * @param newPlacementInfo Desired placement for the Cluster.
   * @return True if the affinitized leader info has changed and there are no new AZs, else false.
   */
  public static boolean didAffinitizedLeadersChange(PlacementInfo oldPlacementInfo,
                                                    PlacementInfo newPlacementInfo) {

    // Map between the old placement's AZs and the affinitized leader info.
    HashMap<UUID, Boolean> oldAZMap = new HashMap<UUID, Boolean>();

    for (PlacementCloud oldCloud : oldPlacementInfo.cloudList) {
      for (PlacementRegion oldRegion : oldCloud.regionList) {
        for (PlacementAZ oldAZ : oldRegion.azList) {
          oldAZMap.put(oldAZ.uuid, oldAZ.isAffinitized);
        }
      }
    }

    for (PlacementCloud newCloud : newPlacementInfo.cloudList) {
      for (PlacementRegion newRegion : newCloud.regionList) {
        for (PlacementAZ newAZ : newRegion.azList) {
          if (!oldAZMap.containsKey(newAZ.uuid)) {
            return false;
          }
          if (oldAZMap.get(newAZ.uuid) != newAZ.isAffinitized) {
            // affinitized leader info has changed, return true.
            return true;
          }
        }
      }
    }
    // No affinitized leader info has changed, return false.
    return false;
  }

  /**
   * Returns the PlacementAZ object with the specified UUID if exists in the provided PlacementInfo,
   * else null.
   *
   * @param placementInfo PlacementInfo object to look through for the desired PlacementAZ.
   * @param azUUID        UUID of the PlacementAZ to look for.
   * @return The specified PlacementAZ if it exists, else null.
   */
  private static PlacementAZ findPlacementAzByUuid(PlacementInfo placementInfo, UUID azUUID) {
    for (PlacementCloud cloud : placementInfo.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        for (PlacementAZ az : region.azList) {
          if (az.uuid.equals(azUUID)) {
            return az;
          }
        }
      }
    }
    return null;
  }

  // Helper function to reset az node counts before a full move.
  private static void clearPlacementAZCounts(PlacementInfo placementInfo) {
    for (PlacementCloud cloud : placementInfo.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        for (PlacementAZ az : region.azList) {
          LOG.info("Clearing {} count {}.", az.name, az.numNodesInAZ);
          az.numNodesInAZ = 0;
        }
      }
    }
  }

  /**
   * Method to check if the given set of nodes match the placement info AZ to detect pure
   * expand/shrink type.
   * The following cases are _not_ considered Expand/Shrink and will be treated as full move.
   * USWest1A - 3 , USWest1B - 1 => USWest1A - 2, USWest1B - 1  IFF all 3 in USWest1A are Masters.
   * USWest1A - 3, USWest1B - 3 => USWest1A - 6.
   * Basically any change in AZ will be treated as full move.
   * Only if number of nodes is increased/decreased and only if there are enough (non-master)
   * tserver only nodes to accomodate the change will it be treated as pure expand/shrink.
   * In Edit case is that the existing user intent should also be compared to new intent.
   * And for both edit and create, we need to compare previously chosen AZ's against the new
   * placementInfo.
   *
   * @param oldParams   Not null iff it is an edit op, so need to honor existing nodes/tservers.
   * @param newParams   Current user task and placement info with user proposed AZ distribution.
   * @param cluster     Cluster to check.
   * @return If the number of nodes only are changed across AZ's in placement or if userIntent
   *         node count only changes, returns that configure mode. Else defaults to new config.
   */
  private static ConfigureNodesMode getPureExpandOrShrinkMode(UniverseDefinitionTaskParams oldParams,
                                                              UniverseDefinitionTaskParams newParams,
                                                              Cluster cluster) {
    boolean isEditUniverse = oldParams != null;
    PlacementInfo newPlacementInfo = cluster.placementInfo;
    UserIntent newIntent = cluster.userIntent;
    Collection<NodeDetails> nodeDetailsSet = isEditUniverse ?
            oldParams.getNodesInCluster(cluster.uuid) :
            newParams.getNodesInCluster(cluster.uuid);

    // If it's an EditUniverse operation, check if old and new intents are equal.
    if (isEditUniverse) {
      UserIntent existingIntent = oldParams.getClusterByUuid(cluster.uuid).userIntent;
      LOG.info("Comparing task '{}' and existing '{}' intents.", newIntent, existingIntent);
      UserIntent tempIntent = newIntent.clone();
      tempIntent.numNodes = existingIntent.numNodes;
      if (!tempIntent.equals(existingIntent)) {
        if (tempIntent.onlyRegionsChanged(existingIntent)) {
          LOG.info("Only new regions are added, so we will do expand");
          return ConfigureNodesMode.UPDATE_CONFIG_FROM_USER_INTENT;
        }
        return ConfigureNodesMode.NEW_CONFIG;
      }
    }

    // Check if at least one AZ's num nodes count has changed
    boolean atLeastOneCountChanged = false;
    Map<UUID, Integer> azUuidToNumNodes = getAzUuidToNumNodes(nodeDetailsSet);
    for (UUID azUuid : azUuidToNumNodes.keySet()) {
      PlacementAZ az = findPlacementAzByUuid(newPlacementInfo, azUuid);
      if (az == null) {
        LOG.info("AZ {} not found in placement, so not pure expand/shrink.", azUuid);
        return ConfigureNodesMode.NEW_CONFIG;
      }
      int numTservers = findCountActiveTServerOnlyInAZ(nodeDetailsSet, azUuid);
      int azDifference = az.numNodesInAZ - azUuidToNumNodes.get(azUuid);
      LOG.info("AZ {} check, azNum={}, azDiff={}, numTservers={}.", az.name, az.numNodesInAZ,
              azDifference, numTservers);
      if (azDifference != 0) {
        atLeastOneCountChanged = true;
      }
      if (isEditUniverse && azDifference < 0  && -azDifference > numTservers) {
        return ConfigureNodesMode.NEW_CONFIG;
      }
    }

    // Log some information about the placement and type of edit/create we're doing.
    int placementCount = getNodeCountInPlacement(newPlacementInfo);
    if (isEditUniverse) {
      LOG.info("Edit taskNumNodes={}, placementNumNodes={}, numNodes={}.", newIntent.numNodes,
              placementCount, nodeDetailsSet.size());
    } else {
      LOG.info("Create taskNumNodes={}, placementNumNodes={}.", newIntent.numNodes, placementCount);
    }

    // If we made it this far, then we are in a pure expand/shrink mode. Check if the UserIntent
    // numNodes matches the sum of nodes in the placement and count changed on the per AZ placement.
    // Else, number of nodes in the UserIntent should be honored and the perAZ node count ignored.
    ConfigureNodesMode mode = ConfigureNodesMode.NEW_CONFIG;
    if (newIntent.numNodes == placementCount && atLeastOneCountChanged) {
      mode = ConfigureNodesMode.UPDATE_CONFIG_FROM_PLACEMENT_INFO;
    } else if (newIntent.numNodes != placementCount) {
      mode = ConfigureNodesMode.UPDATE_CONFIG_FROM_USER_INTENT;
    }
    LOG.info("Pure expand/shrink in {} mode.", mode);
    return mode;
  }

  // Assumes that there is only single provider across all nodes in a given set.
  private static UUID getProviderUUID(Collection<NodeDetails> nodes, UUID placementUuid) {
    if (nodes == null || nodes.isEmpty()) {
      return null;
    }
    NodeDetails node = nodes.stream()
            .filter(n -> n.isInPlacement(placementUuid))
            .findFirst()
            .orElse(null);
    return (node == null) ? null : AvailabilityZone.get(node.azUuid).region.provider.uuid;
  }

  private static Set<UUID> getAllRegionUUIDs(Collection<NodeDetails> nodes, UUID placementUuid) {
    Set<UUID> nodeRegionSet = new HashSet<>();
    nodes.stream()
            .filter(n -> n.isInPlacement(placementUuid))
            .forEach(n -> nodeRegionSet.add(AvailabilityZone.get(n.azUuid).region.uuid));
    return nodeRegionSet;
  }

  /**
   * Helper API to check if the list of regions is the same in existing nodes of the placement
   * and the new userIntent's region list.
   *
   * @param cluster The current user proposed cluster.
   * @param nodes The set of nodes used to compare the current region layout.
   * @return true if the provider or region list changed. false if neither changed.
   */
  private static boolean isProviderOrRegionChange(Cluster cluster, Collection<NodeDetails> nodes) {
    // Initial state. No nodes have been requested, so nothing has changed.
    if (nodes.isEmpty()) {
      return false;
    }

    // Compare Providers.
    UUID intentProvider = getProviderUUID(nodes, cluster.uuid);
    UUID nodeProvider = cluster.placementInfo.cloudList.get(0).uuid;
    if (!intentProvider.equals(nodeProvider)) {
      LOG.info("Provider in intent {} is different from provider in existing nodes {} in cluster {}.",
              intentProvider, nodeProvider, cluster.uuid);
      return true;
    }

    // Compare Regions.
    Set<UUID> nodeRegionSet = getAllRegionUUIDs(nodes, cluster.uuid);
    Set<UUID> intentRegionSet = new HashSet<>(cluster.userIntent.regionList);
    LOG.info("Intended Regions {} vs existing Regions {} in cluster {}.",
            intentRegionSet, nodeRegionSet, cluster.uuid);
    return !intentRegionSet.equals(nodeRegionSet);
  }

  public static int getNodeCountInPlacement(PlacementInfo placementInfo) {
    int count = 0;
    for (PlacementCloud cloud : placementInfo.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        for (PlacementAZ az : region.azList) {
          count += az.numNodesInAZ;
        }
      }
    }
    return count;
  }

  // Helper API to catch duplicated node names in the given set of nodes.
  public static void ensureUniqueNodeNames(Collection<NodeDetails> nodes) throws RuntimeException {
    boolean foundDups = false;
    Set<String> nodeNames = new HashSet<String>();

    for (NodeDetails node : nodes) {
      if (nodeNames.contains(node.nodeName)) {
        LOG.error("Duplicate nodeName {}.", node.nodeName);
        foundDups = true;
      } else {
        nodeNames.add(node.nodeName);
      }
    }

    if (foundDups) {
      throw new RuntimeException("Found duplicated node names, error info in logs just above.");
    }
  }

  // Helper API to order the read-only clusters for naming purposes.
  public static void populateClusterIndices(UniverseDefinitionTaskParams taskParams) {
    for (Cluster cluster : taskParams.getReadOnlyClusters()) {
      if (cluster.index == 0) {
        // The cluster index isn't set, which means its a new cluster, set the cluster
        // index and increment the global max.
        cluster.index = taskParams.nextClusterIndex++;
      }
    }
  }

  // Calculate the number of zones covered by the given placement info.
  private static int getNumZones(PlacementInfo placementInfo) {
   int numZones = 0;
   for (PlacementCloud cloud : placementInfo.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        numZones += region.azList.size();
      }
    }
    return numZones;
  }

  /**
   * Helper API to set some of the non user supplied information in task params.
   * @param taskParams : Universe task params.
   * @param customerId : Current customer's id.
   * @param placementUuid : uuid of the cluster user is working on.
   * @param clusterOpType : Cluster Operation Type (CREATE/EDIT)
   */
  public static void updateUniverseDefinition(UniverseDefinitionTaskParams taskParams,
                                              Long customerId,
                                              UUID placementUuid,
                                              ClusterOperationType clusterOpType) {
    Cluster cluster = taskParams.getClusterByUuid(placementUuid);

    // Create node details set if needed.
    if (taskParams.nodeDetailsSet == null) {
      taskParams.nodeDetailsSet = new HashSet<>();
    }

    Universe universe = null;
    if (taskParams.universeUUID == null) {
      taskParams.universeUUID = UUID.randomUUID();
    } else {
      try {
        universe = Universe.get(taskParams.universeUUID);
      } catch (Exception e) {
        LOG.info("Universe with UUID {} not found, configuring new universe.",
                 taskParams.universeUUID);
      }
    }

    String universeName = universe == null ?
        taskParams.getPrimaryCluster().userIntent.universeName :
        universe.getUniverseDetails().getPrimaryCluster().userIntent.universeName;

    // Reset the config and AZ configuration
    if (taskParams.resetAZConfig) {

      if (clusterOpType.equals(ClusterOperationType.EDIT)) {
        UniverseDefinitionTaskParams details = universe.getUniverseDetails();
        // Set AZ and clusters to original values prior to editing
        taskParams.clusters = details.clusters;
        taskParams.nodeDetailsSet = details.nodeDetailsSet;
        // Set this flag to false to avoid continuous resetting of the form
        taskParams.resetAZConfig = false;
        return;
      } else if (clusterOpType.equals(ClusterOperationType.CREATE)) {
        taskParams.nodeDetailsSet.removeIf(n -> n.isInPlacement(placementUuid));
        // Set node count equal to RF
        cluster.userIntent.numNodes = cluster.userIntent.replicationFactor;
        cluster.placementInfo = cluster.placementInfo == null ?
                getPlacementInfo(cluster.clusterType, cluster.userIntent) :
                cluster.placementInfo;
        LOG.info("Placement created={}.", cluster.placementInfo);
        configureNodeStates(taskParams, null, ConfigureNodesMode.NEW_CONFIG, cluster);
        return;
      }
    }

    // Compose a default prefix for the nodes. This can be overwritten by instance tags (on AWS).
    taskParams.nodePrefix = Util.getNodePrefix(customerId, universeName);

    ConfigureNodesMode mode;
    // If no placement info, and if this is the first primary or readonly cluster create attempt,
    // choose a new placement.
    if (cluster.placementInfo == null &&
        clusterOpType.equals(ClusterOperationType.CREATE)) {
      taskParams.nodeDetailsSet.removeIf(n -> n.isInPlacement(placementUuid));
      cluster.placementInfo = getPlacementInfo(cluster.clusterType, cluster.userIntent);
      LOG.info("Placement created={}.", cluster.placementInfo);
      configureNodeStates(taskParams, null, ConfigureNodesMode.NEW_CONFIG, cluster);
      return;
    }

    // Verify the provided edit parameters, if in edit universe case, and get the mode.
    // Otherwise it is a primary or readonly cluster creation phase changes.
    LOG.info("Placement={}, numNodes={}, AZ={}.", cluster.placementInfo,
             taskParams.nodeDetailsSet.size(), taskParams.userAZSelected);

    // If user AZ Selection is made for Edit get a new configuration from placement info.
    if (taskParams.userAZSelected && universe != null) {
      mode = ConfigureNodesMode.NEW_CONFIG_FROM_PLACEMENT_INFO;
      configureNodeStates(taskParams, universe, mode, cluster);
      return;
    }

    Cluster oldCluster = null;
    if (clusterOpType.equals(ClusterOperationType.EDIT)) {
      if (universe == null) {
        throw new IllegalArgumentException("Cannot perform edit operation on " +
            cluster.clusterType + " without universe.");
      }

      oldCluster = cluster.clusterType.equals(PRIMARY) ?
          universe.getUniverseDetails().getPrimaryCluster() :
          universe.getUniverseDetails().getReadOnlyClusters().get(0);

      if (!oldCluster.uuid.equals(placementUuid)) {
        throw new IllegalArgumentException("Mismatched uuid between existing cluster " +
            oldCluster.uuid + " and requested " + placementUuid + " for " + cluster.clusterType +
            " cluster in universe " + universe.universeUUID);
       }

      verifyEditParams(oldCluster, cluster);
    }

    boolean primaryClusterEdit = cluster.clusterType.equals(PRIMARY) &&
        clusterOpType.equals(ClusterOperationType.EDIT);
    boolean readOnlyClusterEdit = cluster.clusterType.equals(ASYNC) &&
        clusterOpType.equals(ClusterOperationType.EDIT);

    LOG.info("Update mode info: primEdit={}, roEdit={}, opType={}, cluster={}.",
             primaryClusterEdit, readOnlyClusterEdit, clusterOpType, cluster.clusterType);

    // For primary cluster edit only there is possibility of leader changes.
    if (primaryClusterEdit) {
      assert oldCluster != null;
      if (didAffinitizedLeadersChange(oldCluster.placementInfo,
                                      cluster.placementInfo)) {
        mode = ConfigureNodesMode.UPDATE_CONFIG_FROM_PLACEMENT_INFO;
      } else {
        mode = getPureExpandOrShrinkMode(universe.getUniverseDetails(), taskParams, cluster);
        taskParams.nodeDetailsSet.clear();
        taskParams.nodeDetailsSet.addAll(universe.getNodes());
      }
    } else {
      if (readOnlyClusterEdit) {
        assert oldCluster != null;
      }
      mode = getPureExpandOrShrinkMode(readOnlyClusterEdit ? universe.getUniverseDetails() : null,
                                       taskParams, cluster);
    }

    // If RF is not compatible with number of placement zones, reset the placement during create.
    // For ex., when RF goes from 3/5/7 to 1, we want to show only one zone. When RF goes from 1
    // to 5, we will show existing zones. ResetConfig can be used to change zone count.
    boolean mode_changed = false;
    if (clusterOpType.equals(ClusterOperationType.CREATE)) {
      int totalRF = 0;
      // Compute RF across all chosen zones to be <= user RF.
      for (PlacementCloud cloud : cluster.placementInfo.cloudList) {
        for (PlacementRegion region : cloud.regionList) {
          for (PlacementAZ az : region.azList) {
            totalRF += az.replicationFactor;
          }
        }
      }
      int rf = cluster.userIntent.replicationFactor;
      LOG.info("UserIntent replication factor={} while total zone replication factor={}.",
                rf, totalRF);
      if (rf < totalRF) {
        cluster.placementInfo = getPlacementInfo(cluster.clusterType, cluster.userIntent);
        LOG.info("New placement has {} zones.", getNumZones(cluster.placementInfo));
        taskParams.nodeDetailsSet.removeIf(n -> n.isInPlacement(placementUuid));
        mode = ConfigureNodesMode.NEW_CONFIG;
        mode_changed = true;
      }
    }

    // If not a pure expand/shrink, we will pick a new set of nodes. If the provider or region list
    // changed, we will pick a new placement (i.e full move, create primary/RO cluster).
    if (!mode_changed && (mode == ConfigureNodesMode.NEW_CONFIG)) {
      boolean changeNodeStates = false;
      if (isProviderOrRegionChange(
              cluster,
              (primaryClusterEdit || readOnlyClusterEdit) ?
                  universe.getNodes() : taskParams.nodeDetailsSet)) {
        LOG.info("Provider or region changed, getting new placement info.");
        cluster.placementInfo = getPlacementInfo(cluster.clusterType, cluster.userIntent);
        changeNodeStates = true;
      } else {
        String newInstType = cluster.userIntent.instanceType;
        String existingInstType =
            taskParams.getNodesInCluster(cluster.uuid).iterator().next().cloudInfo.instance_type;
        if (!newInstType.equals(existingInstType)) {
          LOG.info("Performing full move with existing placement info for instance type change " +
                   "from  {} to {}.", existingInstType, newInstType);
          clearPlacementAZCounts(cluster.placementInfo);
          changeNodeStates = true;
        } // else can check if RF changed : not supported yet.
      }

      if (!changeNodeStates && oldCluster != null && !oldCluster.areTagsSame(cluster)) {
        LOG.info("No node config change needed, only instance tags changed.");
        return;
      }

      taskParams.nodeDetailsSet.removeIf(n -> n.isInPlacement(placementUuid));
    }

    // Compute the node states that should be configured for this operation.
    configureNodeStates(taskParams, universe, mode, cluster);
  }

  /**
   * Method checks if enough nodes have been configured to satiate the userIntent for an OnPrem configuration
   * @param taskParams Universe task params.
   * @param cluster Cluster to be checked.
   * @return True if provider type is not OnPrem or if enough nodes have been configured for intent, false otherwise
   */
  public static boolean checkIfNodeParamsValid(UniverseDefinitionTaskParams taskParams, Cluster cluster) {
    if (cluster.userIntent.providerType != CloudType.onprem) {
      return true;
    }
    UserIntent userIntent = cluster.userIntent;
    String instanceType = userIntent.instanceType;
    Set<NodeDetails> clusterNodes = taskParams.getNodesInCluster(cluster.uuid);
    // If NodeDetailsSet is null, do a high level check whether number of nodes in given config is present
    if (clusterNodes == null || clusterNodes.isEmpty()) {
      int totalNodesConfiguredInRegionList = 0;
      // Check if number of nodes in the user intent is greater than number of nodes configured for given instance type
      for (UUID regionUUID: userIntent.regionList) {
        for (AvailabilityZone az: Region.get(regionUUID).zones) {
          totalNodesConfiguredInRegionList += NodeInstance.listByZone(az.uuid, instanceType).size();
        }
      }
      if (totalNodesConfiguredInRegionList < userIntent.numNodes) {
        LOG.error("Not enough nodes, required: {} nodes, configured: {} nodes", userIntent.numNodes, totalNodesConfiguredInRegionList);
        return false;
      }
    } else {
      // If NodeDetailsSet is non-empty verify that the node/az combo is valid
      for (Map.Entry<UUID, Integer> entry : toBeAddedAzUuidToNumNodes(clusterNodes).entrySet()) {
        UUID azUUID = entry.getKey();
        int numNodesToBeAdded = entry.getValue().intValue();
        if (numNodesToBeAdded > NodeInstance.listByZone(azUUID, instanceType).size()) {
          LOG.error("Not enough nodes configured for given AZ/Instance type combo, " +
                          "required {} found {} in AZ {} for Instance type {}", numNodesToBeAdded, NodeInstance.listByZone(azUUID, instanceType).size(),
                  azUUID, instanceType);
          return false;
        }
      }
    }
    return true;
  }

  public static void updatePlacementInfo(Collection<NodeDetails> nodes,
                                         PlacementInfo placementInfo) {
    if (nodes != null && placementInfo != null) {
      Map<UUID, Integer> azUuidToNumNodes = getAzUuidToNumNodes(nodes, true);
      for (int cIdx = 0; cIdx < placementInfo.cloudList.size(); cIdx++) {
        PlacementCloud cloud = placementInfo.cloudList.get(cIdx);
        for (int rIdx = 0; rIdx < cloud.regionList.size(); rIdx++) {
          PlacementRegion region = cloud.regionList.get(rIdx);
          for (int azIdx = 0; azIdx < region.azList.size(); azIdx++) {
            PlacementAZ az = region.azList.get(azIdx);
            if (azUuidToNumNodes.get(az.uuid) != null) {
              LOG.info("Update {} {}.", az.name, az.numNodesInAZ, azUuidToNumNodes.get(az.uuid));
              az.numNodesInAZ = azUuidToNumNodes.get(az.uuid);
            } else {
              region.azList.remove(az);
              azIdx--;
            }
          }
          if (region.azList.isEmpty()) {
            cloud.regionList.remove(region);
            rIdx--;
          }
        }
      }
    }
  }

  public static Set<NodeDetails> getMastersToBeRemoved(Set<NodeDetails> nodeDetailsSet) {
    return getServersToBeRemoved(nodeDetailsSet, ServerType.MASTER);
  }

  public static Set<NodeDetails> getTserversToBeRemoved(Set<NodeDetails> nodeDetailsSet) {
    return getServersToBeRemoved(nodeDetailsSet, ServerType.TSERVER);
  }

  private static Set<NodeDetails> getServersToBeRemoved(Set<NodeDetails> nodeDetailsSet,
                                                        ServerType serverType) {
    Set<NodeDetails> servers = new HashSet<NodeDetails>();

    for (NodeDetails node : nodeDetailsSet) {
      if (node.state == NodeDetails.NodeState.ToBeRemoved &&
          (serverType == ServerType.MASTER && node.isMaster ||
           serverType == ServerType.TSERVER && node.isTserver)) {
        servers.add(node);
      }
    }

    return servers;
  }

  public static Set<NodeDetails> getNodesToBeRemoved(Set<NodeDetails> nodeDetailsSet) {
    Set<NodeDetails> servers = new HashSet<NodeDetails>();

    for (NodeDetails node : nodeDetailsSet) {
      if (node.state == NodeDetails.NodeState.ToBeRemoved) {
        servers.add(node);
      }
    }

    return servers;
  }

  public static Set<NodeDetails> getLiveNodes(Set<NodeDetails> nodeDetailsSet) {
    return nodeDetailsSet.stream()
                         .filter(n -> n.state.equals(NodeDetails.NodeState.Live))
                         .collect(Collectors.toSet());
  }

  public static Set<NodeDetails> getNodesToProvision(Set<NodeDetails> nodeDetailsSet) {
    return getServersToProvision(nodeDetailsSet, ServerType.EITHER);
  }

  public static Set<NodeDetails> getMastersToProvision(Set<NodeDetails> nodeDetailsSet) {
    return getServersToProvision(nodeDetailsSet, ServerType.MASTER);
  }

  public static Set<NodeDetails> getTserversToProvision(Set<NodeDetails> nodeDetailsSet) {
    return getServersToProvision(nodeDetailsSet, ServerType.TSERVER);
  }

  private static Set<NodeDetails> getServersToProvision(Set<NodeDetails> nodeDetailsSet,
                                                        ServerType serverType) {
    Set<NodeDetails> nodesToProvision = new HashSet<NodeDetails>();
    for (NodeDetails node : nodeDetailsSet) {
      if (node.state == NodeDetails.NodeState.ToBeAdded &&
          (serverType == ServerType.EITHER ||
           serverType == ServerType.MASTER && node.isMaster ||
           serverType == ServerType.TSERVER && node.isTserver)) {
        nodesToProvision.add(node);
      }
    }
    return nodesToProvision;
  }

  private static class AZInfo {
    public AZInfo(boolean affinitized, int num) {
      isAffinitized = affinitized;
      numNodes = num;
    }

    public boolean isAffinitized;
    public int numNodes;
  }

  // Helper function to check if the old placement and new placement after edit
  // are the same.
  private static boolean isSamePlacement(PlacementInfo oldPlacementInfo,
                                         PlacementInfo newPlacementInfo) {
    HashMap<UUID, AZInfo> oldAZMap = new HashMap<UUID, AZInfo>();

    for (PlacementCloud oldCloud : oldPlacementInfo.cloudList) {
      for (PlacementRegion oldRegion : oldCloud.regionList) {
        for (PlacementAZ oldAZ : oldRegion.azList) {
          oldAZMap.put(oldAZ.uuid, new AZInfo(oldAZ.isAffinitized, oldAZ.numNodesInAZ));
        }
      }
    }

    for (PlacementCloud newCloud : newPlacementInfo.cloudList) {
      for (PlacementRegion newRegion : newCloud.regionList) {
        for (PlacementAZ newAZ : newRegion.azList) {
          if (!oldAZMap.containsKey(newAZ.uuid)) {
            return false;
          }
          AZInfo azInfo = oldAZMap.get(newAZ.uuid);
          if (azInfo.isAffinitized != newAZ.isAffinitized ||
              azInfo.numNodes != newAZ.numNodesInAZ) {
            return false;
          }
        }
      }
    }
    return true;
  }

  /**
   * Verify that the planned changes for an Edit Universe operation are allowed.
   *
   * @param oldCluster    The current (soon to be old) version of a cluster.
   * @param newCluster    The intended next version of the same cluster.
   */
  private static void verifyEditParams(Cluster oldCluster, Cluster newCluster) {
    UserIntent existingIntent = oldCluster.userIntent;
    UserIntent userIntent = newCluster.userIntent;
    LOG.info("old intent: {}", existingIntent.toString());
    LOG.info("new intent: {}", userIntent.toString());
    // Error out if no fields are modified.
    if (userIntent.equals(existingIntent) &&
        userIntent.instanceTags.equals(existingIntent.instanceTags) &&
        isSamePlacement(oldCluster.placementInfo, newCluster.placementInfo)) {
      LOG.error("No fields were modified for edit universe.");
      throw new IllegalArgumentException("Invalid operation: At least one field should be " +
                                         "modified for editing the universe.");
    }

    // Rule out some of the universe changes that we do not allow (they can be enabled as needed).
    if (oldCluster.clusterType != newCluster.clusterType) {
      LOG.error("Cluster type cannot be changed from {} to {}",
                oldCluster.clusterType, newCluster.clusterType);
      throw new UnsupportedOperationException("Cluster type cannot be modified.");
    }

    if (existingIntent.replicationFactor != userIntent.replicationFactor) {
      LOG.error("Replication factor cannot be changed from {} to {}",
              existingIntent.replicationFactor, userIntent.replicationFactor);
      throw new UnsupportedOperationException("Replication factor cannot be modified.");
    }

    if (!existingIntent.universeName.equals(userIntent.universeName)) {
      LOG.error("universeName cannot be changed from {} to {}",
              existingIntent.universeName, userIntent.universeName);
      throw new UnsupportedOperationException("Universe name cannot be modified.");
    }

    if (!existingIntent.provider.equals(userIntent.provider)) {
      LOG.error("Provider cannot be changed from {} to {}",
                existingIntent.provider, userIntent.provider);
      throw new UnsupportedOperationException("Provider cannot be modified.");
    }

    if (existingIntent.providerType != userIntent.providerType) {
      LOG.error("Provider type cannot be changed from {} to {}",
                existingIntent.providerType, userIntent.providerType);
      throw new UnsupportedOperationException("providerType cannot be modified.");
    }

    verifyNodesAndRF(oldCluster.clusterType,
                     userIntent.numNodes, userIntent.replicationFactor);
  }

  // Helper API to verify number of nodes and replication factor requirements.
  public static void verifyNodesAndRF(ClusterType clusterType, int numNodes, int replicationFactor) {
    // We only support a replication factor of 1,3,5,7 for primary cluster.
    // And any value from 1 to 7 for read only cluster.
    if ((clusterType == PRIMARY && !supportedRFs.contains(replicationFactor)) ||
        (clusterType == ASYNC && !supportedReadOnlyRFs.contains(replicationFactor))) {
      String errMsg = String.format("Replication factor %d not allowed, must be one of %s.",
                                    replicationFactor,
                                    Joiner.on(',').join(clusterType == PRIMARY ?
                                                        supportedRFs : supportedReadOnlyRFs));
      LOG.error(errMsg);
      throw new UnsupportedOperationException(errMsg);
    }

    // If not a fresh create, must have at least as many nodes as the replication factor.
    if (numNodes > 0 && numNodes < replicationFactor) {
      String errMsg = String.format("Number of nodes %d cannot be less than the replication " +
          " factor %d.", numNodes, replicationFactor);
      LOG.error(errMsg);
      throw new UnsupportedOperationException(errMsg);
    }
  }

  // Helper API to sort an given map in ascending order of its values and return the same.
  private static LinkedHashMap<UUID, Integer> sortByValues(Map<UUID, Integer> map,
                                                           boolean isAscending) {
    List<Map.Entry<UUID, Integer>> list = new LinkedList<Map.Entry<UUID, Integer>>(map.entrySet());
    if (isAscending) {
      Collections.sort(list, new Comparator<Map.Entry<UUID, Integer>>() {
        public int compare(Map.Entry<UUID, Integer> o1, Map.Entry<UUID, Integer> o2) {
          return (o1.getValue()).compareTo(o2.getValue());
        }
      });
    } else {
      Collections.sort(list, new Comparator<Map.Entry<UUID, Integer>>() {
        public int compare(Map.Entry<UUID, Integer> o1, Map.Entry<UUID, Integer> o2) {
          return (o2.getValue()).compareTo(o1.getValue());
        }
      });
    }
    LinkedHashMap<UUID, Integer> sortedHashMap = new LinkedHashMap<UUID, Integer>();
    for (Map.Entry<UUID, Integer> entry : list) {
      sortedHashMap.put(entry.getKey(), entry.getValue());
    }
    return sortedHashMap;
  }

  public enum Action {
    NONE,  // Just for initial/defaut value.
    ADD,   // A node has to be added at this placement indices combination.
    REMOVE // Remove the node at this placement indices combination.
  }
  // Structure for tracking the calculated placement indexes on cloud/region/az.
  private static class PlacementIndexes {
    public int cloudIdx = 0;
    public int regionIdx = 0;
    public int azIdx = 0;
    public Action action;

    public PlacementIndexes(int aIdx, int rIdx, int cIdx) {
      cloudIdx = cIdx;
      regionIdx= rIdx;
      azIdx = aIdx;
      action = Action.NONE;
    }

    public PlacementIndexes(int aIdx, int rIdx, int cIdx, boolean isAdd) {
      cloudIdx = cIdx;
      regionIdx= rIdx;
      azIdx = aIdx;
      action = isAdd ? Action.ADD : Action.REMOVE;
    }

    public String toString() {
      return "[" + cloudIdx + ":" + regionIdx + ":" + azIdx + ":" + action + "]";
    }
  }

  // Create the ordered (by increasing node count per AZ) list of placement indices in the
  // given placement info.
  private static LinkedHashSet<PlacementIndexes> findPlacementsOfAZUuid(Map<UUID, Integer> azUuids,
                                                                        Cluster cluster) {
    LinkedHashSet<PlacementIndexes> placements = new LinkedHashSet<PlacementIndexes>();
    CloudType cloudType = cluster.userIntent.providerType;
    String instanceType = cluster.userIntent.instanceType;
    for (UUID targetAZUuid : azUuids.keySet()) {
      int cIdx = 0;
      for (PlacementCloud cloud : cluster.placementInfo.cloudList) {
        int rIdx = 0;
        for (PlacementRegion region : cloud.regionList) {
          int aIdx = 0;
          for (PlacementAZ az : region.azList) {
            if (az.uuid.equals(targetAZUuid)) {
              UUID zoneUUID = region.azList.get(aIdx).uuid;
              if (cloudType.equals(CloudType.onprem)){
                List<NodeInstance> nodesInAZ = NodeInstance.listByZone(zoneUUID, instanceType);
                int numNodesConfigured = getNumNodesInAZInPlacement(placements,
                    new PlacementIndexes(aIdx, rIdx, cIdx, true));
                if (numNodesConfigured < nodesInAZ.size()) {
                  placements.add(new PlacementIndexes(aIdx, rIdx, cIdx));
                  continue;
                }
              } else {
                placements.add(new PlacementIndexes(aIdx, rIdx, cIdx));
                continue;
              }
            }
            aIdx++;
          }
          rIdx++;
        }
        cIdx++;
      }
    }
    LOG.debug("Placement indexes {}.", placements);
    return placements;
  }

  // Helper function to compare a given index combo against list of placement indexes of current placement
  // to get number of nodes in current index
  private static int getNumNodesInAZInPlacement(LinkedHashSet<PlacementIndexes> indexes, PlacementIndexes index) {
    return (int) indexes.stream().filter(currentIndex -> (currentIndex.cloudIdx == index.cloudIdx
            && currentIndex.azIdx == index.azIdx && currentIndex.regionIdx == index.regionIdx)).count();
  }

  private static LinkedHashSet<PlacementIndexes> getBasePlacement(int numNodes,
                                                                  Cluster cluster) {
    LinkedHashSet<PlacementIndexes> placements = new LinkedHashSet<PlacementIndexes>();
    CloudType cloudType = cluster.userIntent.providerType;
    String instanceType = cluster.userIntent.instanceType;
    int count = 0;

    // We would only try to find a placement until the max lookup iterations, if unable to
    // find optimal placement we would just return, most times on on-prem flow this lookup
    // sometimes goes into infinite while loop. This is a stop gap fix.
    while (count < numNodes) {
      boolean foundPlacement = false;
      int cIdx = 0;
      for (PlacementCloud cloud : cluster.placementInfo.cloudList) {
        int rIdx = 0;
        for (PlacementRegion region : cloud.regionList) {
          for (int azIdx = 0; azIdx < region.azList.size(); azIdx++) {
            UUID zoneUUID = region.azList.get(azIdx).uuid;
            if (cloudType.equals(CloudType.onprem)) {
              List<NodeInstance> nodesInAZ = NodeInstance.listByZone(zoneUUID, instanceType);
              int numNodesConfigured = getNumNodesInAZInPlacement(placements,
                  new PlacementIndexes(azIdx, rIdx, cIdx, true));
              if (numNodesConfigured < nodesInAZ.size() && count < numNodes) {
                placements.add(new PlacementIndexes(azIdx, rIdx, cIdx, true /* isAdd */));
                LOG.info("Adding {}/{}/{} @ {}.", azIdx, rIdx, cIdx, count);
                foundPlacement = true;
                count++;
              }
            } else {
              placements.add(new PlacementIndexes(azIdx, rIdx, cIdx, true /* isAdd */));
              LOG.info("Adding {}/{}/{} @ {}.", azIdx, rIdx, cIdx, count);
              foundPlacement = true;
              count++;
            }
          }
          rIdx++;
        }
        cIdx++;
      }
      // If we cannot find any matching placement we shouldn't continue further.
      if (!foundPlacement) {
        LOG.warn("Unable to find valid placement");
        break;
      }
    }
    LOG.info("Base placement indexes {} for {} nodes.", placements, numNodes);
    return placements;
  }

  /**
   * Returns a map of the AZ UUID's to number of nodes in each AZ taken from the Universe
   * placement info, the desired setup the user intends to place each AZ.
   * @param placement Structure containing list of cloud providers, regions, and zones
   * @return HashMap of UUID to number of nodes
   */
  public static Map<UUID, Integer> getAzUuidToNumNodes(PlacementInfo placement) {
    Map<UUID, Integer> azUuidToNumNodes = new HashMap<UUID, Integer>();

    for (PlacementCloud cloud : placement.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        for (PlacementAZ az : region.azList) {
          azUuidToNumNodes.put(az.uuid, az.numNodesInAZ);
        }
      }
    }

    LOG.info("Az placement map {}", azUuidToNumNodes);

    return azUuidToNumNodes;
  }

  /**
   * Returns a map of the AZ UUID's to total number of nodes in each AZ, active and inactive.
   * @param nodeDetailsSet Set of NodeDetails for a Universe
   * @return HashMap of UUID to total number of nodes
   */
  public static Map<UUID, Integer> getAzUuidToNumNodes(Collection<NodeDetails> nodeDetailsSet) {
    return getAzUuidToNumNodes(nodeDetailsSet, false /* onlyActive */);
  }

  private static Map<UUID, Integer> getAzUuidToNumNodes(Collection<NodeDetails> nodeDetailsSet,
                                                        boolean onlyActive) {
    // Get node count per azUuid in the current universe.
    Map<UUID, Integer> azUuidToNumNodes = new HashMap<UUID, Integer>();
    for (NodeDetails node : nodeDetailsSet) {
      if ((onlyActive && !node.isActive()) || (node.isMaster && !node.isTserver)) {
        continue;
      }
      UUID azUuid = node.azUuid;
      if (!azUuidToNumNodes.containsKey(azUuid)) {
        azUuidToNumNodes.put(azUuid, 0);
      }
      azUuidToNumNodes.put(azUuid, azUuidToNumNodes.get(azUuid) + 1);
    }

    LOG.info("Az Map {}", azUuidToNumNodes);

    return azUuidToNumNodes;
  }

  // Count number of active tserver-only nodes in the given AZ.
  private static int findCountActiveTServerOnlyInAZ(Collection<NodeDetails> nodeDetailsSet,
                                                    UUID targetAZUuid) {
    int numActiveServers = 0;
    for (NodeDetails node : nodeDetailsSet) {
      if (node.isActive() && !node.isMaster && node.isTserver && node.azUuid.equals(targetAZUuid)) {
        numActiveServers ++;
      }
    }
    return numActiveServers;
  }

  // Find a node running tserver only in the given AZ.
  public static NodeDetails findActiveTServerOnlyInAz(Collection<NodeDetails> nodes,
                                                       UUID targetAZUuid) {
    NodeDetails activeNode = nodes.stream()
                                  .filter(node -> node.isActive() && !node.isMaster &&
                                                  node.isTserver &&
                                                  node.azUuid.equals(targetAZUuid))
                                  .max(Comparator.comparingInt(NodeDetails::getNodeIdx))
                                  .orElse(null);
    return activeNode;

  }

  /**
   * Get new nodes per AZ that need to be added or removed for custom AZ placement scenarios.
   * Assign nodes as per AZ distribution delta between placementInfo and existing nodes and
   * save order of those indices.
   * @param placementInfo has the distribution of Nodes in each AZ.
   * @param nodes Set of currently allocated nodes.
   * @return set of indexes in which to provision the nodes.
   */
  private static LinkedHashSet<PlacementIndexes> getDeltaPlacementIndices(
      PlacementInfo placementInfo, Collection<NodeDetails> nodes) {
    LinkedHashSet<PlacementIndexes> placements = new LinkedHashSet<PlacementIndexes>();
    Map<UUID, Integer> azUuidToNumNodes = getAzUuidToNumNodes(nodes, true);

    for (int cIdx = 0; cIdx < placementInfo.cloudList.size(); cIdx++) {
      PlacementCloud cloud = placementInfo.cloudList.get(cIdx);
      for (int rIdx = 0; rIdx < cloud.regionList.size(); rIdx++) {
        PlacementRegion region = cloud.regionList.get(rIdx);
        for (int azIdx = 0; azIdx < region.azList.size(); azIdx++) {
          PlacementAZ az = region.azList.get(azIdx);
          int numDesired = az.numNodesInAZ;
          int numPresent = azUuidToNumNodes.containsKey(az.uuid) ? azUuidToNumNodes.get(az.uuid) : 0;
          LOG.info("AZ {} : Desired {}, present {}.", az.name, numDesired, numPresent);
          int numChange = Math.abs(numDesired - numPresent);
          while (numChange > 0) {
            placements.add(new PlacementIndexes(azIdx, rIdx, cIdx, numDesired > numPresent));
            numChange--;
          }
        }
      }
    }
    LOG.debug("Delta placement indexes {}.", placements);
    return placements;
  }

  /**
   * Remove a tserver-only node that belongs to the given AZ from the collection of nodes.
   * @param nodes        the list of nodes from which to choose the victim.
   * @param targetAZUuid AZ in which the node should be present.
   */
  private static void removeNodeInAZ(Collection<NodeDetails> nodes,
                                     UUID targetAZUuid) {
    Iterator<NodeDetails> nodeIter = nodes.iterator();
    while (nodeIter.hasNext()) {
      NodeDetails currentNode = nodeIter.next();
      if (currentNode.azUuid.equals(targetAZUuid) && !currentNode.isMaster) {
        nodeIter.remove();
        return;
      }
    }
  }

  /**
   * This method configures nodes for Edit case, with user specified placement info.
   * It supports the following combinations --
   * 1. Reset AZ, it result in a full move as new config is generated
   * 2. Any subsequent operation after a Reset AZ will be a full move since subsequent operations will build on reset
   * 3. Simple Node Count increase will result in an expand.
   * 4. Any Shrink scenario will be a full move. This is to prevent inconsistencies in cases where the node which is subtracted
   * is a master node vs. if it is not a master node which are indistinguishable from an AZ Selector pov.
   * 5. Multi to Single AZ ops will also be a full move operation since it involves shrinks
   * @param taskParams
   */
  public static void configureNodeEditUsingPlacementInfo(UniverseDefinitionTaskParams taskParams) {
    // TODO: this only works for a case when we have on read replica,
    // if we have more than one we need to revisit this logic.
    Cluster currentCluster = taskParams.currentClusterType.equals(PRIMARY) ?
        taskParams.getPrimaryCluster() : taskParams.getReadOnlyClusters().get(0);

    Universe universe = Universe.get(taskParams.universeUUID);
    Collection<NodeDetails> existingNodes = universe.getNodesInCluster(currentCluster.uuid);

    // If placementInfo is null then user has chosen to Reset AZ config
    // Hence a new full move configuration is generated
    if (currentCluster.placementInfo == null) {
      // Remove primary cluster nodes which will be added back in ToBeRemoved state
      taskParams.nodeDetailsSet.removeIf((NodeDetails nd) -> {
        return (nd.placementUuid.equals(currentCluster.uuid));
      });
      currentCluster.placementInfo =
          getPlacementInfo(currentCluster.clusterType, currentCluster.userIntent);
      configureDefaultNodeStates(currentCluster, taskParams.nodeDetailsSet, universe);
    } else {
      // In other operations we need to distinguish between expand and full-move.
      Map<UUID, Integer> requiredAZToNodeMap =
          getAzUuidToNumNodes(currentCluster.placementInfo);
      Map<UUID, Integer> existingAZToNodeMap = getAzUuidToNumNodes(
          universe.getUniverseDetails().getNodesInCluster(currentCluster.uuid)
      );

      boolean isSimpleExpand = true;
      for (UUID requiredAZUUID: requiredAZToNodeMap.keySet()) {
        if (existingAZToNodeMap.containsKey(requiredAZUUID) &&
            requiredAZToNodeMap.get(requiredAZUUID) < existingAZToNodeMap.get(requiredAZUUID)) {
          isSimpleExpand = false;
          break;
        } else {
          existingAZToNodeMap.remove(requiredAZUUID);
        }
      }
      if (existingAZToNodeMap.size() > 0) {
        isSimpleExpand = false;
      }
      if (isSimpleExpand) {
        // If simple expand we can go in the configure using placement info path
        configureNodesUsingPlacementInfo(currentCluster,
                                         taskParams.nodeDetailsSet, true);
        // Break execution sequence because there are no nodes to be decomissioned
        return;
      } else {
        // If not simply create a nodeDetailsSet from the provided placement info.
        taskParams.nodeDetailsSet.clear();
        int startIndex = getNextIndexToConfigure(existingNodes);
        int iter = 0;
        LinkedHashSet<PlacementIndexes> placements = new LinkedHashSet<PlacementIndexes>();
        for (int cIdx = 0; cIdx < currentCluster.placementInfo.cloudList.size(); cIdx++) {
          PlacementCloud cloud = currentCluster.placementInfo.cloudList.get(cIdx);
          for (int rIdx = 0; rIdx < cloud.regionList.size(); rIdx++) {
            PlacementRegion region = cloud.regionList.get(rIdx);
            for (int azIdx = 0; azIdx < region.azList.size(); azIdx++) {
              PlacementAZ az = region.azList.get(azIdx);
              int numDesired = az.numNodesInAZ;

              int numChange = Math.abs(numDesired);
              // Add all new nodes in the tbe added state
              while (numChange > 0) {
                iter ++;
                placements.add(new PlacementIndexes(azIdx, rIdx, cIdx, true));
                NodeDetails nodeDetails =
                    createNodeDetailsWithPlacementIndex(currentCluster,
                        new PlacementIndexes(azIdx, rIdx, cIdx, true),
                        startIndex + iter);
                taskParams.nodeDetailsSet.add(nodeDetails);
                numChange--;
              }
            }
          }
        }
      }
    }
    LOG.info("Removing {} nodes.", existingNodes.size());
    for (NodeDetails node : existingNodes) {
      node.state = NodeDetails.NodeState.ToBeRemoved;
      taskParams.nodeDetailsSet.add(node);
    }
  }

  private static void configureNodesUsingPlacementInfo(Cluster cluster,
                                                       Collection<NodeDetails> nodes,
                                                       boolean isEditUniverse) {
    LinkedHashSet<PlacementIndexes> indexes =
        getDeltaPlacementIndices(
            cluster.placementInfo,
            nodes.stream().filter(n -> n.placementUuid.equals(cluster.uuid)).collect(Collectors.toSet()));
    Set<NodeDetails> deltaNodesSet = new HashSet<NodeDetails>();
    int startIndex = getNextIndexToConfigure(nodes);
    int iter = 0;
    for (PlacementIndexes index : indexes) {
      if (index.action == Action.ADD) {
        NodeDetails nodeDetails =
            createNodeDetailsWithPlacementIndex(cluster, index, startIndex + iter);
        deltaNodesSet.add(nodeDetails);
      } else if (index.action == Action.REMOVE) {
        PlacementCloud placementCloud = cluster.placementInfo.cloudList.get(index.cloudIdx);
        PlacementRegion placementRegion = placementCloud.regionList.get(index.regionIdx);
        PlacementAZ placementAZ = placementRegion.azList.get(index.azIdx);
        if (isEditUniverse) {
          decommissionNodeInAZ(nodes, placementAZ.uuid);
        } else {
          removeNodeInAZ(nodes, placementAZ.uuid);
        }
      }
      iter++;
    }
    nodes.addAll(deltaNodesSet);
  }

  private static int getNumTserverNodes(Collection<NodeDetails> nodeDetailsSet) {
    int count = 0;
    for (NodeDetails node: nodeDetailsSet) {
      if (node.isTserver) {
        count++;
      }
    }
    return count;
  }

  private static void configureNodesUsingUserIntent(Cluster cluster,
                                                    Collection<NodeDetails> nodeDetailsSet,
                                                    boolean isEditUniverse) {
    UserIntent userIntent = cluster.userIntent;
    Set<NodeDetails> nodesInCluster = nodeDetailsSet.stream()
            .filter(n -> n.placementUuid.equals(cluster.uuid))
            .collect(Collectors.toSet());

    int numTservers = getNumTserverNodes(nodesInCluster);
    int numDeltaNodes = userIntent.numNodes - numTservers;
    Map<String, NodeDetails> deltaNodesMap = new HashMap<String, NodeDetails>();
    Map<UUID, Integer> azUuidToNumNodes = getAzUuidToNumNodes(nodesInCluster);
    LOG.info("Nodes desired={} vs existing={}.", userIntent.numNodes,
            numTservers);
    if (numDeltaNodes < 0) {
      // Desired action is to remove nodes from a given cluster.
      Iterator<NodeDetails> nodeIter = nodeDetailsSet.iterator();
      int deleteCounter = 0;
      while (nodeIter.hasNext()) {
        NodeDetails currentNode = nodeIter.next();
        if (currentNode.isMaster || !currentNode.placementUuid.equals(cluster.uuid)) {
          continue;
        }
        if (isEditUniverse) {
          if (currentNode.isActive()) {
            currentNode.state = NodeDetails.NodeState.ToBeRemoved;
            LOG.debug("Removing node [{}].", currentNode);
            deleteCounter++;
          }
        } else {
          nodeIter.remove();
          deleteCounter++;
        }
        if (deleteCounter == -numDeltaNodes) {
          break;
        }
      }
    } else if (numDeltaNodes > 0) {
      // Desired action is to add nodes.
      LinkedHashSet<PlacementIndexes> indexes =
              findPlacementsOfAZUuid(sortByValues(azUuidToNumNodes, true), cluster);
      // If we cannot find enough nodes to do the expand we would return an error.
      if (indexes.size() != azUuidToNumNodes.size()) {
        throw new IllegalStateException("Couldn't find enough nodes to perform expand/shrink");
      }

      int startIndex = getNextIndexToConfigure(nodeDetailsSet);
      addNodeDetailSetToTaskParams(indexes, startIndex, numDeltaNodes, cluster, nodeDetailsSet,
                                   deltaNodesMap);
    }
  }

  private static void configureDefaultNodeStates(Cluster cluster,
                                                 Collection<NodeDetails> nodeDetailsSet,
                                                 Universe universe) {
    UserIntent userIntent = cluster.userIntent;
    int startIndex = universe != null ? getNextIndexToConfigure(universe.getNodes()) :
        getNextIndexToConfigure(nodeDetailsSet);
    int numNodes = userIntent.numNodes;
    Map<String, NodeDetails> deltaNodesMap = new HashMap<>();
    LinkedHashSet<PlacementIndexes> indexes = getBasePlacement(numNodes, cluster);
    addNodeDetailSetToTaskParams(indexes, startIndex, numNodes, cluster, nodeDetailsSet,
                                 deltaNodesMap);

    // Full move.
    if (universe != null) {
      Collection<NodeDetails> existingNodes = universe.getNodesInCluster(cluster.uuid);
      LOG.info("Decommissioning {} nodes.", existingNodes.size());
      for (NodeDetails node : existingNodes) {
        node.state = NodeDetails.NodeState.ToBeRemoved;
        nodeDetailsSet.add(node);
      }
    }
  }

  /**
   * This function helps remove zones in the placement with zero nodes.
   *
   * @param placementInfo The cluster placement info.
   */
  private static void removeUnusedPlacementAZs(PlacementInfo placementInfo) {
    for (PlacementCloud cloud : placementInfo.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        Iterator<PlacementAZ> azIter = region.azList.iterator();
        while (azIter.hasNext()) {
          PlacementAZ az = azIter.next();
          if (az.numNodesInAZ == 0) {
            LOG.info("Removing placement AZ {}", az.name);
            azIter.remove();
          }
        }
      }
    }
  }

  /**
   * Check to confirm the following after each configure call:
   *   - node AZs and placement AZs match.
   *   - instance type of all nodes matches.
   *   - each nodes has a unique name.
   *
   * @param cluster The cluster whose placement is checked.
   * @param nodes   The nodes in this cluster.
   */
  private static void finalSanityCheckConfigure(Cluster cluster,
                                                Collection<NodeDetails> nodes,
                                                boolean resetConfig) {
    PlacementInfo placementInfo = cluster.placementInfo;
    CloudType cloudType = cluster.userIntent.providerType;
    String instanceType = cluster.userIntent.instanceType;
    // For on-prem we could choose nodes from other AZs if not enough provisioned.
    if (cloudType != CloudType.onprem) {
      Map<UUID, Integer> placementAZToNodeMap = getAzUuidToNumNodes(placementInfo);
      Map<UUID, Integer> nodesAZToNodeMap = getAzUuidToNumNodes(nodes, true);
      if (!nodesAZToNodeMap.equals(placementAZToNodeMap) && !resetConfig) {
        String msg = "Nodes are in different AZs compared to placement";
        LOG.error("{}. PlacementAZ={}, nodesAZ={}", msg, placementAZToNodeMap, nodesAZToNodeMap);
        throw new IllegalStateException(msg);
      }
    }

    for (NodeDetails node : nodes) {
      String nodeType = node.cloudInfo.instance_type;
      if (node.state != NodeDetails.NodeState.ToBeRemoved &&
          !instanceType.equals(nodeType)) {
        String msg = "Instance type " + instanceType + " mismatch for " +
                      node.nodeName + ", expected type " + nodeType + ".";
        LOG.error(msg);
        throw new IllegalStateException(msg);
      }
    }
  }

  /**
   * Configures the state of the nodes that need to be created or the ones to be removed.
   *
   * @param taskParams the taskParams for the Universe to be configured.
   * @param universe   the current universe if it exists (only when called during edit universe).
   * @param mode       mode in which to configure with user specified AZ's or round-robin (default).
   *
   * @return set of node details with their placement info filled in.
   */
  private static void configureNodeStates(UniverseDefinitionTaskParams taskParams,
                                          Universe universe,
                                          PlacementInfoUtil.ConfigureNodesMode mode,
                                          Cluster cluster) {
    switch (mode) {
      case NEW_CONFIG:
        // This case covers create universe and full move edit.
        configureDefaultNodeStates(cluster, taskParams.nodeDetailsSet, universe);
        updatePlacementInfo(taskParams.getNodesInCluster(cluster.uuid), cluster.placementInfo);
        break;
      case UPDATE_CONFIG_FROM_PLACEMENT_INFO:
        // The case where there are custom expand/shrink in the placement info.
        configureNodesUsingPlacementInfo(cluster, taskParams.nodeDetailsSet, universe != null);
        break;
      case UPDATE_CONFIG_FROM_USER_INTENT:
        // Case where userIntent numNodes has to be favored - as it is different from the
        // sum of all per AZ node counts).
        configureNodesUsingUserIntent(cluster, taskParams.nodeDetailsSet, universe != null);
        updatePlacementInfo(taskParams.getNodesInCluster(cluster.uuid), cluster.placementInfo);
        break;
      case NEW_CONFIG_FROM_PLACEMENT_INFO:
        configureNodeEditUsingPlacementInfo(taskParams);
    }

    removeUnusedPlacementAZs(cluster.placementInfo);

    LOG.info("Set of nodes after node configure: {}.", taskParams.nodeDetailsSet);
    LOG.info("Placement info: {}.", cluster.placementInfo);
    finalSanityCheckConfigure(cluster,
            taskParams.getNodesInCluster(cluster.uuid),
            taskParams.resetAZConfig || taskParams.userAZSelected);
  }

  /**
   * Find a node which has tservers only to decommission, from the given AZ.
   * Node should be an active T-Server and should not be Master.
   * @param nodes   the list of nodes from which to choose the victim.
   * @param targetAZUuid AZ in which the node should be present.
   */
  private static void decommissionNodeInAZ(Collection<NodeDetails> nodes,
                                           UUID targetAZUuid) {
    NodeDetails nodeDetails = findActiveTServerOnlyInAz(nodes, targetAZUuid);
    if (nodeDetails == null) {
      LOG.error("Could not find an active node running tservers only in AZ {}. All nodes: {}.",
                targetAZUuid, nodes);
      throw new IllegalStateException("Should find an active running tserver.");
    } else {
      nodeDetails.state = NodeDetails.NodeState.ToBeRemoved;
      LOG.debug("Removing node [{}].", nodeDetails);
    }
  }

  /**
   * Method takes a placementIndex and returns a NodeDetail object for it in order to add a node.
   *
   * @param cluster     The current cluster.
   * @param index       The placement index combination.
   * @param nodeIdx     Node index to be used in node name.
   * @return a NodeDetails object.
   */
  private static NodeDetails createNodeDetailsWithPlacementIndex(Cluster cluster,
                                                                 PlacementIndexes index,
                                                                 int nodeIdx) {
    NodeDetails nodeDetails = new NodeDetails();
    // Note: node name is set during customer task run time.
    // Set the cluster.
    nodeDetails.placementUuid = cluster.uuid;
    // Set the cloud.
    PlacementCloud placementCloud = cluster.placementInfo.cloudList.get(index.cloudIdx);
    nodeDetails.cloudInfo = new CloudSpecificInfo();
    nodeDetails.cloudInfo.cloud = placementCloud.code;
    // Set the region.
    PlacementRegion placementRegion = placementCloud.regionList.get(index.regionIdx);
    nodeDetails.cloudInfo.region = placementRegion.code;
    // Set the AZ and the subnet.
    PlacementAZ placementAZ = placementRegion.azList.get(index.azIdx);
    nodeDetails.azUuid = placementAZ.uuid;
    nodeDetails.cloudInfo.az = placementAZ.name;
    nodeDetails.cloudInfo.subnet_id = placementAZ.subnet;
    nodeDetails.cloudInfo.instance_type = cluster.userIntent.instanceType;
    nodeDetails.cloudInfo.assignPublicIP = cluster.userIntent.assignPublicIP;
    nodeDetails.cloudInfo.useTimeSync = cluster.userIntent.useTimeSync;
    // Set the tablet server role to true.
    nodeDetails.isTserver = true;
    // Set the node id.
    nodeDetails.nodeIdx = nodeIdx;
    // We are ready to add this node.
    nodeDetails.state = NodeDetails.NodeState.ToBeAdded;
    LOG.debug("Placed new node [{}] at cloud:{}, region:{}, az:{}. uuid {}.",
            nodeDetails, index.cloudIdx, index.regionIdx, index.azIdx, nodeDetails.azUuid);
    return nodeDetails;
  }

  /**
   * Construct a delta node set and add all those nodes to the Universe's set of nodes.
   */
  private static void addNodeDetailSetToTaskParams(LinkedHashSet<PlacementIndexes> indexes,
                                                   int startIndex,
                                                   int numDeltaNodes,
                                                   Cluster cluster,
                                                   Collection<NodeDetails> nodeDetailsSet,
                                                   Map<String, NodeDetails> deltaNodesMap) {
    Set<NodeDetails> deltaNodesSet = new HashSet<NodeDetails>();
    // Create the names and known properties of all the nodes to be created.
    Iterator<PlacementIndexes> iter = indexes.iterator();
    for (int nodeIdx = startIndex; nodeIdx < startIndex + numDeltaNodes; nodeIdx++) {
      PlacementIndexes index = null;
      if (iter.hasNext()) {
        index = iter.next();
      } else {
        iter = indexes.iterator();
        index = iter.next();
      }
      NodeDetails nodeDetails = createNodeDetailsWithPlacementIndex(cluster, index, nodeIdx);
      deltaNodesSet.add(nodeDetails);
      deltaNodesMap.put(nodeDetails.nodeName, nodeDetails);
    }
    nodeDetailsSet.addAll(deltaNodesSet);
  }

  /**
  * Select masters according to region and zone. We create an ordered list of nodes:
  * The nodes are selected per zone, with each zone itself being selected per region.
  */
  public static void selectMasters(Collection<NodeDetails> nodes, int numMastersToChoose) {
    // Map to region to zone to nodes. Using a multi-level map ensures that even if zones names
    // are similar across regions, there will be no conflict.
    List<NodeDetails> validNodes = nodes.stream()
                                        .filter(node -> node.state == NodeState.Live ||
                                                        node.state == NodeState.ToBeAdded)
                                        .collect(Collectors.toList());

    Map<String, Map<String, LinkedList<NodeDetails>>> regionToZoneToNode =
        validNodes.stream().collect(groupingBy(node -> node.getRegion(),
                                    groupingBy(node -> node.getZone(),
                                               toCollection(LinkedList::new))));

    // Map to region to zone. This is used to populate the ordering of the region/zone pairs from
    // which to select nodes.
    Map<String, LinkedList<String>> regionToZone =
        validNodes.stream().collect(groupingBy(node -> node.getRegion(),
                                    Collectors.mapping(node -> node.getZone(),
                                        Collectors.collectingAndThen(toCollection(HashSet::new),
                                            values -> new LinkedList(values)))));

    int numCandidates =  validNodes.size();

    if (numMastersToChoose > numCandidates) {
      throw new IllegalStateException("Could not pick " + numMastersToChoose + " masters, only " +
              numCandidates + " nodes available. Nodes info. " + nodes);
    }

    // Create a queue to order the region/zone pairs, so that we place one node in each region,
    // and then when all regions have a node placed, the next time they'll use a different region/
    // zone pairing.
    Queue<Pair<String, String>> queue = new LinkedList<Pair<String, String>>();
    while (!regionToZone.isEmpty()) {
      Iterator <Entry<String, LinkedList<String>>> it = regionToZone.entrySet().iterator();
      while (it.hasNext()) {
        Entry<String, LinkedList<String>> entry = it.next();
        String region = entry.getKey();
        String zone = entry.getValue().pop();
        queue.add(new Pair(region, zone));
        if (entry.getValue().isEmpty()) {
          it.remove();
        }
      }
    }

    // Now that we have the ordering of the region/zones, we place masters.
    int mastersSelected = 0;
    while (mastersSelected < numMastersToChoose) {
      Pair<String, String> regionZone = queue.remove();
      Map<String, LinkedList<NodeDetails>> zoneToNode =
          regionToZoneToNode.get(regionZone.first);
      LinkedList<NodeDetails> nodeList = zoneToNode.get(regionZone.second);
      NodeDetails node = nodeList.pop();
      node.isMaster = true;
      mastersSelected++;
      // If the region/zone pair doesn't have a node remaining, we don't need
      // to consider it again.
      if (!nodeList.isEmpty()) {
        queue.add(regionZone);
      }
    }
  }

  // Select the number of AZs for each deployment.
  public static void selectNumMastersAZ(PlacementInfo pi, int numTotalMasters) {
    int totalAZs = 0;
    for (PlacementCloud pc : pi.cloudList) {
      for (PlacementRegion pr : pc.regionList) {
        totalAZs += pr.azList.size();
      }
    }
    for (PlacementCloud pc : pi.cloudList) {

      int remainingMasters = numTotalMasters;
      int azsRemaining = totalAZs;

      for (PlacementRegion pr : pc.regionList) {

        int numAzsInRegion = pr.azList.size();
        // Distribute masters in each region according to the number of AZs in each region.
        int mastersInRegion = (int) Math.round(remainingMasters * ((double) numAzsInRegion / azsRemaining));
        int mastersAdded = 0;
        int saturated = 0;
        int count = 0;
        while (mastersInRegion != mastersAdded && saturated != numAzsInRegion ) {
          saturated = 0;
          for (PlacementAZ pa : pr.azList) {
            // The number of masters in an AZ cannot exceed the number of tservers.
            if ((count + 1) <= pa.numNodesInAZ) {
              pa.replicationFactor = count + 1;
              mastersAdded++;
            } else {
              saturated++;
            }
            if (mastersInRegion == 0) {
              break;
            }
          }
          count++;
        }
        remainingMasters -= mastersAdded;
        azsRemaining -= numAzsInRegion;
      }
    }
  }

  // Check if there are multiple zones for deployment.
  public static boolean isMultiAZ(PlacementInfo pi) {
    if (pi.cloudList.size() > 1) {
      return true;
    }
    for (PlacementCloud pc : pi.cloudList) {
      if (pc.regionList.size() > 1) {
        return true;
      }
      for (PlacementRegion pr : pc.regionList) {
        if (pr.azList.size() > 1) {
          return true;
        }
      }
    }
    return false;
  }

  // Check if there are multiple zones for deployment in the provider config.
  public static boolean isMultiAZ(Provider provider) {
    List<Region> regionList = Region.getByProvider(provider.uuid);
    if (regionList.size() > 1) {
      return true;
    }
    Region region = regionList.get(0);
    if (region == null) {
      throw new RuntimeException("No Regions found");
    }
    List<AvailabilityZone> azList = AvailabilityZone.getAZsForRegion(region.uuid);
    if (azList.size() > 1) {
      return true;
    }
    return false;
  }

  // Get the zones with the number of masters for each zone.
  public static Map<UUID, Integer> getNumMasterPerAZ(PlacementInfo pi) {
    Map<UUID, Integer> azToNumMasters = new HashMap<UUID, Integer>();
    for (PlacementCloud pc : pi.cloudList) {
      for (PlacementRegion pr : pc.regionList) {
        for (PlacementAZ pa : pr.azList) {
          azToNumMasters.put(pa.uuid, pa.replicationFactor);
        }
      }
    }
    return azToNumMasters;
  }

  // Get the zones with the number of tservers for each zone.
  public static Map<UUID, Integer> getNumTServerPerAZ(PlacementInfo pi) {
    Map<UUID, Integer> azToNumTservers = new HashMap<UUID, Integer>();
    for (PlacementCloud pc : pi.cloudList) {
      for (PlacementRegion pr : pc.regionList) {
        for (PlacementAZ pa : pr.azList) {
          azToNumTservers.put(pa.uuid, pa.numNodesInAZ);
        }
      }
    }
    return azToNumTservers;
  }

  // Get the zones with the kubeconfig for that zone.
  public static Map<UUID, Map<String, String>> getConfigPerAZ(PlacementInfo pi) {
    Map<UUID, Map<String, String>> azToConfig = new HashMap<UUID, Map<String, String>>();
    Map<String, String> config = new HashMap<String, String>();
    for (PlacementCloud pc : pi.cloudList) {
      Map<String, String> cloudConfig = Provider.get(pc.uuid).getConfig();
      for (PlacementRegion pr : pc.regionList) {
        Map<String, String> regionConfig = Region.get(pr.uuid).getConfig();
        for (PlacementAZ pa : pr.azList) {
          Map<String, String> zoneConfig = AvailabilityZone.get(pa.uuid).getConfig();
          if (cloudConfig.containsKey("KUBECONFIG")) {
            azToConfig.put(pa.uuid, cloudConfig);
          } else if (regionConfig.containsKey("KUBECONFIG")) {
            azToConfig.put(pa.uuid, regionConfig);
          } else if (zoneConfig.containsKey("KUBECONFIG")) {
            azToConfig.put(pa.uuid, zoneConfig);
          } else {
            throw new RuntimeException("No config found at any level");
          }
        }
      }
    }
    return azToConfig;
  }

  public static String getKubernetesNamespace(String nodePrefix, String azName) {
    return String.format("%s-%s", nodePrefix, azName);
  }

  public static Map<String, String> getConfigPerNamespace(PlacementInfo pi, String nodePrefix,
                                                          Provider provider) {
    Map<String, String> namespaceToConfig = new HashMap<String, String>();
    Map<UUID, Map<String, String>> azToConfig = getConfigPerAZ(pi);
    for (Entry<UUID, Map<String, String>> entry : azToConfig.entrySet()) {
      String kubeconfig = entry.getValue().get("KUBECONFIG");
      if (kubeconfig == null) {
        throw new NullPointerException("Couldn't find a kubeconfig");
      }
      if (!isMultiAZ(provider)) {
        namespaceToConfig.put(nodePrefix, kubeconfig);
        break;
      } else {
        String azName = AvailabilityZone.get(entry.getKey()).code;
        String namespace = getKubernetesNamespace(nodePrefix, azName);
        namespaceToConfig.put(namespace, kubeconfig);
      }
    }
    return namespaceToConfig;
  }


  // Compute the master addresses of the pods in the deployment if multiAZ.
  public static String computeMasterAddresses(PlacementInfo pi, Map<UUID, Integer> azToNumMasters,
                                              String nodePrefix, Provider provider,
                                              int masterRpcPort) {
    List<String> masters = new ArrayList<String>();
    Map<UUID, String> azToDomain = getDomainPerAZ(pi);
    if (!isMultiAZ(provider)) {
      return null;
    }
    for (Entry<UUID, Integer> entry : azToNumMasters.entrySet()) {
      AvailabilityZone az = AvailabilityZone.get(entry.getKey());
      String domain = azToDomain.get(entry.getKey());
      for (int idx = 0; idx < entry.getValue(); idx++) {
        String master = String.format("yb-master-%d.yb-masters.%s-%s.%s:%d", idx, nodePrefix, az.code,
            domain, masterRpcPort);
        masters.add(master);
      }
    }
    return String.join(",", masters);
  }

  public static Map<UUID, String> getDomainPerAZ(PlacementInfo pi) {
    Map<UUID, String> azToDomain = new HashMap<>();
    for (PlacementCloud pc : pi.cloudList) {
      for (PlacementRegion pr : pc.regionList) {
        for (PlacementAZ pa : pr.azList) {
          Map<String, String> config = AvailabilityZone.get(pa.uuid).getConfig();
          if (config.containsKey("KUBE_DOMAIN")) {
            azToDomain.put(pa.uuid, String.format("%s.%s", "svc", config.get("KUBE_DOMAIN")));
          } else {
            azToDomain.put(pa.uuid, "svc.cluster.local");
          }
        }
      }
    }
    return azToDomain;
  }

  // Returns the start index for provisioning new nodes based on the current maximum node index
  // across existing nodes. If called for a new universe being created, then it will return a
  // start index of 1.
  public static int getStartIndex(Collection<NodeDetails> nodes) {
    int maxNodeIdx = 0;
    for (NodeDetails node : nodes) {
      if (node.state != NodeDetails.NodeState.ToBeAdded && node.nodeIdx > maxNodeIdx) {
        maxNodeIdx = node.nodeIdx;
      }
    }
    return maxNodeIdx + 1;
  }

  // Returns the start index for provisioning new nodes based on the current maximum node index.
  public static int getNextIndexToConfigure(Collection<NodeDetails> existingNodes) {
    int maxNodeIdx = 0;
    if (existingNodes != null) {
      for (NodeDetails node : existingNodes) {
        if (node.nodeIdx > maxNodeIdx) {
          maxNodeIdx = node.nodeIdx;
        }
      }
    }
    return maxNodeIdx + 1;
  }

  public static boolean isRegionListMultiAZ(UserIntent userIntent) {
    List<UUID> regionList = userIntent.regionList;
    if (regionList.size() > 1) {
      return true;
    }
    if (regionList.get(0) == null || regionList.get(0).toString().isEmpty()) {
        throw new RuntimeException("Invalid region uuid " + regionList.get(0));
    }
    Region region = Region.get(regionList.get(0));
    if (region == null) {
      throw new RuntimeException("Invalid region list, region not found " + regionList);
    }
    if (region.zones != null &&
        region.zones.size() > 1) {
      return true;
    }
    return false;
  }

  // Returns the AZ placement info for the node in the user intent. It chooses a maximum of
  // `MAX_ZONES` zones for the placement.
  public static PlacementInfo getPlacementInfo(ClusterType clusterType, UserIntent userIntent) {
    if (userIntent == null || userIntent.regionList == null || userIntent.regionList.isEmpty()) {
      LOG.info("No placement due to userIntent={} or regions={}.", userIntent, userIntent.regionList);
      return null;
    }
    verifyNodesAndRF(clusterType, userIntent.numNodes, userIntent.replicationFactor);
    // TODO: Make MAX_ZONES match the RF. This will cover other rf's which are not MAX_ZONES.
    int num_zones = MAX_ZONES;
    if (userIntent.replicationFactor < MAX_ZONES) {
      num_zones = userIntent.replicationFactor;
    }

    // Make sure the preferred region is in the list of user specified regions.
    if (userIntent.preferredRegion != null &&
        !userIntent.regionList.contains(userIntent.preferredRegion)) {
      throw new RuntimeException("Preferred region " + userIntent.preferredRegion +
          " not in user region list.");
    }
    // Create the placement info object.
    PlacementInfo placementInfo = new PlacementInfo();
    boolean useSingleAZ = !isRegionListMultiAZ(userIntent);
    // Handle the single AZ deployment case or RF=1 case.
    // We use single AZ for RF=1 to workaround pending YB issue ENG-3652.
    if (useSingleAZ || (userIntent.replicationFactor == 1)) {
      // Select an AZ in the required region.
      List<AvailabilityZone> azList =
          AvailabilityZone.getAZsForRegion(userIntent.regionList.get(0));
      if (azList.isEmpty()) {
        throw new RuntimeException("No AZ found for region: " + userIntent.regionList.get(0));
      }
      Collections.shuffle(azList);
      UUID azUUID = azList.get(0).uuid;
      LOG.info("Using AZ {} out of {}", azUUID, azList.size());
      // Add all replicas into the same AZ.
      for (int idx = 0; idx < userIntent.replicationFactor; idx++) {
        addPlacementZone(azUUID, placementInfo);
      }
      placementInfo.cloudList.get(0).regionList.get(0).azList.get(0).numNodesInAZ =
          userIntent.numNodes;
      return placementInfo;
    } else {
      // We would group the zones by region and the corresponding nodes, and use
      // this map for subsequent calls, instead of recomputing the list every time.
      Map<UUID, List<AvailabilityZone>> azByRegionMap = new HashMap<>();

      for (int idx = 0; idx < userIntent.regionList.size(); idx++) {
        List<AvailabilityZone> zones =
            AvailabilityZone.getAZsForRegion(userIntent.regionList.get(idx));

        // Filter out zones which doesn't have enough nodes.
        if (userIntent.providerType.equals(CloudType.onprem)) {
          zones = zones.stream().filter((az) ->
              NodeInstance.listByZone(az.uuid, userIntent.instanceType).size() > 0
          ).collect(Collectors.toList());
        }
        if (!zones.isEmpty()) {
          azByRegionMap.put(userIntent.regionList.get(idx), zones);
        }
      }
      List<AvailabilityZone> totalAzsInRegions = azByRegionMap.keySet()
          .stream().flatMap(a -> azByRegionMap.get(a).stream())
          .collect(Collectors.toList());

      if (totalAzsInRegions.isEmpty()) {
        throw new RuntimeException("No AZ found across regions: " + userIntent.regionList);
      }
      LOG.info("numRegions={}, numAzsInRegion={}, zones={}", userIntent.regionList.size(),
               totalAzsInRegions.size(), num_zones);
      if (totalAzsInRegions.size() <= (num_zones - 1)) {
        for (int idx = 0; idx < userIntent.numNodes; idx++) {
          addPlacementZone(totalAzsInRegions.get(idx % totalAzsInRegions.size()).uuid,
                           placementInfo);
        }
      } else {
        // If one region is specified, pick all three AZs from it.
        if (userIntent.regionList.size() == 1) {
          selectAndAddPlacementZones(azByRegionMap.get(userIntent.regionList.get(0)),
                                     placementInfo, num_zones);
        } else {
          // If preferred region was specified we use that region, if not we pick the first region
          // and use that as the preferred region.
          UUID preferredRegionUUID = userIntent.preferredRegion != null ?
              userIntent.preferredRegion : azByRegionMap.entrySet().iterator().next().getKey();
          int numPreferredRegionZones = num_zones - userIntent.regionList.size();

          List<AvailabilityZone> preferredZones = azByRegionMap.get(preferredRegionUUID);
          numPreferredRegionZones = Integer.max(1,
              Integer.min(numPreferredRegionZones, preferredZones.size()) - 1);

          int numNonPreferredRegionZones = num_zones - numPreferredRegionZones;

          List<AvailabilityZone> nonPreferredZones = azByRegionMap.keySet()
              .stream()
              .filter(regionUUID -> !regionUUID.equals(preferredRegionUUID))
              .flatMap(regionUUID -> azByRegionMap.get(regionUUID).stream())
              .collect(Collectors.toList());

          // If we don't have enough nodes on the other Zones/Regions, we would fall back
          // using the preferred region itself.
          if (nonPreferredZones.size() < numNonPreferredRegionZones) {
            numPreferredRegionZones = num_zones - nonPreferredZones.size();
            numNonPreferredRegionZones = nonPreferredZones.size();
          }

          selectAndAddPlacementZones(
              preferredZones, placementInfo, numPreferredRegionZones);
          selectAndAddPlacementZones(
              nonPreferredZones,  placementInfo, numNonPreferredRegionZones);
        }
      }
    }
    return placementInfo;
  }

  private static void selectAndAddPlacementZones(List<AvailabilityZone> azList,
                                                 PlacementInfo placementInfo,
                                                 int numZones) {
    if (azList.size() < numZones) {
      throw new RuntimeException("Need at least " + numZones + " zones but found only " +
          azList.size());
    }
    Collections.shuffle(azList);
    // Pick as many AZs as required.
    for (int idx = 0; idx < numZones; idx++) {
      addPlacementZone(azList.get(idx).uuid, placementInfo);
    }
  }

  public static int getNumMasters(Set<NodeDetails> nodes) {
    int count = 0;
    for (NodeDetails node : nodes) {
      if (node.isMaster) {
        count++;
      }
    }
    return count;
  }

  // Return the count of master nodes which are considered active (for ex., not ToBeRemoved).
  public static int getNumActiveMasters(Set<NodeDetails> nodes) {
    int count = 0;
    for (NodeDetails node : nodes) {
      if (node.isMaster && node.isActive()) {
        count++;
      }
    }
    return count;
  }

  // Helper for creating PlacementInfo for multi-az kubernetes.
  public static void addPlacementZoneHelper(UUID zone, PlacementInfo placementInfo) {
    addPlacementZone(zone, placementInfo);
  }

  private static void addPlacementZone(UUID zone, PlacementInfo placementInfo) {
    // Get the zone, region and cloud.
    AvailabilityZone az = AvailabilityZone.get(zone);
    Region region = az.region;
    Provider cloud = region.provider;
    LOG.debug("provider: {}", cloud.uuid);
    // Find the placement cloud if it already exists, or create a new one if one does not exist.
    PlacementCloud placementCloud = null;
    for (PlacementCloud pCloud : placementInfo.cloudList) {
      if (pCloud.uuid.equals(cloud.uuid)) {
        placementCloud = pCloud;
        break;
      }
    }
    if (placementCloud == null) {
      placementCloud = new PlacementCloud();
      placementCloud.uuid = cloud.uuid;
      placementCloud.code = cloud.code;
      placementInfo.cloudList.add(placementCloud);
    }

    // Find the placement region if it already exists, or create a new one.
    PlacementRegion placementRegion = null;
    for (PlacementRegion pRegion : placementCloud.regionList) {
      if (pRegion.uuid.equals(region.uuid)) {
        placementRegion = pRegion;
        break;
      }
    }
    if (placementRegion == null) {
      placementRegion = new PlacementRegion();
      placementRegion.uuid = region.uuid;
      placementRegion.code = region.code;
      placementRegion.name = region.name;
      placementCloud.regionList.add(placementRegion);
    }

    // Find the placement AZ in the region if it already exists, or create a new one.
    PlacementAZ placementAZ = null;
    for (PlacementAZ pAz : placementRegion.azList) {
      if (pAz.uuid.equals(az.uuid)) {
        placementAZ = pAz;
        break;
      }
    }
    if (placementAZ == null) {
      placementAZ = new PlacementAZ();
      placementAZ.uuid = az.uuid;
      placementAZ.name = az.name;
      placementAZ.replicationFactor = 0;
      placementAZ.subnet = az.subnet;
      placementAZ.isAffinitized = true;
      placementRegion.azList.add(placementAZ);
    }
    placementAZ.replicationFactor++;
    placementAZ.numNodesInAZ++;
  }

  // Removes a given node from universe nodeDetailsSet
  public static void removeNodeByName(String nodeName, Set<NodeDetails> nodeDetailsSet) {
    Iterator<NodeDetails> nodeIter = nodeDetailsSet.iterator();
    while (nodeIter.hasNext()) {
      NodeDetails currentNode = nodeIter.next();
      if (currentNode.nodeName.equals(nodeName)) {
        nodeIter.remove();
        return;
      }
    }
  }

  // Checks the status of the node by given name in current universe
  public static boolean isNodeRemovable(String nodeName, Set<NodeDetails> nodeDetailsSet) {
    Iterator<NodeDetails> nodeIter = nodeDetailsSet.iterator();
    while (nodeIter.hasNext()) {
      NodeDetails currentNode = nodeIter.next();
      if (currentNode.nodeName.equals(nodeName) && currentNode.isRemovable()) {
        return true;
      }
    }
    return false;
  }

  /**
   * Given a node, return its tserver & master alive/not-alive status and if either server is
   * running we claim that the node is live else we claim it unreachable.
   *
   * @param nodeDetails The node to get the status of.
   * @param nodeJson Metadata about all the nodes in the node's universe.
   * @return JsonNode with the following format:
   *  {
   *    tserver_alive: true/false,
   *    master_alive: true/false,
   *    node_status: <NodeDetails.NodeState>
   *  }
   */
  private static ObjectNode getNodeAliveStatus(NodeDetails nodeDetails, JsonNode nodeJson) {
    boolean tserverAlive = false;
    boolean masterAlive = false;
    List<String> nodeValues = new ArrayList<String>();

    if (nodeJson.get("data") != null) {
      for (JsonNode json : nodeJson.get("data")) {
        String[] name = json.get("name").asText().split(":", 2);
        if (name.length == 2 && name[0].equals(nodeDetails.cloudInfo.private_ip)) {
          boolean alive = false;
          for (JsonNode upData : json.get("y")) {
            try {
              alive = alive || (1 == (int)Float.parseFloat(upData.asText()));
            } catch (NumberFormatException nfe) {
              LOG.debug("Invalid number in node alive data: " + upData.asText());
              // ignore this value
            }
          }

          int port = Integer.parseInt(name[1]);

          if (port == nodeDetails.masterHttpPort) {
            masterAlive = masterAlive || alive;
          } else if (port == nodeDetails.tserverHttpPort) {
            tserverAlive = tserverAlive || alive;
          }

          if (!alive) {
            nodeValues.add(json.toString());
          }
        }
      }
    }

    if (!masterAlive || !tserverAlive) {
      LOG.debug("Master or tserver considered not alive based on data: %s", nodeValues);
    }
    nodeDetails.state = (!masterAlive && !tserverAlive) ?
        NodeDetails.NodeState.Unreachable : nodeDetails.state;

    return Json.newObject()
            .put("tserver_alive", tserverAlive)
            .put("master_alive", masterAlive)
            .put("node_status", nodeDetails.state.toString());
  }

  /**
   * Helper function to get the status for each node and the alive/not alive status for each master
   * and tserver.
   *
   * @param universe The universe to process alive status for.
   * @param metricQueryResult The result of the query for node, master, and tserver status of the
   *                          universe.
   * @return The response object containing the status of each node in the universe.
   */
  private static ObjectNode constructUniverseAliveStatus(Universe universe,
                                                         JsonNode metricQueryResult) {
    ObjectNode response = Json.newObject();

    // If error detected, update state and exit. Otherwise, get and return per-node liveness.
    if (metricQueryResult.has("error")) {
      for (NodeDetails nodeDetails : universe.getNodes()) {
        nodeDetails.state = NodeDetails.NodeState.Unreachable;
        ObjectNode result = Json.newObject()
                .put("tserver_alive", false)
                .put("master_alive", false)
                .put("node_status", nodeDetails.state.toString());
        response.set(nodeDetails.nodeName, result);
      }
    } else {
      JsonNode nodeJson = metricQueryResult.get(UNIVERSE_ALIVE_METRIC);
      for (NodeDetails nodeDetails : universe.getNodes()) {
        ObjectNode result = getNodeAliveStatus(nodeDetails, nodeJson);
        response.set(nodeDetails.nodeName, result);
      }
    }
    return response;
  }

  /**
   * Given a universe, return a status for each master and tserver as alive/not alive and the
   * node's status.
   *
   * @param universe The universe to query alive status for.
   * @param metricQueryHelper Helper to execute the metrics query.
   * @return JsonNode with the following format:
   *  {
   *    universe_uuid: <universeUUID>,
   *    <node_name_n>: {tserver_alive: true/false, master_alive: true/false,
   *                    node_status: <NodeDetails.NodeState>}
   *  }
   */
  public static JsonNode getUniverseAliveStatus(Universe universe,
                                                MetricQueryHelper metricQueryHelper) {
    List<String> metricKeys = ImmutableList.of(UNIVERSE_ALIVE_METRIC);

    // Set up params for metrics query.
    Map<String, String> params = new HashMap<>();
    DateTime now = DateTime.now();
    params.put("end", Long.toString(now.getMillis()/1000, 10));
    DateTime start = now.minusMinutes(1);
    params.put("start", Long.toString(start.getMillis()/1000, 10));
    ObjectNode filterJson = Json.newObject();
    filterJson.put("node_prefix", universe.getUniverseDetails().nodePrefix);
    params.put("filters", Json.stringify(filterJson));
    for (int i = 0; i < metricKeys.size(); ++i) {
      params.put("metrics[" + Integer.toString(i) + "]", metricKeys.get(i));
    }
    params.put("step", "30");

    // Execute query and check for errors.
    JsonNode metricQueryResult = metricQueryHelper.query(metricKeys, params);

    // Persist the desired node information into the DB.
    ObjectNode response = constructUniverseAliveStatus(universe, metricQueryResult);
    response.put("universe_uuid", universe.universeUUID.toString());

    return metricQueryResult.has("error") ? metricQueryResult : response;
  }

  public static int getZoneRF(PlacementInfo pi, String cloud,
                              String region, String zone) {
    for (PlacementCloud pc : pi.cloudList) {
      if (pc.code.equals(cloud)) {
        for (PlacementRegion pr : pc.regionList) {
          if (pr.code.equals(region)) {
            for (PlacementAZ pa : pr.azList) {
              if (pa.name.equals(zone)) {
                return pa.replicationFactor;
              }
            }
          }
        }
      }
    }
    return -1;
  }

  public static int getNumActiveTserversInZone(Collection<NodeDetails> nodes, String cloud,
                                               String region, String zone) {
    List<NodeDetails> validNodes = nodes.stream()
                                        .filter(node -> node.isActive() &&
                                                        node.cloudInfo.cloud.equals(cloud) &&
                                                        node.cloudInfo.region.equals(region) &&
                                                        node.cloudInfo.az.equals(zone))
                                        .collect(Collectors.toList());
    return validNodes.size();
  }

}
