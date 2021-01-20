// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import com.google.common.base.Functions;
import com.google.common.base.Joiner;
import com.google.common.collect.Lists;
import com.google.common.net.HostAndPort;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;

import java.net.InetSocketAddress;
import java.net.MalformedURLException;
import java.net.URL;
import java.util.*;
import java.text.SimpleDateFormat;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import static com.yugabyte.yw.common.PlacementInfoUtil.getNumMasters;

public class Util {
  public static final Logger LOG = LoggerFactory.getLogger(Util.class);

  /**
   * Convert a list of {@link HostAndPort} objects to a comma separate string.
   *
   * @param hostsAndPorts A list of {@link HostAndPort} objects.
   * @return Comma separate list of "host:port" pairs.
   */
  public static String hostsAndPortsToString(List<HostAndPort> hostsAndPorts) {
    return Joiner.on(",").join(Lists.transform(hostsAndPorts, Functions.toStringFunction()));
  }

  // Create the list which contains the outcome of 'a - b', i.e., elements in a but not in b.
  public static <T> List<T> ListDiff(List<T> a, List<T> b) {
    List<T> diff = new ArrayList<T> (a.size());
    diff.addAll(a);
    diff.removeAll(b);

    return diff;
  }

  public static String CHARACTERS="ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  // Helper to create a random string of length numChars from characters in CHARACTERS.
  public static String randomString(Random rng, int numChars) {
    if (numChars < 1) {
      return "";
    }

    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < numChars; i++) {
      sb.append(CHARACTERS.charAt(rng.nextInt(CHARACTERS.length())));
    }

