// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.common.AssertHelper.*;
import static com.yugabyte.yw.common.FakeApiHelper.doRequestWithAuthToken;
import static com.yugabyte.yw.common.FakeApiHelper.doRequestWithAuthTokenAndBody;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThat;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.hamcrest.core.StringContains.containsString;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Mockito.doNothing;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;
import static play.inject.Bindings.bind;
import static play.mvc.Http.Status.OK;
import static play.test.Helpers.contentAsString;
import org.mockito.Mock;

import java.util.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.CallHome;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.HealthChecker;
import com.yugabyte.yw.commissioner.tasks.CommissionerBaseTest;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.NodeActionType;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.forms.ImportUniverseFormData;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ImportedState;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Capability;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Users;

import org.junit.Before;
import org.junit.Ignore;
import org.junit.Test;
import org.yb.client.YBClient;
import org.yb.client.ListTabletServersResponse;
import org.yb.util.ServerInfo;

import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

public class ImportControllerTest extends CommissionerBaseTest {
  private static String MASTER_ADDRS = "127.0.0.1:7100,127.0.0.2:7100,127.0.0.3:7100";
  private Customer customer;
  private Users user;
  private String authToken;
  private YBClient mockClient;
  private ListTabletServersResponse mockResponse;

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer();
    user = ModelFactory.testUser(customer);
    authToken = user.createAuthToken();
    mockClient = mock(YBClient.class);
    mockResponse = mock(ListTabletServersResponse.class);
    when(mockApiHelper.getRequest(any(String.class))).thenReturn(Json.newObject());
    when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    when(mockYBClient.getClient(any())).thenReturn(mockClient);
    when(mockClient.waitForServer(any(), anyLong())).thenReturn(true);
    when(mockResponse.getTabletServersCount()).thenReturn(3);
    List<ServerInfo> mockTabletSIs = new ArrayList<ServerInfo>();
    ServerInfo si = new ServerInfo("UUID1", "127.0.0.1", 9100, false, "ALIVE");
    mockTabletSIs.add(si);
    si = new ServerInfo("UUID2", "127.0.0.2", 9100, false, "ALIVE");
    mockTabletSIs.add(si);
    si = new ServerInfo("UUID3", "127.0.0.3", 9100, false, "ALIVE");
    mockTabletSIs.add(si);
    when(mockResponse.getTabletServersList()).thenReturn(mockTabletSIs);
    try {
      when(mockClient.listTabletServers()).thenReturn(mockResponse);
      doNothing().when(mockClient).waitForMasterLeader(anyLong());
    } catch (Exception e) {
      e.printStackTrace();
    }
  }

  @Test
  public void testImportUniverseMultiStep() {
    testImportUniverse(false);
  }

  @Test
  public void testImportUniverseSingleStep() {
    testImportUniverse(true);
  }

  public void testImportUniverse(boolean singleStep) {

    String url = "/api/customers/" + customer.uuid + "/universes/import";
    String univUUID = "2565538e-b7b3-4065-8eb9-7e96ddcb863c";
    ObjectNode bodyJson = Json.newObject()
                              .put("universeName", "importUniv")
                              .put("masterAddresses", MASTER_ADDRS)
                              .put("universeUUID", univUUID);

    JsonNode json = null;
    Universe universe = null;
    if (singleStep) {
      bodyJson.put("singleStep", "true");
    }

    when(mockApiHelper.getBody(any())).thenReturn("Connection refused");
    when(mockApiHelper.getHeaderStatus(any())).thenReturn(Json.newObject().put("status", "OK"));

    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    if (!singleStep) {
      // Master phase
      assertOk(result);
      json = Json.parse(contentAsString(result));
      assertValueAtPath(json, "/checks/create_db_entry", "OK");
      assertValueAtPath(json, "/checks/check_masters_are_running", "OK");
      assertValueAtPath(json, "/checks/check_master_leader_election", "OK");
      assertValue(json, "state", "IMPORTED_MASTERS");
      assertValue(json, "universeName", "importUniv");
      assertValue(json, "masterAddresses", MASTER_ADDRS);
      assertValue(json, "universeUUID", univUUID);

      universe = Universe.get(UUID.fromString(univUUID));
      assertEquals(universe.getUniverseDetails().importedState, ImportedState.MASTERS_ADDED);
      assertEquals(universe.getUniverseDetails().capability, Capability.READ_ONLY);

      // Tserver phase
      bodyJson.put("currentState", json.get("state").asText())
              .put("universeUUID", univUUID);
      result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
      assertOk(result);
      json = Json.parse(contentAsString(result));

      assertValue(json, "state", "IMPORTED_TSERVERS");
      assertValueAtPath(json, "/checks/find_tservers_list", "OK");
      assertValueAtPath(json, "/checks/check_tservers_are_running", "OK");
      assertValueAtPath(json, "/checks/check_tserver_heartbeats", "OK");
      assertValue(json, "universeName", "importUniv");
      assertValue(json, "masterAddresses", MASTER_ADDRS);
      assertValue(json, "tservers_count", "3");
      universe = Universe.get(UUID.fromString(univUUID));
      assertEquals(universe.getUniverseDetails().importedState, ImportedState.TSERVERS_ADDED);
      assertEquals(universe.getUniverseDetails().capability, Capability.READ_ONLY);

      // Finish. Default expectation is that imported universes do not have node exporter running.
      bodyJson.put("currentState", json.get("state").asText());
      result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
      assertOk(result);
      json = Json.parse(contentAsString(result));
    } else {
      assertOk(result);
      System.out.println("query output was " + contentAsString(result));
      json = Json.parse(contentAsString(result));
      assertValueAtPath(json, "/checks/create_db_entry", "OK");
      assertValueAtPath(json, "/checks/check_masters_are_running", "OK");
      assertValueAtPath(json, "/checks/check_master_leader_election", "OK");
      assertValueAtPath(json, "/checks/find_tservers_list", "OK");
      assertValueAtPath(json, "/checks/check_tservers_are_running", "OK");
      assertValueAtPath(json, "/checks/check_tserver_heartbeats", "OK");
      assertValue(json, "universeName", "importUniv");
      assertValue(json, "masterAddresses", MASTER_ADDRS);
      assertValue(json, "tservers_count", "3");
      universe = Universe.get(UUID.fromString(univUUID));
      assertEquals(universe.getUniverseDetails().capability, Capability.READ_ONLY);
    }

    assertValue(json, "state", "FINISHED");
    assertValueAtPath(json, "/checks/create_prometheus_config", "OK");
    assertThat(json.get("checks").get("node_exporter").asText(),
        allOf(notNullValue(), containsString("OK")));
    assertThat(json.get("checks").get("node_exporter_ip_error_map").asText(),
            allOf(notNullValue(), containsString("127.0.0")));
    assertValue(json, "universeUUID", univUUID);
    assertEquals(universe.getUniverseDetails().capability, Capability.READ_ONLY);

    int numAuditsExpected = (singleStep ? 1 : 3);
    assertAuditEntry(numAuditsExpected, customer.uuid);

    // Confirm customer knows about this universe and has correct node names/ips.
    url = "/api/customers/" + customer.uuid + "/universes/" + univUUID;
    result = doRequestWithAuthToken("GET", url, authToken);
    assertOk(result);
    json = Json.parse(contentAsString(result));
    JsonNode universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    JsonNode nodeDetailsMap = universeDetails.get("nodeDetailsSet");
    assertNotNull(nodeDetailsMap);
    int numNodes = 0;
    for (Iterator<JsonNode> it = nodeDetailsMap.elements(); it.hasNext();) {
      JsonNode node = it.next();
      int nodeIdx = node.get("nodeIdx").asInt();
      assertValue(node, "nodeName", "yb-tc-importUniv-n" + nodeIdx);
      assertThat(node.get("cloudInfo").get("private_ip").asText(),
                 allOf(notNullValue(), containsString("127.0.0")));
      numNodes++;
    }
    assertEquals(3, numNodes);

    // Provider should have the instance type.
    UUID provUUID = Provider.get(customer.uuid, CloudType.local).uuid;
    url = "/api/customers/" + customer.uuid + "/providers/" + provUUID + "/instance_types/" +
          ImportUniverseFormData.DEFAULT_INSTANCE;
    result = doRequestWithAuthToken("GET", url, authToken);
    assertOk(result);
    json = Json.parse(contentAsString(result));
    assertEquals(json.get("instanceTypeCode").asText(), ImportUniverseFormData.DEFAULT_INSTANCE);
    assertEquals(json.get("providerCode").asText(), CloudType.local.toString());

    // Edit should fail.
    bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", universe.name)
      .put("numNodes", 5)
      .put("replicationFactor", 3);
    bodyJson.set("clusters", Json.newArray().add(Json.newObject().set("userIntent", userIntentJson)));
    url = "/api/customers/" + customer.uuid + "/universes/" + univUUID;
    result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);
    assertBadRequest(result, "cannot be edited");

    // Node ops should fail.
    url = "/api/customers/" + customer.uuid + "/universes/" + univUUID + "/nodes/" +
          nodeDetailsMap.elements().next().get("nodeName").asText();
    bodyJson.put("nodeAction", NodeActionType.REMOVE.name());
    result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);
    assertBadRequest(result, "Node actions cannot be performed on universe");

    // Delete should succeed.
    url = "/api/customers/" + customer.uuid + "/universes/" + univUUID;
    result = doRequestWithAuthToken("DELETE", url, authToken);
    assertOk(result);
    json = Json.parse(contentAsString(result));
    UUID taskUUID = UUID.fromString(json.get("taskUUID").asText());
    TaskInfo deleteTaskInfo = null;
    try {
      deleteTaskInfo = waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    assertNotNull(deleteTaskInfo);
    assertValue(Json.toJson(deleteTaskInfo), "taskState", "Success");
    assertAuditEntry(numAuditsExpected + 1, customer.uuid);

    url = "/api/customers/" + customer.uuid + "/universes/" + univUUID;
    result = doRequestWithAuthToken("GET", url, authToken);
    String expectedResult = String.format("No universe found with UUID: %s", univUUID);
    assertBadRequest(result, expectedResult);

    try {
      universe = Universe.get(UUID.fromString(univUUID));
    } catch (RuntimeException e) {
      assertThat(e.getMessage(),
                 allOf(notNullValue(), containsString("Cannot find universe")));
    }
  }

  @Test
  public void testInvalidAddressImport() {
    String url = "/api/customers/" + customer.uuid + "/universes/import";
    ObjectNode bodyJson = Json.newObject()
                              .put("universeName", "importUniv")
                              .put("masterAddresses", "incorrect_format");
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, "Could not parse host:port from masterAddresseses: incorrect_format");
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testInvalidStateImport() {
    String url = "/api/customers/" + customer.uuid + "/universes/import";
    ObjectNode bodyJson = Json.newObject()
                              .put("universeName", "importUniv")
                              .put("masterAddresses", MASTER_ADDRS)
                              .put("currentState",
                                   ImportUniverseFormData.State.IMPORTED_TSERVERS.name());
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, "Valid universe uuid needs to be set.");
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testFailedMasterImport() {
    when(mockClient.waitForServer(any(), anyLong())).thenThrow(IllegalStateException.class);
    String url = "/api/customers/" + customer.uuid + "/universes/import";
    ObjectNode bodyJson = Json.newObject()
                              .put("universeName", "importUniv")
                              .put("masterAddresses", MASTER_ADDRS);
    // Master phase
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertInternalServerError(result, "java.lang.RuntimeException: WaitForServer");
    JsonNode resultJson = Json.parse(contentAsString(result));
    String univUUID = resultJson.get("error").get("universeUUID").asText();
    assertNotNull(univUUID);
    assertNotNull(resultJson.get("error").get("checks").get("create_db_entry"));
    assertEquals(resultJson.get("error").get("checks").get("create_db_entry").asText(), "OK");
    assertNotNull(resultJson.get("error").get("checks").get("check_masters_are_running"));
    assertEquals(resultJson.get("error").get("checks").get("check_masters_are_running").asText(),
                 "FAILURE");
    Universe universe = Universe.get(UUID.fromString(univUUID));
    assertEquals(universe.getUniverseDetails().importedState, ImportedState.STARTED);
    assertEquals(universe.getUniverseDetails().capability, Capability.READ_ONLY);
    assertFalse(universe.getUniverseDetails().isUniverseEditable());
    assertAuditEntry(1, customer.uuid);
  }
}
