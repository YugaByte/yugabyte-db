// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static org.hamcrest.CoreMatchers.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertThat;
import static play.mvc.Http.Status.BAD_REQUEST;
import static play.mvc.Http.Status.OK;
import static play.mvc.Http.Status.UNAUTHORIZED;
import static play.test.Helpers.contentAsString;
import static play.test.Helpers.fakeRequest;
import static play.test.Helpers.route;

import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.common.ModelFactory;
import org.junit.After;
import org.junit.Test;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Result;
import play.test.Helpers;

import java.util.Map;

public class SessionControllerTest {
  Application app;
  private void startApp(boolean isMultiTenant) {
    app = new GuiceApplicationBuilder()
        .configure((Map) Helpers.inMemoryDatabase())
        .configure(ImmutableMap.of("yb.multiTenant", isMultiTenant))
        .build();
    Helpers.start(app);
  }

  @After
  public void tearDown() {
    Helpers.stop(app);
  }

  @Test
  public void testValidLogin() {
    startApp(false);
    ModelFactory.testCustomer("foo@bar.com");
    ObjectNode loginJson = Json.newObject();
    loginJson.put("email", "Foo@bar.com");
    loginJson.put("password", "password");
    Result result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    JsonNode json = Json.parse(contentAsString(result));

    assertEquals(OK, result.status());
    assertNotNull(json.get("authToken"));
  }

  @Test
  public void testLoginWithInvalidPassword() {
    startApp(false);
    ObjectNode loginJson = Json.newObject();
    loginJson.put("email", "foo@bar.com");
    loginJson.put("password", "password1");
    Result result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    JsonNode json = Json.parse(contentAsString(result));

    assertEquals(UNAUTHORIZED, result.status());
    assertThat(json.get("error").toString(),
               allOf(notNullValue(), containsString("Invalid Customer Credentials")));
  }

  @Test
  public void testLoginWithNullPassword() {
    startApp(false);
    ObjectNode loginJson = Json.newObject();
    loginJson.put("email", "foo@bar.com");
    Result result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    JsonNode json = Json.parse(contentAsString(result));

    assertEquals(BAD_REQUEST, result.status());
    assertThat(json.get("error").toString(),
               allOf(notNullValue(), containsString("{\"password\":[\"This field is required\"]}")));
  }

  @Test
  public void testRegisterCustomer() {
    startApp(true);
    ObjectNode registerJson = Json.newObject();
    registerJson.put("email", "foo2@bar.com");
    registerJson.put("password", "password");
    registerJson.put("name", "Foo");

    Result result = route(fakeRequest("POST", "/api/register").bodyJson(registerJson));
    JsonNode json = Json.parse(contentAsString(result));

    assertEquals(OK, result.status());
    assertNotNull(json.get("authToken"));

    ObjectNode loginJson = Json.newObject();
    loginJson.put("email", "foo2@bar.com");
    loginJson.put("password", "password");
    result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    json = Json.parse(contentAsString(result));

    assertEquals(OK, result.status());
    assertNotNull(json.get("authToken"));
  }

  @Test
  public void testRegisterCustomerExceedingLimit() {
    startApp(false);
    ModelFactory.testCustomer("foo@bar.com");
    ObjectNode registerJson = Json.newObject();
    registerJson.put("email", "foo2@bar.com");
    registerJson.put("password", "password");
    registerJson.put("name", "Foo");
    Result result = route(fakeRequest("POST", "/api/register").bodyJson(registerJson));
    assertBadRequest(result, "Cannot register multiple accounts in Single tenancy.");
  }

  @Test
  public void testRegisterCustomerWithoutEmail() {
    startApp(false);
    ObjectNode registerJson = Json.newObject();
    registerJson.put("email", "foo@bar.com");
    Result result = route(fakeRequest("POST", "/api/login").bodyJson(registerJson));

    JsonNode json = Json.parse(contentAsString(result));

    assertEquals(BAD_REQUEST, result.status());
    assertThat(json.get("error").toString(),
               allOf(notNullValue(),
                     containsString("{\"password\":[\"This field is required\"]}")));
  }

  @Test
  public void testLogout() {
    startApp(false);
    ModelFactory.testCustomer("foo@bar.com");
    ObjectNode loginJson = Json.newObject();
    loginJson.put("email", "Foo@bar.com");
    loginJson.put("password", "password");
    Result result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    JsonNode json = Json.parse(contentAsString(result));

    assertEquals(OK, result.status());
    String authToken = json.get("authToken").asText();
    result = route(fakeRequest("GET", "/api/logout").header("X-AUTH-TOKEN", authToken));
    assertEquals(OK, result.status());
  }

  @Test
  public void testAuthTokenExpiry() {
    startApp(false);
    ModelFactory.testCustomer("foo@bar.com");
    ObjectNode loginJson = Json.newObject();
    loginJson.put("email", "Foo@bar.com");
    loginJson.put("password", "password");
    Result result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    JsonNode json = Json.parse(contentAsString(result));
    String authToken1 = json.get("authToken").asText();
    loginJson.put("email", "Foo@bar.com");
    loginJson.put("password", "password");
    result = route(fakeRequest("POST", "/api/login").bodyJson(loginJson));
    json = Json.parse(contentAsString(result));
    String authToken2 = json.get("authToken").asText();
    assertEquals(authToken1, authToken2);
  }
}
