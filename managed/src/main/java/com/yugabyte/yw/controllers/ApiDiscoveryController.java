// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import java.util.List;

import com.google.inject.Inject;
import com.google.inject.Provider;

import play.mvc.Result;
import play.routing.Router;
import play.routing.Router.RouteDocumentation;
import play.libs.Json;

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

public class ApiDiscoveryController extends AuthenticatedController {

  @Inject
  private Provider<Router> routesProvider;

  public Result index() {
    ArrayNode responseJson = Json.newArray();
    List<Router.RouteDocumentation> routeDocs = routesProvider.get().documentation();
    for (Router.RouteDocumentation docItem: routeDocs) {
      ObjectNode routeJsonNode = Json.newObject();
      routeJsonNode.put("method", docItem.getHttpMethod());
      routeJsonNode.put("path_pattern", docItem.getPathPattern());
      routeJsonNode.put("controller_method_invocation", docItem.getControllerMethodInvocation());
      responseJson.add(routeJsonNode);
    }
    return ok(responseJson);
  }

}
