// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import io.ebean.*;
import com.yugabyte.yw.forms.NodeInstanceFormData.NodeInstanceData;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
import java.util.UUID;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.libs.Json;

@Entity
public class NodeInstance extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(NodeInstance.class);

  @Id
  public UUID nodeUuid;

  @Column
  public String instanceTypeCode;

  @Column(nullable = false)
  private String nodeName;
  public String getNodeName() { return nodeName; }
  public void setNodeName(String name) {
    nodeName = name;
    // This parses the JSON if first time accessing details.
    NodeInstanceData details = getDetails();
    details.nodeName = name;
    setDetails(details);
  }

  @Column(nullable = false)
  public String instanceName;

  @Column(nullable = false)
  public UUID zoneUuid;

  @Column(nullable = false)
  public boolean inUse;

  @Column(nullable = false)
  private String nodeDetailsJson;

  // Preserving the details into a structured class.
  private NodeInstanceData nodeDetails;

  public void setDetails(NodeInstanceData details) {
    this.nodeDetails = details;
    this.nodeDetailsJson = Json.stringify(Json.toJson(this.nodeDetails));
  }
  public NodeInstanceData getDetails() {
    if (nodeDetails == null) {
      nodeDetails = Json.fromJson(
        Json.parse(nodeDetailsJson),
        NodeInstanceData.class);
    }
    return nodeDetails;
  }
  // Method sets node name to empty string and inUse to false and persists the value
  public void clearNodeDetails() {
    this.inUse = false;
    this.setNodeName("");
    this.save();
  }

  public String getDetailsJson() {
    return nodeDetailsJson;
  }

  public static final Finder<UUID, NodeInstance> find =
    new Finder<UUID, NodeInstance>(NodeInstance.class){};

  public static List<NodeInstance> listByZone(UUID zoneUuid, String instanceTypeCode) {
    List<NodeInstance> nodes = null;
    // Search in the proper AZ.
    ExpressionList<NodeInstance> exp = NodeInstance.find.query().where().eq("zone_uuid", zoneUuid);
    // Search only for nodes not in use.
    exp.where().eq("in_use", false);
    // Filter by instance type if asked to.
    if (instanceTypeCode != null) {
      exp.where().eq("instance_type_code", instanceTypeCode);
    }
    nodes = exp.findList();
    return nodes;
  }

  public static List<NodeInstance> listByProvider(UUID providerUUID) {
    String nodeQuery = "select DISTINCT n.*   from node_instance n, availability_zone az, region r, provider p " +
      " where n.zone_uuid = az.uuid and az.region_uuid = r.uuid and r.provider_uuid = " + "'"+ providerUUID + "'";
    RawSql rawSql = RawSqlBuilder.unparsed(nodeQuery).columnMapping("node_uuid",  "nodeUuid").create();
    Query<NodeInstance> query = Ebean.find(NodeInstance.class);
    query.setRawSql(rawSql);
    List<NodeInstance> list = query.findList();
    return list;
  }

  public static int deleteByProvider(UUID providerUUID) {
    String deleteNodeQuery = "delete from node_instance where zone_uuid in" +
                             " (select az.uuid from availability_zone az join region r on az.region_uuid = r.uuid and r.provider_uuid=:provider_uuid)";
    SqlUpdate deleteStmt = Ebean.createSqlUpdate(deleteNodeQuery);
    deleteStmt.setParameter("provider_uuid",  providerUUID);
    return deleteStmt.execute();
  }

  public static synchronized Map<String, NodeInstance> pickNodes(
    Map<UUID, List<String>> onpremAzToNodes, String instanceTypeCode) {
    Map<String, NodeInstance> outputMap = new HashMap<String, NodeInstance>();
    Throwable error = null;
    try {
      for (Entry<UUID, List<String>> entry : onpremAzToNodes.entrySet()) {
        UUID zoneUuid = entry.getKey();
        List<String> nodeNames = entry.getValue();
        List<NodeInstance> nodes = listByZone(zoneUuid, instanceTypeCode);
        if (nodes.size() < nodeNames.size()) {
          LOG.error("AZ {} has {} nodes of instance type {} but needs {}.",
            zoneUuid, nodes.size(), instanceTypeCode, nodeNames.size());
          throw new RuntimeException("Not enough nodes in AZ " + zoneUuid);
        }
        int index = 0;
        for (String nodeName : nodeNames) {
          NodeInstance node = nodes.get(index);
          node.inUse = true;
          node.setNodeName(nodeName);
          outputMap.put(nodeName, node);
          ++index;
        }
      }
      // All good, save to DB.
      for (NodeInstance node : outputMap.values()) {
        node.save();
      }
    } catch (Throwable t) {
      error = t;
      throw t;
    } finally {
      if (error != null) {
        outputMap = null;
        // TODO: any cleanup needed?
      }
    }
    return outputMap;
  }

  public static NodeInstance get(UUID nodeUuid) {
    NodeInstance node = NodeInstance.find.byId(nodeUuid);
    return node;
  }

  // TODO: this is a temporary hack until we manage to plumb through the node UUID through the task
  // framework.
  public static NodeInstance getByName(String name) {
    List<NodeInstance> nodes = NodeInstance.find.query().where().eq("node_name", name).findList();
    if (nodes == null || nodes.size() != 1) {
      throw new RuntimeException("Expecting to find a single node with name: " + name);
    }
    return nodes.get(0);
  }

  public static NodeInstance create(UUID zoneUuid, NodeInstanceData formData) {
    NodeInstance node = new NodeInstance();
    node.zoneUuid = zoneUuid;
    node.inUse = false;
    node.instanceTypeCode = formData.instanceType;
    String instanceName = formData.instanceName;
    if (instanceName == null) instanceName = "";
    node.instanceName = instanceName;
    node.setDetails(formData);
    node.setNodeName("");
    node.save();
    return node;
  }
}
