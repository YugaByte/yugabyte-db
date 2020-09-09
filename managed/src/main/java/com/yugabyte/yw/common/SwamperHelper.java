/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.google.inject.Singleton;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.Universe;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.UUID;

@Singleton
public class SwamperHelper {
  public static final Logger LOG = LoggerFactory.getLogger(SwamperHelper.class);

  /*
     Sample targets file
    [
      {
        "targets": [
          '10.150.0.64:9300', '10.150.0.49:9300', '10.150.0.62:9300',
          '10.150.0.64:7000', '10.150.0.49:7000', '10.150.0.62:7000',
          '10.150.0.64:9000', '10.150.0.49:9000', '10.150.0.62:9000',
          '10.150.0.64:11000', '10.150.0.49:11000', '10.150.0.62:11000',
          '10.150.0.64:12000', '10.150.0.49:12000', '10.150.0.62:12000'
        ],
        "labels": {
          "node_prefix": "1-a-b"
        }
      }
    ]
  */

  @Inject
  play.Configuration appConfig;

  public enum TargetType {
    INVALID_EXPORT,
    NODE_EXPORT,
    MASTER_EXPORT,
    TSERVER_EXPORT,
    REDIS_EXPORT,
    CQL_EXPORT,
    YSQL_EXPORT;

    public int getPort(NodeDetails nodeDetails) {
      switch (this) {
        case INVALID_EXPORT:
          return 0;
        case NODE_EXPORT:
          return nodeDetails.nodeExporterPort;
        case MASTER_EXPORT:
          return nodeDetails.masterHttpPort;
        case TSERVER_EXPORT:
          return nodeDetails.tserverHttpPort;
        case REDIS_EXPORT:
          return nodeDetails.redisServerHttpPort;
        case CQL_EXPORT:
          return nodeDetails.yqlServerHttpPort;
        case YSQL_EXPORT:
          return nodeDetails.ysqlServerHttpPort;
        default:
          return 0;
      }
    }
  }

  public enum LabelType {
    NODE_PREFIX,
    EXPORT_TYPE,
    EXPORTED_INSTANCE
  }

  private ObjectNode getIndividualConfig(
      Universe universe, TargetType t, Collection<NodeDetails> nodes, String exportedInstance) {
    ObjectNode target = Json.newObject();
    ArrayNode targetNodes = Json.newArray();
    nodes.forEach((node) -> {
      if (node.isActive()) {
        targetNodes.add(node.cloudInfo.private_ip + ":" + t.getPort(node));
      }
    });

    ObjectNode labels = Json.newObject();
    labels.put(
        LabelType.NODE_PREFIX.toString().toLowerCase(),
        universe.getUniverseDetails().nodePrefix);
    labels.put(
        LabelType.EXPORT_TYPE.toString().toLowerCase(),
        t.toString().toLowerCase());
    if (exportedInstance != null) {
      labels.put(LabelType.EXPORTED_INSTANCE.toString().toLowerCase(), exportedInstance);
    }

    target.set("targets", targetNodes);
    target.set("labels", labels);
    return target;
  }

  private String getSwamperFile(UUID universeUUID, String prefix) {
    String swamperFile = appConfig.getString("yb.swamper.targetPath");
    if (swamperFile == null || swamperFile.isEmpty()) {
      return null;
    }
    File swamperTargetFolder = new File(swamperFile);

    if (swamperTargetFolder.exists() && swamperTargetFolder.isDirectory()) {
      return String.format("%s/%s.%s.json",
          swamperTargetFolder.toString(), prefix, universeUUID.toString());
    }
    return null;
  }

  private void writeTargetJsonFile(String swamperFile, ArrayNode targetsJson) {
    if (swamperFile != null) {
      try {
        FileWriter file = new FileWriter(swamperFile);
        file.write((Json.prettyPrint(targetsJson)));
        file.flush();
        file.close();
        LOG.info("Wrote Swamper Target file: {}", swamperFile);

      } catch (IOException e) {
        LOG.error("Unable to write to Swamper Target JSON: {}", swamperFile);
        throw new RuntimeException(e.getMessage());
      }
    }
  }

  public void writeUniverseTargetJson(UUID universeUUID) {
    Universe universe = Universe.get(universeUUID);

    // Write out the node specific file.
    ArrayNode nodeTargets = Json.newArray();
    String swamperFile = getSwamperFile(universeUUID, "node");
    universe.getNodes().forEach((node) -> {
      nodeTargets.add(getIndividualConfig(
          universe, TargetType.NODE_EXPORT, Collections.singletonList(node), node.nodeName));
    });
    writeTargetJsonFile(swamperFile, nodeTargets);

    // Write out the yugabyte specific file.
    ArrayNode ybTargets = Json.newArray();
    swamperFile = getSwamperFile(universeUUID, "yugabyte");
    for (TargetType t : TargetType.values()) {
      if (t != TargetType.NODE_EXPORT && t != TargetType.INVALID_EXPORT) {
        universe.getNodes().forEach((node) -> {
          // Since some nodes might not be active (for example removed),
          // we do not want to add them to the swamper targets.
          if (node.isActive()) {
            ybTargets.add(getIndividualConfig(
              universe, t, Collections.singletonList(node), node.nodeName
            ));
          }
        });
      }
    }
    writeTargetJsonFile(swamperFile, ybTargets);
  }

  private void removeUniverseTargetJson(UUID universeUUID, String prefix) {
    String swamperFile = getSwamperFile(universeUUID, prefix);
    if (swamperFile != null) {
      LOG.info("Going to delete the file... {}", swamperFile);
      File file = new File(swamperFile);

      if (file.exists()) {
        LOG.info("Deleting Swamper Target file: {}", swamperFile);
        file.delete();
      }
    }
  }

  public void removeUniverseTargetJson(UUID universeUUID) {
    // TODO: make these constants / enums.
    removeUniverseTargetJson(universeUUID, "node");
    removeUniverseTargetJson(universeUUID, "yugabyte");
  }
}

