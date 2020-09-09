// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import static org.junit.Assert.*;
import static play.mvc.Http.Status.OK;
import static org.mockito.Mockito.*;
import static play.inject.Bindings.bind;
import static play.test.Helpers.contentAsString;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.common.AssertHelper.assertAuditEntry;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.net.HostAndPort;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import org.junit.*;
import play.libs.Json;
import play.mvc.*;
import play.test.WithApplication;

import org.yb.client.ListTabletServersResponse;
import org.yb.client.YBClient;
import org.yb.util.ServerInfo;

import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.models.Customer;
import static org.mockito.Matchers.anyString;
import com.yugabyte.yw.models.Universe;

import play.Application;
import play.inject.guice.GuiceApplicationBuilder;

public class TabletServerControllerTest extends FakeDBApplication {
  private TabletServerController tabletController;
  private YBClient mockClient;
  private ListTabletServersResponse mockResponse;
  private HostAndPort testHostAndPort = HostAndPort.fromString("0.0.0.0").withDefaultPort(11);

  @Before
  public void setUp() throws Exception {
    mockClient = mock(YBClient.class);
    mockResponse = mock(ListTabletServersResponse.class);
    when(mockClient.listTabletServers()).thenReturn(mockResponse);
    when(mockClient.getLeaderMasterHostAndPort()).thenReturn(testHostAndPort);
    when(mockService.getClient(any())).thenReturn(mockClient);
    when(mockService.getClient(any(), any())).thenReturn(mockClient);
    tabletController = new TabletServerController(mockService);
    when(mockApiHelper.getRequest(anyString())).thenReturn(Json.newObject());
    tabletController.apiHelper = mockApiHelper;
  }

  @Test
  public void testListTabletServersSuccess() {
    when(mockResponse.getTabletServersCount()).thenReturn(2);
    List<ServerInfo> mockTabletSIs = new ArrayList<ServerInfo>();
    ServerInfo si = new ServerInfo("UUID1", "abc", 1001, false, "ALIVE");
    mockTabletSIs.add(si);
    si = new ServerInfo("UUID2", "abc", 2001, true, "ALIVE");
    mockTabletSIs.add(si);
    when(mockResponse.getTabletServersList()).thenReturn(mockTabletSIs);
    Result r = tabletController.list();
    JsonNode json = Json.parse(contentAsString(r));
    assertEquals(OK, r.status());
    assertTrue(json.get("servers").isArray());
 }

  @Test
  public void testListTabletServersFailure() {
    when(mockResponse.getTabletServersCount()).thenThrow(new RuntimeException("Unknown Error"));
    Result r = tabletController.list();
    assertEquals(500, r.status());
    assertEquals("Error: Unknown Error", contentAsString(r));
  }

  @Test
  public void testListTabletServersWrapperSuccess() {
    Customer customer = ModelFactory.testCustomer();
    Universe u1 = createUniverse(customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(u1.universeUUID);
    customer.save();
    Result r = tabletController.listTabletServers(customer.uuid, u1.universeUUID);
    assertEquals(200, r.status());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testListTabletServersWrapperFailure() {
    when(mockApiHelper.getRequest(anyString()))
            .thenThrow(new RuntimeException("Unknown Error"));
    Customer customer = ModelFactory.testCustomer();
    Universe u1 = createUniverse(customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater());
    customer.addUniverseUUID(u1.universeUUID);
    customer.save();
    Result r = tabletController.listTabletServers(customer.uuid, u1.universeUUID);
    assertEquals(500, r.status());
    assertAuditEntry(0, customer.uuid);
  }
}
