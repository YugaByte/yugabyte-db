// Copyright 2020 YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.yugabyte.yw.controllers;

import java.util.List;
import java.util.Map;
import java.util.HashMap;
import java.util.UUID;
import java.util.stream.Collectors;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.google.common.collect.ImmutableList;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.common.*;
import com.yugabyte.yw.forms.AlertingFormData;
import com.yugabyte.yw.forms.FeatureUpdateFormData;
import com.yugabyte.yw.forms.MetricQueryParams;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.models.helpers.CommonUtils;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Users;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;


public class CustomerController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(CustomerController.class);

  @Inject
  FormFactory formFactory;

  @Inject
  MetricQueryHelper metricQueryHelper;

  @Inject
  CloudQueryHelper cloudQueryHelper;

  public Result list() {
    ArrayNode responseJson = Json.newArray();
    Customer.getAll().forEach(c -> responseJson.add(c.getUuid().toString()));
    return ok(responseJson);
  }

  public Result index(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      ObjectNode responseJson = Json.newObject();
      responseJson.put("error", "Invalid Customer UUID:" + customerUUID);
      return badRequest(responseJson);
    }

    ObjectNode responseJson = (ObjectNode)Json.toJson(customer);
    CustomerConfig config = CustomerConfig.getAlertConfig(customerUUID);
    if (config != null) {
      responseJson.set("alertingData", config.data);
    } else {
      responseJson.set("alertingData", null);
    }
    CustomerConfig smtpConfig = CustomerConfig.getSmtpConfig(customerUUID);
    if (smtpConfig != null) {
      responseJson.set("smtpData", smtpConfig.data);
    } else {
      responseJson.set("smtpData", null);
    }
    responseJson.put("callhomeLevel", CustomerConfig.getOrCreateCallhomeLevel(customerUUID).toString());

    Users user = (Users) ctx().args.get("user");
    if (customer.getFeatures().size() != 0 && user.getFeatures().size() != 0) {
      JsonNode featureSet = user.getFeatures();
      CommonUtils.deepMerge(featureSet, customer.getFeatures());
      responseJson.put("features", featureSet);
    } else if (customer.getFeatures().size() != 0) {
      responseJson.put("features", customer.getFeatures());
    } else {
      responseJson.put("features", user.getFeatures());
    }

    return ok(responseJson);
  }

  public Result update(UUID customerUUID) {
    ObjectNode responseJson = Json.newObject();

    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      responseJson.put("error", "Invalid Customer UUID:" + customerUUID);
      return badRequest(responseJson);
    }

    JsonNode request = request().body().asJson();
    Form<AlertingFormData> formData = formFactory.form(AlertingFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      responseJson.set("error", formData.errorsAsJson());
      return badRequest(responseJson);
    }

    if (formData.get().name != null) {
      customer.name = formData.get().name;
      customer.save();
    }

    if (request.has("alertingData") || request.has("smtpData")) {

      CustomerConfig config = CustomerConfig.getAlertConfig(customerUUID);
      if (config == null && formData.get().alertingData != null) {
        CustomerConfig.createAlertConfig(customerUUID, Json.toJson(formData.get().alertingData));
      } else if (config != null && formData.get().alertingData != null) {
        config.data = Json.toJson(formData.get().alertingData);
        config.update();
      }

      CustomerConfig smtpConfig = CustomerConfig.getSmtpConfig(customerUUID);
      if (smtpConfig == null && formData.get().smtpData != null) {
        CustomerConfig.createSmtpConfig(customerUUID, Json.toJson(formData.get().smtpData));
      } else if (smtpConfig != null && formData.get().smtpData != null) {
        smtpConfig.data = Json.toJson(formData.get().smtpData);
        smtpConfig.update();
      } // In case we want to reset the smtpData and use the default mailing server.
      else if (request.has("smtpData") && formData.get().smtpData == null) {
        if (smtpConfig != null) {
          smtpConfig.delete();
        }
      }
    }

    // Features would be a nested json, so we should fetch it differently.
    JsonNode requestBody = request().body().asJson();
    if (requestBody.has("features")) {
      customer.upsertFeatures(requestBody.get("features"));
    }

    CustomerConfig.upsertCallhomeConfig(customerUUID, formData.get().callhomeLevel);

    return ok(Json.toJson(customer));
  }

  public Result delete(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);

    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }
    List<Users> users = Users.getAll(customerUUID);
    for (Users user : users) {
      user.delete();
    }

    if (customer.delete()) {
      ObjectNode responseJson = Json.newObject();
      Audit.createAuditEntry(ctx(), request());
      responseJson.put("success", true);
      return ApiResponse.success(responseJson);
    } else {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to delete Customer UUID: " + customerUUID);
    }
  }

  public Result upsertFeatures(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }

    JsonNode requestBody = request().body().asJson();
    ObjectMapper mapper = new ObjectMapper();
    FeatureUpdateFormData formData;
    try {
      formData = mapper.treeToValue(requestBody, FeatureUpdateFormData.class);
    } catch (RuntimeException | JsonProcessingException e) {
      return ApiResponse.error(BAD_REQUEST, "Invalid JSON");
    }

    try {
      customer.upsertFeatures(formData.features);
    } catch (RuntimeException e) {
      return ApiResponse.error(BAD_REQUEST, "Failed to update features: " + e.getMessage());
    }
    Audit.createAuditEntry(ctx(), request(), requestBody);
    return ok(customer.getFeatures());
  }

  public Result metrics(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);

    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }

    Form<MetricQueryParams> formData = formFactory.form(MetricQueryParams.class).bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    Map<String, String> params = formData.data();
    HashMap<String, Map<String, String>> filterOverrides = new HashMap<>();
    // Given we have a limitation on not being able to rename the pod labels in
    // kubernetes cadvisor metrics, we try to see if the metric being queried is for
    // container or not, and use pod_name vs exported_instance accordingly.
    // Expect for container metrics, all the metrics would with node_prefix and exported_instance.
    boolean hasContainerMetric = formData.get().metrics.stream().anyMatch(s -> s.startsWith("container"));
    String universeFilterLabel = hasContainerMetric ? "namespace" : "node_prefix";
    String nodeFilterLabel = hasContainerMetric ? "pod_name" : "exported_instance";
    String containerLabel = "container_name";
    String pvcLabel = "persistentvolumeclaim";

    ObjectNode filterJson = Json.newObject();
    if (!params.containsKey("nodePrefix")) {
      String universePrefixes = customer.getUniverses().stream()
        .map((universe -> universe.getUniverseDetails().nodePrefix)).collect(Collectors.joining("|"));
      filterJson.put(universeFilterLabel, String.join("|", universePrefixes));
    } else {
      // Check if it is a kubernetes deployment.
      if (hasContainerMetric) {
        if (params.containsKey("nodeName")) {
          // Get the correct namespace by appending the zone if it exists.
          String[] nodeWithZone = params.remove("nodeName").split("_");
          filterJson.put(nodeFilterLabel, nodeWithZone[0]);
          // The pod name is of the format yb-<server>-<replica_num> and we just need the
          // container, which is yb-<server>.
          String containerName = nodeWithZone[0].substring(0, nodeWithZone[0].lastIndexOf("-"));
          String pvcName = String.format("(.*)-%s", nodeWithZone[0]);
          String completeNamespace = params.remove("nodePrefix");
          if (nodeWithZone.length == 2) {
             completeNamespace = String.format("%s-%s", completeNamespace,
                                                      nodeWithZone[1]);
          }
          filterJson.put(universeFilterLabel, completeNamespace);
          filterJson.put(containerLabel, containerName);
          filterJson.put(pvcLabel, pvcName);

        } else {
          // If no nodename, we need to figure out the correct regex for the namespace.
          // We get this by getting the correct universe and then checking that the
          // provider for that universe is multi-az or not.
          final String nodePrefix = params.remove("nodePrefix");
          String completeNamespace = nodePrefix;
          List<Universe> universes =  customer.getUniverses().stream()
            .filter(u -> u.getUniverseDetails().nodePrefix.equals(nodePrefix))
            .collect(Collectors.toList());
          Provider provider = Provider.get(UUID.fromString(
            universes.get(0).getUniverseDetails().getPrimaryCluster().userIntent.provider));
          if (PlacementInfoUtil.isMultiAZ(provider)) {
            completeNamespace = String.format("%s-(.*)", completeNamespace);
          }
          filterJson.put(universeFilterLabel, completeNamespace);
        }
      } else {
        final String nodePrefix = params.remove("nodePrefix");
        filterJson.put(universeFilterLabel, nodePrefix);
        if (params.containsKey("nodeName")) {
          filterJson.put(nodeFilterLabel, params.remove("nodeName"));
        }

        filterOverrides.putAll(getFilterOverrides(customer, nodePrefix, formData.get()));
      }
    }
    if (params.containsKey("tableName")) {
      filterJson.put("table_name", params.remove("tableName"));
    }
    params.put("filters", Json.stringify(filterJson));
    try {
      JsonNode response = metricQueryHelper.query(formData.get().metrics, params, filterOverrides);
      if (response.has("error")) {
        return ApiResponse.error(BAD_REQUEST, response.get("error"));
      }
      return ApiResponse.success(response);
    } catch (RuntimeException e) {
      return ApiResponse.error(BAD_REQUEST, e.getMessage());
    }
  }

  public Result getHostInfo(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    ObjectNode hostInfo = Json.newObject();
    hostInfo.put(Common.CloudType.aws.name(), cloudQueryHelper.currentHostInfo(
        Common.CloudType.aws, ImmutableList.of("instance-id", "vpc-id", "privateIp", "region")));
    hostInfo.put(Common.CloudType.gcp.name(), cloudQueryHelper.currentHostInfo(
        Common.CloudType.gcp, null));

    return ApiResponse.success(hostInfo);
  }

  private HashMap<String, HashMap<String, String>> getFilterOverrides(
    Customer customer,
    String nodePrefix,
    MetricQueryParams mqParams) {

    HashMap<String, HashMap<String, String>> filterOverrides = new HashMap<>();
    // For a disk usage metric query, the mount point has to be modified to match the actual
    // mount point for an onprem universe.
    if (mqParams.metrics.contains("disk_usage")) {
      List<Universe> universes =  customer.getUniverses().stream()
        .filter(u -> u.getUniverseDetails().nodePrefix != null &&
                     u.getUniverseDetails().nodePrefix.equals(nodePrefix))
        .collect(Collectors.toList());
      if (universes.get(0).getUniverseDetails().getPrimaryCluster().userIntent.providerType ==
          CloudType.onprem) {
        final String mountRoots = universes.get(0).getNodes().stream().
                                  filter(n -> n.cloudInfo != null &&
                                         n.cloudInfo.mount_roots != null &&
                                         !n.cloudInfo.mount_roots.isEmpty()).
                                  map(n -> n.cloudInfo.mount_roots).
                                  findFirst().
                                  orElse("");
        // TODO: technically, this code is based on the primary cluster being onprem
        // and will return inaccurate results if the universe has a read replica that is
        // not onprem.
        if (!mountRoots.isEmpty()) {
          HashMap<String, String> mountFilters = new HashMap<>();
          mountFilters.put("mountpoint", mountRoots.replace(',', '|'));
          // convert "/storage1,/bar" to the filter "/storage1|/bar"
          filterOverrides.put("disk_usage", mountFilters);
        } else {
          LOG.debug("No mount points found in onprem universe {}", nodePrefix);
        }
      }
    }
    return filterOverrides;
  }
}
