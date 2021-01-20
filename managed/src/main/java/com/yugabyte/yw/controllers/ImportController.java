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

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.util.concurrent.ThreadFactoryBuilder;
import com.google.inject.Inject;

import com.yugabyte.yw.commissioner.ITask;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.ConfigHelper;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.forms.ImportUniverseFormData;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.ImportUniverseFormData.State;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ImportedState;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Capability;
import com.yugabyte.yw.forms.UniverseTaskParams;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.InstanceType.InstanceTypeDetails;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Universe.UniverseUpdater;
import com.yugabyte.yw.models.helpers.CloudSpecificInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementCloud;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementRegion;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementAZ;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.ListMastersResponse;
import org.yb.client.ListTabletServersResponse;
import org.yb.client.YBClient;
import org.yb.util.ServerInfo;


import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.*;

public class ImportController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(ImportController.class);

  // Threadpool to run user submitted tasks.
  static ExecutorService executor;

  // Minimum number of concurrent tasks to execute at a time.
  private static final int TASK_THREADS = 200;

  // The maximum time that excess idle threads will wait for new tasks before terminating.
  // The unit is specified in the API (and is seconds).
  private static final long THREAD_ALIVE_TIME = 60L;

  // The RPC timeouts.
  private static final Duration RPC_TIMEOUT_MS = Duration.ofMillis(5000L);

  // Expected string for node exporter http request.
  private static final String NODE_EXPORTER_RESP = "Node Exporter";

  @Inject
  FormFactory formFactory;

  @Inject
  YBClientService ybService;

  @Inject
  ApiHelper apiHelper;

  @Inject
  ConfigHelper configHelper;

  public Result importUniverse(UUID customerUUID) {
    // Get the submitted form data.
    Form<ImportUniverseFormData> formData =
        formFactory.form(ImportUniverseFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    ObjectNode results = Json.newObject();
    ObjectNode checks = Json.newObject();
    results.put("checks", checks);
    ImportUniverseFormData importForm = formData.get();

    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid customer uuid : " + customerUUID.toString());
    }

    Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));

    if (importForm.singleStep) {
      importForm.currentState = State.BEGIN;
      Result res = importUniverseMasters(importForm, customer, results);
      if (res.status() != Http.Status.OK) {
        return res;
      }
      res = importUniverseTservers(importForm, customer, results);
      if (res.status() != Http.Status.OK) {
        return res;
      }
      return finishUniverseImport(importForm, customer, results);
    } else {
      switch (importForm.currentState) {
        case BEGIN:
          return importUniverseMasters(importForm, customer, results);
        case IMPORTED_MASTERS:
          return importUniverseTservers(importForm, customer, results);
        case IMPORTED_TSERVERS:
          return finishUniverseImport(importForm, customer, results);
        case FINISHED:
          return ApiResponse.success(results);
        default:
          return ApiResponse.error(BAD_REQUEST,
                              "Unknown current state: " + importForm.currentState.toString());
      }
    }
  }

  // Helper function to convert comma seperated list of host:port into a list of host ips.
  // Returns null if there are parsing or invalid port errors.
  private Map<String, Integer> getMastersList(String masterAddresses) {
    Map<String, Integer> userMasterIpPorts = new HashMap<>();
    String nodesList[] = masterAddresses.split(",");
    for (String hostPort : nodesList) {
      String parts[] = hostPort.split(":");
      if (parts.length != 2) {
        LOG.error("Incorrect host:port format: " + hostPort);
        return null;
      }

      int portInt = 0;
      try {
        portInt = Integer.parseInt(parts[1]);
      } catch (NumberFormatException nfe) {
        LOG.error("Incorrect master rpc port '" + parts[1] + "', cannot be converted to integer.");
        return null;
      }

      userMasterIpPorts.put(parts[0], portInt);
    }
    return userMasterIpPorts;
  }

  // Helper function to verify masters are up and running on the saved set of nodes.
  // Returns true if there are no errors.
  private boolean verifyMastersRunning(UniverseDefinitionTaskParams taskParams,
                                       ObjectNode results) {
    UniverseDefinitionTaskBase checkMasters = new UniverseDefinitionTaskBase() {
      @Override
      public void run() {
        try {
          // Create the task list sequence.
          subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
          // Get the list of masters.
          // Note that at this point, we have only added the masters into the cluster.
          Set<NodeDetails> masterNodes =
              taskParams().getNodesInCluster(taskParams().getPrimaryCluster().uuid);
          // Wait for new masters to be responsive.
          createWaitForServersTasks(masterNodes, ServerType.MASTER, RPC_TIMEOUT_MS);
          // Run the task.
          subTaskGroupQueue.run();
        } catch (Throwable t) {
          LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
          throw t;
        }
      }
    };
    checkMasters.initialize(taskParams);
    // Execute the task. If it fails, sets the error in the results.
    if (!executeITask(checkMasters, "check_masters_are_running", results)) {
      return false;
    }
    return true;
  }

  // Helper function to check that a master leader exists.
  // Returns true if there are no errors.
  private boolean verifyMasterLeaderExists(UniverseDefinitionTaskParams taskParams,
                                           ObjectNode results) {
    UniverseDefinitionTaskBase checkMasterLeader = new UniverseDefinitionTaskBase() {
      @Override
      public void run() {
        try {
          // Create the task list sequence.
          subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
          // Wait for new masters to be responsive.
          createWaitForMasterLeaderTask();
          // Run the task.
          subTaskGroupQueue.run();
        } catch (Throwable t) {
          LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
          throw t;
        }
      }
    };
    checkMasterLeader.initialize(taskParams);
    // Execute the task. If it fails, return an error.
    if (!executeITask(checkMasterLeader, "check_master_leader_election", results)) {
      return false;
    }
    return true;
  }

  /**
   * Given the master addresses, create a basic universe object.
   */
  private Result importUniverseMasters(ImportUniverseFormData importForm,
                                       Customer customer,
                                       ObjectNode results) {
    String universeName = importForm.universeName;
    String masterAddresses = importForm.masterAddresses;

    if (universeName == null || universeName.isEmpty()) {
      return ApiResponse.error(BAD_REQUEST, "Null or empty universe name.");
    }

    if (!Util.isValidUniverseNameFormat(universeName)) {
      return ApiResponse.error(BAD_REQUEST, Util.UNIV_NAME_ERROR_MESG);
    }

    if (masterAddresses == null || masterAddresses.isEmpty()) {
      return ApiResponse.error(BAD_REQUEST, "Invalid master addresses list.");
    }

    masterAddresses = masterAddresses.replaceAll("\\s+","");

    //---------------------------------------------------------------------------------------------
    // Get the user specified master node ips.
    //---------------------------------------------------------------------------------------------
    Map<String, Integer> userMasterIpPorts = getMastersList(masterAddresses);
    if (userMasterIpPorts == null) {
      return ApiResponse.error(BAD_REQUEST, "Could not parse host:port from masterAddresseses: " +
                               masterAddresses);
    }

    //---------------------------------------------------------------------------------------------
    // Create a universe object in the DB with the information so far: master info, cloud, etc.
    //---------------------------------------------------------------------------------------------
    Universe universe = null;
    UniverseDefinitionTaskParams taskParams = null;
    // Attempt to find an existing universe with this id
    if (importForm.universeUUID != null) {
      try {
        universe = Universe.get(importForm.universeUUID);
      } catch (Exception e) {
        universe = null;
      }
    }

    try {
      if (null == universe) {
        universe = createNewUniverseForImport(customer, importForm,
                                            Util.getNodePrefix(customer.getCustomerId(),
                                                               universeName),
                                            universeName, userMasterIpPorts);
      }
      Provider provider = Provider.get(customer.uuid, importForm.providerType);
      Region region = Region.getByCode(provider, importForm.regionCode);
      AvailabilityZone zone = AvailabilityZone.getByCode(importForm.zoneCode);
      taskParams = universe.getUniverseDetails();
      // Update the universe object and refresh it.
      universe = addServersToUniverse(
        userMasterIpPorts, taskParams,
        provider, region, zone, true /*isMaster*/
      );
      results.with("checks").put("create_db_entry", "OK");
      results.put("universeUUID", universe.universeUUID.toString());
    } catch (Exception e) {
      results.with("checks").put("create_db_entry", "FAILURE");
      results.put("error", e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }
    taskParams = universe.getUniverseDetails();

    //---------------------------------------------------------------------------------------------
    // Verify the master processes are running on the master nodes.
    //---------------------------------------------------------------------------------------------
    if (!verifyMastersRunning(taskParams, results)) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }

    //---------------------------------------------------------------------------------------------
    // Wait for the master leader election if needed.
    //---------------------------------------------------------------------------------------------
    if (!verifyMasterLeaderExists(taskParams, results)) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }

    setImportedState(universe, ImportedState.MASTERS_ADDED);

    LOG.info("Done importing masters " + masterAddresses);
    results.put("state", State.IMPORTED_MASTERS.toString());
    results.put("universeName", universeName.toString());
    results.put("masterAddresses", masterAddresses.toString());

    return ApiResponse.success(results);
  }

  private void setImportedState(Universe universe, ImportedState newState) {
    UniverseUpdater updater = new UniverseUpdater() {
      public void run(Universe universe) {
         UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
         universeDetails.importedState = newState;
         universe.setUniverseDetails(universeDetails);
      }
    };
    Universe.saveDetails(universe.universeUUID, updater, false);
  }

  /**
   * Import the various tablet servers from the masters.
   */
  private Result importUniverseTservers(ImportUniverseFormData importForm,
                                        Customer customer,
                                        ObjectNode results) {
    String masterAddresses = importForm.masterAddresses;
    if (importForm.universeUUID == null || importForm.universeUUID.toString().isEmpty()) {
      results.put("error", "Valid universe uuid needs to be set instead of " +
                           importForm.universeUUID);
      return ApiResponse.error(BAD_REQUEST, results);
    }
    masterAddresses = masterAddresses.replaceAll("\\s+","");

    Universe universe = null;
    try {
      universe = Universe.get(importForm.universeUUID);
    } catch (RuntimeException re) {
      String errMsg= "Invalid universe UUID: " + importForm.universeUUID + ", universe not found.";
      LOG.error(errMsg);
      results.put("error", errMsg);
      return ApiResponse.error(BAD_REQUEST, results);
    }

    ImportedState curState = universe.getUniverseDetails().importedState;
    if (curState != ImportedState.MASTERS_ADDED) {
      results.put("error", "Unexpected universe state " + curState.name() + " expecteed " +
                           ImportedState.MASTERS_ADDED.name());
      return ApiResponse.error(BAD_REQUEST, results);
    }

    UniverseDefinitionTaskParams taskParams = universe.getUniverseDetails();
    // TODO: move this into a common location.
    results.put("universeUUID", universe.universeUUID.toString());

    //---------------------------------------------------------------------------------------------
    // Verify tservers count and list.
    //---------------------------------------------------------------------------------------------
    Map<String, Integer> tservers_list = getTServers(masterAddresses, results);
    if (tservers_list.isEmpty()) {
      results.put("error", "No tservers known to the master leader in " + masterAddresses);
      ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }

    // Record the count of the tservers.
    results.put("tservers_count", tservers_list.size());
    ArrayNode arrayNode = results.putArray("tservers_list");
    for (String tserver : tservers_list.keySet()) {
      arrayNode.add(tserver);
    }

    //---------------------------------------------------------------------------------------------
    // Update the universe object in the DB with new information : complete set of nodes.
    //---------------------------------------------------------------------------------------------
    // Find the provider, region and zone. These should have been created during master info update.
    Provider provider = Provider.get(customer.uuid, importForm.providerType);
    Region region = Region.getByCode(provider, importForm.regionCode);
    AvailabilityZone zone = AvailabilityZone.getByCode(importForm.zoneCode);
    // Update the universe object and refresh it.
    universe = addServersToUniverse(
      tservers_list, taskParams, provider,
      region, zone, false /*isMaster*/
    );
    // Refresh the universe definition object as well.
    taskParams = universe.getUniverseDetails();


    //---------------------------------------------------------------------------------------------
    // Verify that the tservers processes are running.
    //---------------------------------------------------------------------------------------------
    UniverseDefinitionTaskBase checkTservers = new UniverseDefinitionTaskBase() {
      @Override
      public void run() {
        try {
          // Create the task list sequence.
          subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
          // Get the list of tservers.
          Set<NodeDetails> tserverNodes =
              taskParams().getNodesInCluster(taskParams().getPrimaryCluster().uuid);
          // Wait for tservers to be responsive.
          createWaitForServersTasks(tserverNodes, ServerType.TSERVER);
          // Run the task.
          subTaskGroupQueue.run();
        } catch (Throwable t) {
          LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
          throw t;
        }
      }
    };
    checkTservers.initialize(taskParams);
    // Execute the task. If it fails, return an error.
    if (!executeITask(checkTservers, "check_tservers_are_running", results)) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }

    //---------------------------------------------------------------------------------------------
    // Wait for all these tservers to heartbeat to the master leader.
    //---------------------------------------------------------------------------------------------
    UniverseDefinitionTaskBase waitForTserverHBs = new UniverseDefinitionTaskBase() {
      @Override
      public void run() {
        try {
          // Create the task list sequence.
          subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
          // Wait for the master leader to hear from all the tservers.
          createWaitForTServerHeartBeatsTask();
          // Run the task.
          subTaskGroupQueue.run();
        } catch (Throwable t) {
          LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
          throw t;
        }
      }
    };
    waitForTserverHBs.initialize(taskParams);
    // Execute the task. If it fails, return an error.
    if (!executeITask(waitForTserverHBs, "check_tserver_heartbeats", results)) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }

    LOG.info("Verified " + tservers_list.size() + " tservers present and imported them.");

    setImportedState(universe, ImportedState.TSERVERS_ADDED);

    results.put("state", State.IMPORTED_TSERVERS.toString());
    results.put("masterAddresses", masterAddresses.toString());
    results.put("universeName", importForm.universeName.toString());

    return ApiResponse.success(results);
  }

  /**
   * Finalizes the universe in the database by:
   *   - setting up Prometheus config for metrics.
   *   - adding the universe to the active list of universes for this customer.
   *   - checking if node_exporter is reachable on all the nodes.
   */
  private Result finishUniverseImport(ImportUniverseFormData importForm,
                                      Customer customer,
                                      ObjectNode results) {
    if (importForm.universeUUID == null || importForm.universeUUID.toString().isEmpty()) {
      results.put("error", "Valid universe uuid needs to be set.");
      return ApiResponse.error(BAD_REQUEST, results);
    }

    Universe universe = null;
    try {
      universe = Universe.get(importForm.universeUUID);
    } catch (RuntimeException re) {
      String errMsg= "Invalid universe UUID: " + importForm.universeUUID + ", universe not found.";
      LOG.error(errMsg);
      results.put("error", errMsg);
      return ApiResponse.error(BAD_REQUEST, results);
    }

    ImportedState curState = universe.getUniverseDetails().importedState;
    if (curState != ImportedState.TSERVERS_ADDED) {
      results.put("error", "Unexpected universe state " + curState.name() + " expecteed " +
                           ImportedState.TSERVERS_ADDED.name());
      return ApiResponse.error(BAD_REQUEST, results);
    }

    UniverseDefinitionTaskParams taskParams = universe.getUniverseDetails();
    // TODO: move to common location.
    results.put("universeUUID", universe.universeUUID.toString());

    //---------------------------------------------------------------------------------------------
    // Configure metrics.
    //---------------------------------------------------------------------------------------------

    // TODO: verify we can reach the various YB ports.

    UniverseDefinitionTaskBase createPrometheusConfig = new UniverseDefinitionTaskBase() {
      @Override
      public void run() {
        try {
          // Create the task list sequence.
          subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);
          // Create a Prometheus config to pull from targets.
          createSwamperTargetUpdateTask(false /* removeFile */);
          // Run the task.
          subTaskGroupQueue.run();
        } catch (Throwable t) {
          LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
          throw t;
        }
      }
    };
    createPrometheusConfig.initialize(taskParams);
    // Execute the task. If it fails, return an error.
    if (!executeITask(createPrometheusConfig, "create_prometheus_config", results)) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, results);
    }

    //---------------------------------------------------------------------------------------------
    // Check if node_exporter is enabled on all nodes.
    //---------------------------------------------------------------------------------------------
    Map<String, String> nodeExporterIPsToError = new HashMap<String, String>();
    for (NodeDetails node : universe.getTServers()) {
      String nodeIP = node.cloudInfo.private_ip;
      int nodeExporterPort = universe.getUniverseDetails().communicationPorts.nodeExporterPort;
      String basePage = "http://" + nodeIP + ":" + nodeExporterPort;
      ObjectNode resp = apiHelper.getHeaderStatus(basePage + "/metrics");
      if (resp.isNull() || !resp.get("status").asText().equals("OK")) {
        nodeExporterIPsToError.put(nodeIP,
            resp.isNull() ? "Invalid response" : resp.get("status").asText());
      } else {
        String body = apiHelper.getBody(basePage);
        if (!body.contains(NODE_EXPORTER_RESP)) {
          nodeExporterIPsToError.put(nodeIP,
              basePage + "response does not contain '" + NODE_EXPORTER_RESP + "'");
        }
      }
    }
    results.with("checks").put("node_exporter", "OK");
    LOG.info("Errors per node: " + nodeExporterIPsToError.toString());
    if (!nodeExporterIPsToError.isEmpty()) {
      results.with("checks").put("node_exporter_ip_error_map", nodeExporterIPsToError.toString());
    }

    //---------------------------------------------------------------------------------------------
    // Create a simple redis table if needed.
    //---------------------------------------------------------------------------------------------


    //---------------------------------------------------------------------------------------------
    // Update the DNS entry for all the clusters in this universe.
    //---------------------------------------------------------------------------------------------


    //---------------------------------------------------------------------------------------------
    // Finalize universe entry in DB
    //---------------------------------------------------------------------------------------------
    // Add the universe to the current user account
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    results.put("state", State.FINISHED.toString());

    LOG.info("Completed " + universe.universeUUID + " import.");

    return ApiResponse.success(results);
  }


  /**
   * Helper function to add a list of servers into an existing universe.
   * It creates a new node entry if the node does not exist, otherwise it sets the
   * appropriate flag (master/tserver) on the existing node.
   */
  private Universe addServersToUniverse(Map<String, Integer> tserverList,
                                         UniverseDefinitionTaskParams taskParams,
                                         Provider provider, Region region, AvailabilityZone zone,
                                         boolean isMaster) {
    // Update the node details and persist into the DB.
    UniverseUpdater updater = new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        // Get the details of the universe to be updated.
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        Cluster cluster = universeDetails.getPrimaryCluster();
        int index = cluster.index;

        for (Map.Entry<String, Integer> entry : tserverList.entrySet()) {
          // Check if this node is already present.
          NodeDetails node = universe.getNodeByPrivateIP(entry.getKey());
          if (node == null) {
            // If the node is not present, create the node details and add it.
            node = createAndAddNode(universeDetails, entry.getKey(), provider, region,
                                                  zone, index, cluster.userIntent.instanceType);

            node.isMaster = isMaster;
          }

          UniverseTaskParams.CommunicationPorts
            .setCommunicationPorts(universeDetails.communicationPorts, node);

          node.isTserver = !isMaster;
          if (isMaster) {
            node.masterRpcPort = entry.getValue();
          } else {
            node.tserverRpcPort = entry.getValue();
          }
          index++;
        }

        // Update the number of nodes in the user intent. TODO: update the correct cluster.
        cluster.userIntent.numNodes = tserverList.size();
        cluster.index = index;

        // Update the node details.
        universe.setUniverseDetails(universeDetails);
      }
    };
    // Save the updated universe object and return the updated universe.
    // saveUniverseDetails(taskParams.universeUUID);
    return Universe.saveDetails(taskParams.universeUUID, updater, false);
  }

  /**
   * This method queries the master leader and returns a list of tserver ip addresses.
   * TODO: We need to get the number of nodes information also from the end user and check that
   *       count matches what master leader provides, to ensure no unreachable/failed tservers.
   */
  private Map<String, Integer> getTServers(String masterAddresses, ObjectNode results) {
    Map<String, Integer> tservers_list = new HashMap<>();
    YBClient client = null;
    try {
      // Get the client to the YB master service.
      client = ybService.getClient(masterAddresses);

      // Fetch the tablet servers.
      ListTabletServersResponse listTServerResp = client.listTabletServers();

      for (ServerInfo tserver : listTServerResp.getTabletServersList()) {
        tservers_list.put(tserver.getHost(), tserver.getPort());
      }
      results.with("checks").put("find_tservers_list", "OK");
    } catch (Exception e) {
      LOG.error("Hit error: ", e);
      results.with("checks").put("find_tservers_list", "FAILURE");
    } finally {
      ybService.closeClient(client, masterAddresses);
    }

    return tservers_list;
  }

  /**
   * This method executes the passed in task on a threadpool and waits for it to complete. Upon a
   * successful completion of the task, it returns true and adds the stage that completed to the
   * results object. Upon a failure, it returns false and adds the appropriate error message into
   * the results object.
   */
  private boolean executeITask(ITask task, String taskName, ObjectNode results) {
    // Initialize the threadpool if needed.
    initializeThreadpool();
    // Submit the task, and get a future object.
    Future<?> future = executor.submit(task);
    try {
      // Wait for the task to complete.
      future.get();
      // Indicate that this task executed successfully.
      results.with("checks").put(taskName, "OK");
    } catch (Exception e) {
      // If this task failed, return the failure and the reason.
      results.with("checks").put(taskName, "FAILURE");
      results.put("error", e.getMessage());
      LOG.error("Failed to execute " + taskName, e);
      return false;
    }
    return true;
  }

  /**
   * Creates a new universe object for the purpose of importing it. Note that we would not have all
   * the information about the universe at this point, so we are just creating the master ip parts.
   */
  private Universe createNewUniverseForImport(Customer customer, ImportUniverseFormData importForm,
                                              String nodePrefix, String universeName,
                                              Map<String, Integer> userMasterIpPorts) {
    // Find the provider by the code given, or create a new provider if one does not exist.
    Provider provider = Provider.get(customer.uuid, importForm.providerType);
    if (provider == null) {
      provider = Provider.create(customer.uuid, importForm.providerType, importForm.cloudName);
    }
    // Find the region by the code given, or create a new provider if one does not exist.
    Region region = Region.getByCode(provider, importForm.regionCode);
    if (region == null) {
      // TODO: Find real coordinates instead of assuming somwhere in US west.
      double defaultLat = 37;
      double defaultLong = -121;
      region = Region.create(
        provider, importForm.regionCode, importForm.regionName, null, defaultLat, defaultLong);
    }
    // Find the zone by the code given, or create a new provider if one does not exist.
    AvailabilityZone zone = AvailabilityZone.getByCode(importForm.zoneCode);
    if (zone == null) {
      zone = AvailabilityZone.create(region, importForm.zoneCode, importForm.zoneName, null);
    }

    // Create the universe definition task params.
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    // We will assign this universe a random UUID if not given one.
    if (importForm.universeUUID == null) {
      taskParams.universeUUID = UUID.randomUUID();
    } else {
      taskParams.universeUUID = importForm.universeUUID;
    }
    taskParams.nodePrefix = nodePrefix;
    taskParams.importedState = ImportedState.STARTED;
    taskParams.capability = Capability.READ_ONLY;

    // Set the various details in the user intent.
    UniverseDefinitionTaskParams.UserIntent userIntent =
        new UniverseDefinitionTaskParams.UserIntent();
    userIntent.universeName = universeName;
    userIntent.provider = provider.uuid.toString();
    userIntent.regionList = new ArrayList<UUID>();
    userIntent.regionList.add(region.uuid);
    userIntent.providerType = importForm.providerType;
    userIntent.instanceType = importForm.instanceType;
    userIntent.replicationFactor = importForm.replicationFactor;
    userIntent.enableYSQL = true;
    // Currently using YW version instead of YB version.
    // TODO: #1842: Create YBClient endpoint for getting ybSoftwareVersion.
    userIntent.ybSoftwareVersion = (String) configHelper.getConfig(
      ConfigHelper.ConfigType.SoftwareVersion).get("version");

    InstanceType.upsert(importForm.providerType.toString(), importForm.instanceType.toString(),
                        0 /* numCores */, 0.0 /* memSizeGB */, new InstanceTypeDetails());

    // Placement info for imports default to the current cluster.
    PlacementInfo placementInfo = new PlacementInfo();

    PlacementCloud placementCloud = new PlacementCloud();
    placementCloud.uuid = provider.uuid;
    placementCloud.code = provider.code;
    placementCloud.regionList = new ArrayList<>();

    PlacementRegion placementRegion = new PlacementRegion();
    placementRegion.uuid = region.uuid;
    placementRegion.name = region.name;
    placementRegion.code = region.code;
    placementRegion.azList = new ArrayList<>();
    placementCloud.regionList.add(placementRegion);

    PlacementAZ placementAZ = new PlacementAZ();
    placementAZ.name = zone.name;
    placementAZ.subnet = zone.subnet;
    placementAZ.replicationFactor = 1;
    placementAZ.uuid = zone.uuid;
    placementAZ.numNodesInAZ = 1;
    placementRegion.azList.add(placementAZ);

    placementInfo.cloudList.add(placementCloud);

    // Add the placement info and user intent.
    Cluster cluster = taskParams.upsertPrimaryCluster(userIntent, placementInfo);
    // Create the node details set. This is a partial set that contains only the master nodes.
    taskParams.nodeDetailsSet = new HashSet<>();

    cluster.index = 1;

    // Return the universe we just created.
    return Universe.create(taskParams, customer.getCustomerId());
  }

  // TODO: (Daniel) - Do I need to add communictionPorts here?
  // Create a new node with the given placement information.
  private NodeDetails createAndAddNode(UniverseDefinitionTaskParams taskParams, String nodeIP,
                                       Provider provider, Region region, AvailabilityZone zone,
                                       int index, String instanceType) {
    NodeDetails nodeDetails = new NodeDetails();
    // Set the node index and node details.
    nodeDetails.nodeIdx = index;
    nodeDetails.nodeName = taskParams.nodePrefix + Universe.NODEIDX_PREFIX + nodeDetails.nodeIdx;
    nodeDetails.azUuid = zone.uuid;

    // Set this node as a part of the primary cluster.
    nodeDetails.placementUuid = taskParams.getPrimaryCluster().uuid;

    // Set this node as live and running.
    nodeDetails.state = NodeDetails.NodeState.Live;

    // Set the node name and the private ip address. TODO: Set public ip.
    nodeDetails.cloudInfo = new CloudSpecificInfo();
    nodeDetails.cloudInfo.private_ip = nodeIP;
    nodeDetails.cloudInfo.cloud = provider.code;
    nodeDetails.cloudInfo.region = region.code;
    nodeDetails.cloudInfo.az = zone.code;
    nodeDetails.cloudInfo.instance_type = instanceType;

    taskParams.nodeDetailsSet.add(nodeDetails);

    return nodeDetails;
  }

  private void initializeThreadpool() {
    if (executor != null) {
      return;
    }

    // Initialize the tasks threadpool.
    ThreadFactory namedThreadFactory =
        new ThreadFactoryBuilder().setNameFormat("Import-Pool-%d").build();
    // Create an task pool which can handle an unbounded number of tasks, while using an initial set
    // of threads that get spawned upto TASK_THREADS limit.
    executor =
        new ThreadPoolExecutor(TASK_THREADS, TASK_THREADS, THREAD_ALIVE_TIME,
                               TimeUnit.SECONDS, new LinkedBlockingQueue<Runnable>(),
                               namedThreadFactory);
    LOG.trace("Started Import Thread Pool.");
  }
}
