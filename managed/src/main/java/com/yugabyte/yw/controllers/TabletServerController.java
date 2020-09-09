// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import java.util.UUID;

import org.yb.client.ListTabletServersResponse;

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.models.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.YBClient;
import play.libs.Json;
import play.mvc.Result;

public class TabletServerController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(TabletServerController.class);
  @Inject
  ApiHelper apiHelper;
  private final YBClientService ybService;

  @Inject
  public TabletServerController(YBClientService service) { this.ybService = service; }

  /**
   * This API would query for all the tabletServers using YB Client and return a JSON
   * with tablet server UUIDs
   *
   * @return Result tablet server uuids
   */
  public Result list() {
    ObjectNode result = Json.newObject();
    YBClient client = null;

    try {
      client = ybService.getClient(null);
      ListTabletServersResponse response = client.listTabletServers();
      result.put("count", response.getTabletServersCount());
      ArrayNode tabletServers = result.putArray("servers");
      response.getTabletServersList().forEach(tabletServer->{
        tabletServers.add(tabletServer.getHost());
      });
    } catch (Exception e) {
      return internalServerError("Error: " + e.getMessage());
    } finally {
      ybService.closeClient(client, null);
    }

    return ok(result);
  }

  /**
   * This API directly queries the leader master server for a list of tablet servers in a universe
   *
   * @param customerUUID UUID of the customer
   * @param universeUUID UUID of the universe
   *
   * @return Result tablet server information
   */
  public Result listTabletServers(UUID customerUUID, UUID universeUUID) {
    // Validate customer UUID
    if (Customer.get(customerUUID) == null) {
      final String errMsg = "Invalid Customer UUID: " + customerUUID;
      LOG.error(errMsg);
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    // Validate universe UUID and retrieve master leader address
    final Universe universe = Universe.get(universeUUID);
    final String masterLeaderIPAddr = universe.getMasterLeaderHostText();
    if (masterLeaderIPAddr.isEmpty()) {
      final String errMsg =
              "Could not find the master leader address in universe " + universeUUID;
      LOG.error(errMsg);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, errMsg);
    }

    JsonNode response;
    // Query master leader tablet servers endpoint
    try {
      final int masterHttpPort = universe.getUniverseDetails().communicationPorts.masterHttpPort;
      final String masterLeaderUrl = String.format(
        "http://%s:%s/api/v1/tablet-servers",
        masterLeaderIPAddr,
        masterHttpPort
      );

      response = apiHelper.getRequest(masterLeaderUrl);
    } catch (Exception e) {
      LOG.error("Failed to get list of tablet servers in universe " + universeUUID, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
    return ApiResponse.success(response);
  }
}