    return sb.toString();
  }

  // In place of apache StringUtils.indexInArray(), check if needle is in haystack.
  public static boolean existsInList(String needle, List<String> haystack) {
    for (String something : haystack) {
      if (something.equals(needle))
        return true;
    }

    return false;
  }

  // Convert node details to list of host/ports.
  public static List<HostAndPort> getHostPortList(Collection<NodeDetails> nodes) {
    List<HostAndPort> curServers = new ArrayList<HostAndPort>();
    for (NodeDetails node : nodes) {
      curServers.add(HostAndPort.fromParts(node.cloudInfo.private_ip, node.masterRpcPort));
    }
    return curServers;
  }

  /**
   * Returns a list of Inet address objects in the proxy tier. This is needed by Cassandra clients.
   */
  public static List<InetSocketAddress> getNodesAsInet(UUID universeUUID) {
    Universe universe = Universe.get(universeUUID);
    List<InetSocketAddress> inetAddrs = new ArrayList<>();
    for (String address : universe.getYQLServerAddresses().split(",")) {
      String[] splitAddress = address.split(":");
      String privateIp = splitAddress[0];
      int yqlRPCPort = Integer.parseInt(splitAddress[1]);
      inetAddrs.add(new InetSocketAddress(privateIp, yqlRPCPort));
    }
    return inetAddrs;
  }

  /**
   * Returns UUID representation of ID string without dashes
   * For eg. 87d2d6473b3645f7ba56d9e3f7dae239 becomes 87d2d647-3b36-45f7-ba56-d9e3f7dae239
   */
  public static UUID getUUIDRepresentation(String id) {
    if (id.length() != 32 || id.contains("-")) {
      return null;
    } else {
      String uuidWithHyphens = id.replaceAll(
        "(\\w{8})(\\w{4})(\\w{4})(\\w{4})(\\w{12})",
        "$1-$2-$3-$4-$5");
      return UUID.fromString(uuidWithHyphens);
    }
  }

  /**
   * Returns a map of nodes in the ToBeAdded state in the given set of nodes.
   * @param nodes nodes to examine for the Create/Edit operation
   * @return Map of AZUUID to number of desired nodes in the AZ
   */
  public static HashMap<UUID, Integer> toBeAddedAzUuidToNumNodes(Collection<NodeDetails> nodes) {
    HashMap<UUID, Integer> toBeAddedAzUUIDToNumNodes = new HashMap<>();
    if (nodes == null || nodes.isEmpty()) {
      return toBeAddedAzUUIDToNumNodes;

    }
    for (NodeDetails currentNode: nodes) {
      if (currentNode.state == NodeDetails.NodeState.ToBeAdded) {
        UUID currentAZUUID = currentNode.azUuid;
        toBeAddedAzUUIDToNumNodes.put(currentAZUUID, toBeAddedAzUUIDToNumNodes.getOrDefault(currentAZUUID, 0) + 1);

      }
    }
    return toBeAddedAzUUIDToNumNodes;
  }

  /**
   * Create a custom node prefix name from the given parameters.
   * @param  custId customer id owing the universe.
   * @param  univName universe name.
   * @return The custom node prefix name.
   */
  public static String getNodePrefix(Long custId, String univName) {
    Customer c = Customer.find.query().where().eq("id", custId).findOne();
    if (c == null) {
      throw new RuntimeException("Invalid Customer Id: " + custId);
    }
    return String.format("yb-%s-%s", c.code, univName);
  }

  /**
   * Method returns a map of azUUID to number of master's per AZ.
   * @param nodeDetailsSet The nodeDetailSet in the universe where masters are to be mapped.
   * @return Map of azUUID to numMastersInAZ.
   */
  private static Map<UUID, Integer> getMastersToAZMap(Collection<NodeDetails> nodeDetailsSet) {
    Map<UUID, Integer> mastersToAZMap = new HashMap<>();
    for (NodeDetails currentNode : nodeDetailsSet) {
      if (currentNode.isMaster) {
        mastersToAZMap.put(currentNode.azUuid,
                           mastersToAZMap.getOrDefault(currentNode.azUuid, 0) + 1);
      } else {
        mastersToAZMap.putIfAbsent(currentNode.azUuid, 0);
      }
    }
    LOG.info("Masters to AZ :" + mastersToAZMap);
    return mastersToAZMap;
  }

  /* Helper function to check if the set of nodes are in a single AZ or spread across multiple AZ's */
  public static boolean isSingleAZ(Collection<NodeDetails> nodeDetailsSet) {
    UUID firstAZ = null;
    for (NodeDetails node : nodeDetailsSet) {
      UUID azUuid = node.azUuid;
      if (firstAZ == null) {
        firstAZ = azUuid;
        continue;
      }

      if (!firstAZ.equals(azUuid)) {
        return false;
      }
    }

    return true;
  }

  /**
   * API detects if addition of a master to the same AZ of current node makes master quorum get
   * closer to satisfying the replication factor requirements.
   * @param currentNode the node whose AZ is checked.
   * @param nodeDetailsSet collection of nodes in a universe.
   * @param numMastersToBeAdded number of masters to be added.
   * @return true if starting a master on the node will enhance master replication of the universe.
   */
  public static boolean needMasterQuorumRestore(NodeDetails currentNode,
                                                Set<NodeDetails> nodeDetailsSet,
                                                long numMastersToBeAdded) {
    Map<UUID, Integer> mastersToAZMap = getMastersToAZMap(nodeDetailsSet);

    // If this is a single AZ deploy or if no master in current AZ, then start a master.
    if (isSingleAZ(nodeDetailsSet) || mastersToAZMap.get(currentNode.azUuid) == 0) {
      return true;
    }

    Map<UUID, Integer> azToNumStoppedNodesMap = getAZToStoppedNodesCountMap(nodeDetailsSet);
    int numStoppedMasters = 0;
    for (UUID azUUID : azToNumStoppedNodesMap.keySet()) {
      if (azUUID != currentNode.azUuid &&
          (!mastersToAZMap.containsKey(azUUID) || mastersToAZMap.get(azUUID) == 0)) {
        numStoppedMasters++;
      }
    }
    LOG.info("Masters: numStopped {}, numToBeAdded {}", numStoppedMasters, numMastersToBeAdded);

    return numStoppedMasters < numMastersToBeAdded;
  }

  /**
   * Method returns a map of azuuid to number of nodes stopped per az.
   * @param nodeDetailsSet The set of nodes that need to be mapped.
   * @return Map of azUUID to num stopped nodes in that AZ.
   */
  private static Map<UUID, Integer> getAZToStoppedNodesCountMap(Set<NodeDetails> nodeDetailsSet) {
    Map<UUID, Integer> azToNumStoppedNodesMap = new HashMap<>();
    for (NodeDetails currentNode : nodeDetailsSet) {
      if (currentNode.state == NodeDetails.NodeState.Stopped ||
          currentNode.state == NodeDetails.NodeState.Removed ||
          currentNode.state == NodeDetails.NodeState.Decommissioned) {
        azToNumStoppedNodesMap.put(currentNode.azUuid,
                                   azToNumStoppedNodesMap.getOrDefault(currentNode.azUuid, 0) + 1);
      }
    }
    LOG.info("AZ to stopped count {}", azToNumStoppedNodesMap);
    return azToNumStoppedNodesMap;
  }

  /**
   * Checks if the universe needs a new master spawned on the current node.
   * @param currentNode candidate node to be used to potentially spawn a master.
   * @param universe Universe to check for under replicated masters.
   * @return true if universe has fewer number of masters than RF.
   */
  public static boolean areMastersUnderReplicated(NodeDetails currentNode,
                                                  Universe universe) {
    Cluster cluster = universe.getCluster(currentNode.placementUuid);
    if ((cluster == null) || (cluster.clusterType != ClusterType.PRIMARY)) {
      return false;
    }

    UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
    Set<NodeDetails> nodes = universeDetails.nodeDetailsSet;
    long numMasters = getNumMasters(nodes);
    int replFactor = universeDetails.getPrimaryCluster().userIntent.replicationFactor;
    LOG.info("RF = {} , numMasters = {}", replFactor, numMasters);

    return replFactor > numMasters &&
      needMasterQuorumRestore(currentNode, nodes, replFactor - numMasters);
  }

  public static String UNIV_NAME_ERROR_MESG =
      "Invalid universe name format, valid characters [a-zA-Z0-9-].";
  // Validate the universe name pattern.
  public static boolean isValidUniverseNameFormat(String univName) {
    return univName.matches("^[a-zA-Z0-9-]*$");
  }

  // Helper API to create a CSV of any keys present in existing map but not in new map.
  public static String getKeysNotPresent(Map<String, String> existing,
                                         Map<String, String> newMap) {
    Set<String> keysNotPresent = new HashSet<>();
    Set<String> existingKeySet = existing.keySet();
    Set<String> newKeySet = newMap.keySet();
    for (String key : existingKeySet) {
      if (!newKeySet.contains(key)) {
        keysNotPresent.add(key);
      }
    }
    LOG.info("KeysNotPresent  = " + keysNotPresent);

    return String.join(",", keysNotPresent);
  }

  public static JsonNode convertStringToJson(String inputString) {
    ObjectMapper mapper = new ObjectMapper();
    try {
      return mapper.readTree(inputString);
    } catch (IOException e) {
      throw new RuntimeException("Shell Response message is not a valid Json.");
    }
  }

  public static String buildURL(String host, String endpoint) {
    try {
      return new URL("https", host, endpoint).toString();
    } catch (MalformedURLException e) {
      LOG.error("Error building request URL", e);

      return null;
    }
  }

  public static String unixTimeToString(long epochSec) {
    Date date = new Date(epochSec * 1000);
    SimpleDateFormat format = new SimpleDateFormat();

    return format.format(date);
  }

  public static void writeStringToFile(File file, String contents) throws Exception {
    FileWriter writer = new FileWriter(file);
    writer.write(contents);
    writer.close();
  }

  public static String readStringFromFile(File file) throws Exception {
    StringBuilder stringBuilder = new StringBuilder();
    Scanner fileReader = new Scanner(file);
    while (fileReader.hasNextLine()) stringBuilder.append(fileReader.nextLine());
    fileReader.close();

    return stringBuilder.toString();
  }

  /**
   * Extracts the name and extension parts of a file name.
   *
   * The resulting string is the rightmost characters of fullName, starting with
   * the first character after the path separator that separates the path
   * information from the name and extension.
   *
   * The resulting string is equal to fullName, if fullName contains no path.
   *
   * @param fullName
   * @return
   */
  public static String getFileName(String fullName) {
    if (fullName == null) {
      return null;
    }
    int delimiterIndex = fullName.lastIndexOf(File.separatorChar);
    return delimiterIndex >= 0 ? fullName.substring(delimiterIndex + 1) : fullName;
  }

  public static String getFileChecksum(String file) throws IOException, NoSuchAlgorithmException {
    FileInputStream fis = new FileInputStream(file);
    byte[] byteArray = new byte[1024];
    int bytesCount = 0;

    MessageDigest digest = MessageDigest.getInstance("MD5");

    while ((bytesCount = fis.read(byteArray)) != -1) {
      digest.update(byteArray, 0, bytesCount);
    };
    fis.close();

    byte[] bytes = digest.digest();
    StringBuilder sb = new StringBuilder();
    for(int i = 0; i < bytes.length; i++) {
      sb.append(Integer.toString((bytes[i] & 0xff) + 0x100, 16).substring(1));
    }
   return sb.toString();
  }
}
