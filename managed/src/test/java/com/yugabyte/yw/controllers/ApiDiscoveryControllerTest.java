// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.node.ArrayNode;

import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Users;

import org.junit.Before;
import org.junit.Test;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;

import java.util.Arrays;

import static org.hamcrest.CoreMatchers.*;
import static play.test.Helpers.*;
import static org.junit.Assert.*;
import static play.mvc.Http.Status.OK;
import static play.mvc.Http.Status.NOT_FOUND;
import static play.mvc.Http.Status.FORBIDDEN;
import static play.test.Helpers.fakeRequest;

public class ApiDiscoveryControllerTest extends FakeDBApplication {

  Http.Cookie validCookie;

  @Before
  public void setUp() {
    Customer customer = ModelFactory.testCustomer();
    Users user = ModelFactory.testUser(customer);
    String authToken = user.createAuthToken();
    validCookie = Http.Cookie.builder("authToken", authToken).build();
  }

  @Test
  public void testApiIndexWithoutAuth() {
    Result result = route(fakeRequest("GET", "/api/index"));
    assertEquals(FORBIDDEN, result.status());
  }

  @Test
  public void testApiIndexWithAuthBadRoute() {
    Result result = route(fakeRequest("GET", "/index").cookie(validCookie));
    assertEquals(NOT_FOUND, result.status());
  }

  @Test
  public void testApiIndexWithAuthGoodRoute() {
    Arrays.asList("/api/index", "/api/v1/index").forEach(endPoint -> {
        Result result = route(fakeRequest("GET", endPoint).cookie(validCookie));
        assertEquals(OK, result.status());
    });
  }

  @Test
  public void testApiIndexWithAuthGoodRouteHasKeys() {
    Result result = route(fakeRequest("GET", "/api/index").cookie(validCookie));
    assertEquals(OK, result.status());
    ArrayNode json = (ArrayNode)Json.parse(contentAsString(result));
    ObjectNode sampleEndpoint = (ObjectNode)json.get(0);
    assertThat(sampleEndpoint.get("method"), notNullValue());
    assertThat(sampleEndpoint.get("path_pattern"), notNullValue());
    assertThat(sampleEndpoint.get("controller_method_invocation"), notNullValue());
  }

  @Test
  public void testApiIndexWithAuthGoodRouteHasSelfDescription() {
    Result result = route(fakeRequest("GET", "/api/v1/index").cookie(validCookie));
    assertEquals(OK, result.status());
    ArrayNode json = (ArrayNode)Json.parse(contentAsString(result));
    boolean endPointFound = false;
    for (JsonNode n: json) {
        if (n.get("path_pattern").textValue().equals("/api/v1/index")) {
            endPointFound = true;
            assertTrue(n.get("method").textValue().equals("GET"));
            assertTrue(n.get("controller_method_invocation").textValue().equals(
                "com.yugabyte.yw.controllers.ApiDiscoveryController.index()"));
            break;
        }
    }
    assertTrue(endPointFound);
  }

}
