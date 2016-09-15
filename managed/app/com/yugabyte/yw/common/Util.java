// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.common;

import com.google.common.base.Functions;
import com.google.common.base.Joiner;
import com.google.common.collect.Lists;
import com.google.common.net.HostAndPort;
import com.typesafe.config.Config;
import com.typesafe.config.ConfigFactory;
import com.yugabyte.yw.models.helpers.NodeDetails;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Random;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.MiniYBCluster;

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

  // Check if we want to unit test via the mini-cluster.
  public static boolean isLocalTesting() {
    String ybLocalTesting = System.getProperty("yb.local.testing");
    if (ybLocalTesting == null) {
      return false;
    }
    return true;
  }

  public static MiniYBCluster getMiniCluster(int numMasters) {
    MiniYBCluster ret = null; 
    try {
      ret = new MiniYBCluster
               .MiniYBClusterBuilder()
               .numMasters(numMasters)
               .numTservers(1)
               .defaultTimeoutMs(60)
               .build();
      LOG.info("Created new mini cluster with {} masters.", numMasters);
    } catch (Exception e) {
      String errMsg = "Could not start mini cluster with " + numMasters + " masters. " +
                      "Error: " + e.getMessage();
      LOG.error(errMsg);
      throw new RuntimeException(errMsg);
    }

    return ret;
  }

  public static void closeMiniCluster(MiniYBCluster miniCluster) {
    try {
      miniCluster.close();
    } catch (Exception e) {
      LOG.warn("Could not close mini cluster : ", e.getMessage());
    }
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

}
