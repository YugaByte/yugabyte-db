// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.models;

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.common.ModelFactory;
import org.junit.Before;
import org.junit.Test;
import play.libs.Json;

import com.yugabyte.yw.common.FakeDBApplication;

import javax.persistence.PersistenceException;

import static com.yugabyte.yw.models.Users.Role;
import static org.junit.Assert.*;

public class UsersTest extends FakeDBApplication {

  private Customer customer;

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer("tc1", "Test Customer 1");
  }

  @Test
  public void testCreate() {
    Users user = Users.create("tc1@test.com","password", Role.Admin, customer.uuid);
    assertNotNull(user.uuid);
    assertEquals("tc1@test.com", user.email);
    assertNotNull(user.creationDate);
  }

  @Test
  public void testGet() {
    Users user = Users.create("tc1@test.com","password", Role.Admin, customer.uuid);
    Users getUser = Users.get(user.uuid);
    assertEquals("tc1@test.com", user.email);
    assertNotNull(user.creationDate);
    assertEquals(user, getUser);
  }

   @Test(expected = PersistenceException.class)
   public void testCreateWithDuplicateEmail() {
     Users u1 = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
     Users u2 = Users.create("foo@foo.com", "password", Role.ReadOnly, customer.uuid);
   }


  @Test
  public void authenticateWithEmailAndValidPassword() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    Users authUser = Users.authWithPassword("foo@foo.com", "password");
    assertEquals(authUser.uuid, u.uuid);
  }

  @Test
  public void authenticateWithEmailAndInvalidPassword() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    Users authUser = Users.authWithPassword("foo@foo.com", "password1");
    assertNull(authUser);
  }

  @Test
  public void testCreateAuthToken() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    assertNotNull(u.uuid);

    String authToken = u.createAuthToken();
    assertNotNull(authToken);
    assertNotNull(u.getAuthTokenIssueDate());

    Users authUser = Users.authWithToken(authToken);
    assertEquals(authUser.uuid, u.uuid);
  }
  @Test
  public void testAuthTokenExpiry() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    assertNotNull(u.uuid);
    String authTokenOld = u.createAuthToken();
    assertNotNull(authTokenOld);
    assertNotNull(u.getAuthTokenIssueDate());
    Users u1 = Users.get(u.uuid);
    String authTokenNew = u1.createAuthToken();
    assertEquals(authTokenNew, authTokenOld);
  }

  @Test
  public void testDeleteAuthToken() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    assertNotNull(u.uuid);

    String authToken = u.createAuthToken();
    assertNotNull(authToken);
    assertNotNull(u.getAuthTokenIssueDate());

    Users fetchUser = Users.find.query().where().eq("uuid", u.uuid).findOne();
    fetchUser.deleteAuthToken();

    fetchUser = Users.find.query().where().eq("uuid", u.uuid).findOne();
    assertNull(fetchUser.getAuthTokenIssueDate());

    Users authUser = Users.authWithToken(authToken);
    assertNull(authUser);
  }

  @Test
  public void testUpsertApiToken() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    assertNotNull(u.uuid);

    String apiToken = u.upsertApiToken();
    assertNotNull(apiToken);

    Users apiUser = Users.authWithApiToken(apiToken);
    assertEquals(apiUser.uuid, u.uuid);
  }

  @Test
  public void testSetRole() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    assertNotNull(u.uuid);
    u.setRole(Role.ReadOnly);
    u.save();
    Users fetchUser = Users.get(u.uuid);
    assertEquals(fetchUser.getRole(), Role.ReadOnly);
  }

  @Test
  public void testNoSensitiveDataInJson() {
    Users u = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    assertNotNull(u.uuid);

    JsonNode json = Json.toJson(u);
    assertEquals(false, json.has("passwordHash"));
    assertEquals(false, json.has("apiToken"));
  }
}
