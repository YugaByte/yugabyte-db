// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.NodeActionType;
import com.yugabyte.yw.forms.NodeActionFormData;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.models.helpers.NodeDetails;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.google.inject.Inject;

import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.forms.NodeInstanceFormData;
import com.yugabyte.yw.forms.NodeInstanceFormData.NodeInstanceData;

import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;
import play.mvc.Results;

public class NodeInstanceController extends AuthenticatedController {
  @Inject
  FormFactory formFactory;

  @Inject
  Commissioner commissioner;

  public static final Logger LOG = LoggerFactory.getLogger(NodeInstanceController.class);

  /**
   * GET endpoint for Node data
   * @param customerUuid the customer UUID
   * @param nodeUuid the node UUID
   * @return JSON response with Node data
   */
  public Result get(UUID customerUuid, UUID nodeUuid) {
    String error = validateParams(customerUuid, null /* zoneUuid */);
    if (error != null) return ApiResponse.error(BAD_REQUEST, error);

    try {
      NodeInstance node = NodeInstance.get(nodeUuid);
      return ApiResponse.success(node);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  /**
   * GET endpoint for getting all unused nodes under a zone
   * @param customerUuid the customer UUID
   * @param zoneUuid the zone UUID
   * @return JSON response with list of nodes
   */
  public Result listByZone(UUID customerUuid, UUID zoneUuid) {
    String error = validateParams(customerUuid, zoneUuid);
    if (error != null) return ApiResponse.error(BAD_REQUEST, error);

    try {
      List<NodeInstance> nodes = NodeInstance.listByZone(zoneUuid, null /* instanceTypeCode */);
      return ApiResponse.success(nodes);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  public Result listByProvider(UUID customerUUID, UUID providerUUID) {
    List<NodeInstance> regionList;
    try {
      regionList = NodeInstance.listByProvider(providerUUID);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
    return ApiResponse.success(regionList);
  }

  /**
   * POST endpoint for creating new Node(s)
   * @param customerUuid the customer UUID
   * @param zoneUuid the zone UUID
   * @return JSON response of newly created Nodes
   */
  public Result create(UUID customerUuid, UUID zoneUuid) {
    String error = validateParams(customerUuid, zoneUuid);
    if (error != null) return ApiResponse.error(BAD_REQUEST, error);

    Form<NodeInstanceFormData> formData = formFactory.form(NodeInstanceFormData.class).bindFromRequest();
    List<NodeInstanceData> nodeDataList = formData.get().nodes;
    Map<String, NodeInstance> nodes = new HashMap<>();
    try {
      for (NodeInstanceData nodeData : nodeDataList) {
        NodeInstance node = NodeInstance.create(zoneUuid, nodeData);
        nodes.put(node.getDetails().ip, node);
      }
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return ApiResponse.success(nodes);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  /**
   * Endpoint deletes the configured instance for a provider.
   * Since instance name and instance uuid are absent in a pristine (unused) instance
   * We use IP to query for Instance and delete it
   */
  public Result deleteInstance(UUID customerUUID, UUID providerUUID, String instanceIP) {
    try {
      // Validate customer UUID and universe UUID and AWS provider.
      Customer customer = Customer.get(customerUUID);
      if (customer == null) {
        String errMsg = "Invalid Customer UUID: " + customerUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      Provider provider = Provider.get(providerUUID);
      if (provider == null) {
        String errMsg = "Invalid Provider UUID: " + providerUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }
      List<NodeInstance> nodesInProvider = NodeInstance.listByProvider(providerUUID);
      NodeInstance nodeToBeFound = null;
      for (int a = 0; a < nodesInProvider.size(); a++) {
        if (nodesInProvider.get(a).getDetails().ip.equals(instanceIP)) {
          nodeToBeFound = nodesInProvider.get(a);
        }
      }
      if (nodeToBeFound != null) {
        nodeToBeFound.delete();
        Audit.createAuditEntry(ctx(), request());
        return Results.status(OK);
      } else {
        return ApiResponse.error(BAD_REQUEST, "Node Not Found");
      }
    } catch (Exception e) {
      return ApiResponse.error(500, e.getMessage());
    }
  }

  public Result nodeAction(UUID customerUUID, UUID universeUUID, String nodeName) {
    String apiParamsErrorMessage = validateParams(customerUUID, universeUUID, nodeName);
    if (apiParamsErrorMessage != null) {
      return ApiResponse.error(BAD_REQUEST, apiParamsErrorMessage);
    }
    Form<NodeActionFormData> formData = formFactory.form(NodeActionFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    try {
      Universe universe = Universe.get(universeUUID);

      if (!universe.getUniverseDetails().isUniverseEditable()) {
        String errMsg= "Node actions cannot be performed on universe UUID " + universeUUID;
        LOG.error(errMsg);
        return ApiResponse.error(BAD_REQUEST, errMsg);
      }

      Customer customer =  Customer.get(customerUUID);
      NodeTaskParams taskParams = new NodeTaskParams();
      taskParams.universeUUID = universe.universeUUID;
      taskParams.expectedUniverseVersion = universe.version;
      taskParams.nodeName = nodeName;
      NodeActionType nodeAction = formData.get().nodeAction;

      // Check deleting/removing a node will not go below the RF
      if (nodeAction == NodeActionType.STOP || nodeAction == NodeActionType.REMOVE) {
        if (!universe.isNodeActionAllowed(nodeName, nodeAction)) {
          String errMsg = "Cannot " + nodeAction.name() + " " + nodeName + " as it will under replicate the masters.";
          LOG.error(errMsg);
          return ApiResponse.error(BAD_REQUEST, errMsg);
        }
      }

      if (nodeAction == NodeActionType.ADD || nodeAction == NodeActionType.START
          || nodeAction == NodeActionType.START_MASTER) {
        taskParams.clusters = universe.getUniverseDetails().clusters;
        taskParams.rootCA = universe.getUniverseDetails().rootCA;
        if (!CertificateInfo.isCertificateValid(taskParams.rootCA)) {
          String errMsg = String.format("The certificate %s needs info. Update the cert" +
                                        " and retry.",
                                        CertificateInfo.get(taskParams.rootCA).label);
            LOG.error(errMsg);
            return ApiResponse.error(BAD_REQUEST, errMsg);
        }
      }
      LOG.info("{} Node {} in universe={}: name={} at version={}.",
               nodeAction.toString(false), nodeName, universe.universeUUID,
               universe.name, universe.version);

      UUID taskUUID = commissioner.submit(nodeAction.getCommissionerTask(), taskParams);
      CustomerTask.create(customer, universe.universeUUID,
              taskUUID, CustomerTask.TargetType.Node,
              nodeAction.getCustomerTask(), nodeName);
      LOG.info("Saved task uuid {} in customer tasks table for universe {} : {} for node {}",
              taskUUID, universe.universeUUID, universe.name, nodeName);
      ObjectNode resultNode = Json.newObject();
      resultNode.put("taskUUID", taskUUID.toString());
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()), taskUUID);
      return ApiResponse.success(resultNode);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  private String validateParams(UUID customerUuid, UUID zoneUuid) {
    String e = null;
    e = validateClassByUuid(Customer.class, customerUuid);
    if (e != null) return e;
    e = validateClassByUuid(AvailabilityZone.class, zoneUuid);
    if (e != null) return e;
    return e;
  }

  private String validateParams(UUID customerUUID, UUID universeUUID, String nodeName) {
    String e = null;
    e = validateClassByUuid(Customer.class, customerUUID);
    if (e != null) return e;
    e = validateClassByUuid(Universe.class, universeUUID);
    if (e != null) return e;
    Universe u = Universe.get(universeUUID);
    NodeDetails nodeDetails = u.getNode(nodeName);
    if (nodeDetails == null) {
      return String.format("Invalid Node %s for Universe", nodeName);
    }
    return e;
  }

  private String validateClassByUuid(Class c, UUID uuid) {
    try {
      if (uuid != null && c.getMethod("get", UUID.class).invoke(null, uuid) == null) {
        return "Invalid " + c.getName() + "UUID: " + uuid;
      }
    } catch (Throwable t) {
      return t.getMessage();
    }
    return null;
  }
}
