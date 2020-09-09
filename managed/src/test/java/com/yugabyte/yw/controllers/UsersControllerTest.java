// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.ObjectReader;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.CallHomeManager.CollectionLevel;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Users;

import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import org.junit.Before;
import org.junit.Test;
import org.mindrot.jbcrypt.BCrypt;
import org.mockito.ArgumentCaptor;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.io.*;

import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.AssertHelper.assertAuditEntry;
import static com.yugabyte.yw.common.AssertHelper.assertUnauthorized;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.models.Users.Role;
import static org.hamcrest.CoreMatchers.*;
import static org.hamcrest.core.StringContains.containsString;
import static org.mockito.Matchers.anyList;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Mockito.*;
import static play.inject.Bindings.bind;
import static play.test.Helpers.*;
import static org.junit.Assert.*;
import static play.mvc.Http.Status.OK;
import static play.mvc.Http.Status.FORBIDDEN;
import static play.test.Helpers.fakeRequest;

public class UsersControllerTest extends FakeDBApplication {
  String baseRoute = "/api/customers/%s/users";

  private Customer customer1, customer2;
  private Users user1, user2;
  private String authToken1, authToken2;

  @Before
  public void setUp() {
    customer1 = ModelFactory.testCustomer("tc1", "Test Customer 1");
    customer2 = ModelFactory.testCustomer("tc2", "Test Customer 2");
    user1 = ModelFactory.testUser(customer1, "tc1@test.com");
    user2 = ModelFactory.testUser(customer2, "tc2@test.com");
    authToken1 = user1.createAuthToken();
    authToken2 = user2.createAuthToken();
  }

  public List<Users> getListOfUsers(String authToken, Customer customer) throws IOException {
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken1).build();
    Result result = route(fakeRequest("GET",
        String.format(baseRoute, customer.uuid)).cookie(validCookie));
    if (result.status() == FORBIDDEN) {
      return null;
    }
    JsonNode json = Json.parse(contentAsString(result));
    ObjectMapper mapper = new ObjectMapper();
    ObjectReader reader = mapper.readerFor(new TypeReference<List<Users>>(){});
    List<Users> userList = reader.readValue(json);
    return userList;
  }

  @Test
  public void testGetUsersWithValidToken() throws IOException {
    List<Users> userList = getListOfUsers(authToken1, customer1);
    assertNotNull(userList);
    assertEquals(userList.size(), 1);
    assertThat(userList.get(0).uuid, allOf(notNullValue(), equalTo(user1.uuid)));
    assertEquals(userList.get(0).email, user1.email);
    assertAuditEntry(0, customer1.uuid);
  }

  @Test
  public void testGetUsersWithInvalidToken() throws IOException {
    List<Users> userList = getListOfUsers(authToken1, customer2);
    assertNull(userList);
    assertAuditEntry(0, customer1.uuid);
  }

  @Test
  public void testCreateUserWithValidToken() throws IOException {
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken1).build();
    ObjectNode params = Json.newObject();
    params.put("email", "foo@bar.com");
    params.put("password", "new-password");
    params.put("confirmPassword", "new-password");
    params.put("role", "ReadOnly");
    Result result = route(fakeRequest("POST",
        String.format(baseRoute, customer1.uuid)).cookie(validCookie).bodyJson(params));
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    ObjectMapper mapper = new ObjectMapper();
    ObjectReader reader = mapper.readerFor(new TypeReference<Users>(){});
    Users user = reader.readValue(json);
    assertEquals(user.email, "foo@bar.com");
    List<Users> userList = getListOfUsers(authToken1, customer1);
    assertEquals(userList.size(), 2);
    assertAuditEntry(1, customer1.uuid);
  }

  @Test
  public void testDeleteUserWithValidToken() throws IOException {
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken1).build();
    Result result = route(fakeRequest("DELETE",
        String.format("%s/%s", String.format(baseRoute, customer1.uuid), user1.uuid))
        .cookie(validCookie));
    List<Users> userList = getListOfUsers(authToken1, customer1);
    assertNull(userList);
    assertAuditEntry(1, customer1.uuid);
  }

  @Test
  public void testRoleChange() throws IOException {
    Users testUser1 = ModelFactory.testUser(customer1, "tc3@test.com", Role.Admin);
    assertEquals(testUser1.getRole(), Role.Admin);
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken1).build();
    Result result = route(fakeRequest("PUT",
        String.format("%s/%s?role=ReadOnly",
        String.format(baseRoute, customer1.uuid), testUser1.uuid))
        .cookie(validCookie));
    testUser1 = Users.get(testUser1.uuid);
    assertEquals(testUser1.getRole(), Role.ReadOnly);
    assertAuditEntry(1, customer1.uuid);
  }

  @Test
  public void testPasswordChangeInvalid() throws IOException {
    Users testUser1 = ModelFactory.testUser(customer1, "tc3@test.com", Role.Admin);
    assertEquals(testUser1.getRole(), Role.Admin);
    ObjectNode params = Json.newObject();
    params.put("email", "tc3@test.com");
    params.put("password", "new-password");
    params.put("confirmPassword", "new-password");
    params.put("role", "Admin");
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken1).build();
    Result result = route(fakeRequest("PUT",
        String.format("%s/%s/change_password",
        String.format(baseRoute, customer1.uuid), testUser1.uuid))
        .cookie(validCookie).bodyJson(params));
    testUser1 = Users.get(testUser1.uuid);
    assertEquals(result.status(), FORBIDDEN);
  }

  @Test
  public void testPasswordChangeValid() throws IOException {
    Users testUser1 = ModelFactory.testUser(customer1, "tc3@test.com", Role.Admin);
    String authTokenTest = testUser1.createAuthToken();
    assertEquals(testUser1.getRole(), Role.Admin);
    ObjectNode params = Json.newObject();
    params.put("email", "tc3@test.com");
    params.put("password", "new-password");
    params.put("confirmPassword", "new-password");
    params.put("role", "Admin");
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authTokenTest).build();
    Result result = route(fakeRequest("PUT",
        String.format("%s/%s/change_password",
        String.format(baseRoute, customer1.uuid), testUser1.uuid))
        .cookie(validCookie).bodyJson(params));
    testUser1 = Users.get(testUser1.uuid);
    assertEquals(testUser1.getRole(), Role.Admin);
    assertTrue(BCrypt.checkpw("new-password", testUser1.passwordHash));
    assertAuditEntry(0, customer1.uuid);
  }
}
