// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.models;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import org.junit.Before;
import org.junit.Test;
import play.libs.Json;
import play.mvc.Http;

import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static com.yugabyte.yw.models.Users.Role;
import static org.junit.Assert.*;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;
import static play.test.Helpers.contextComponents;

public class AuditTest extends FakeDBApplication {

  Users user;
  Customer customer;
  Http.Request request;
  Http.Context context;

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer("tc1", "Test Customer 1");
    user = ModelFactory.testUser(customer);
    Map<String, String> flashData = Collections.emptyMap();
    Map<String, Object> argData = ImmutableMap.of("user", user);
    request = mock(Http.Request.class);
    Long id = 2L;
    play.api.mvc.RequestHeader header = mock(play.api.mvc.RequestHeader.class);
    context = new Http.Context(
      id, header, request, flashData, flashData, argData, contextComponents()
    );
    Http.Context.current.set(context);
    when(request.method()).thenReturn("PUT");
    when(request.path()).thenReturn("/api/customer/test/universe/test");
  }

  public Audit createEntry(UUID taskUUID, Users user) {
    return Audit.create(user.uuid, user.customerUUID,
        "/test/api/call", "PUT", null, taskUUID);
  }

  @Test
  public void testCreate() {
    for (long i = 0; i < 2; i++) {
      UUID randUUID = UUID.randomUUID();
      Audit entry = createEntry(randUUID, user);
      assertSame(i+1, entry.getAuditID());
      assertEquals("/test/api/call", entry.getApiCall());
      assertEquals("PUT", entry.getApiMethod());
      assertEquals(randUUID, entry.getTaskUUID());
      assertNotNull(entry.getTimestamp());
    }
  }

  @Test
  public void testCreateAuditEntry() {
    Audit.createAuditEntry(context, request);
    List<Audit> entries = Audit.getAll(customer.uuid);
    assertEquals(entries.size(), 1);
    assertEquals(entries.get(0).getUserUUID(), user.uuid);
    assertEquals(entries.get(0).getApiCall(), "/api/customer/test/universe/test");
    assertEquals(entries.get(0).getApiMethod(), "PUT");
    assertNull(entries.get(0).getTaskUUID());
    assertNull(entries.get(0).getPayload());
    assertNotNull(entries.get(0).getTimestamp());
  }

  @Test
  public void testCreateAuditEntryWithTaskUUID() {
    UUID randUUID = UUID.randomUUID();
    Audit.createAuditEntry(context, request, randUUID);
    List<Audit> entries = Audit.getAll(customer.uuid);
    assertEquals(entries.size(), 1);
    assertEquals(entries.get(0).getUserUUID(), user.uuid);
    assertEquals(entries.get(0).getApiCall(), "/api/customer/test/universe/test");
    assertEquals(entries.get(0).getApiMethod(), "PUT");
    assertEquals(entries.get(0).getTaskUUID(), randUUID);
    assertNull(entries.get(0).getPayload());
    assertNotNull(entries.get(0).getTimestamp());
  }

  @Test
  public void testCreateAuditEntryWithPayload() {
    ObjectNode testPayload = Json.newObject()
            .put("foo", "bar")
            .put("abc", "xyz");
    Audit.createAuditEntry(context, request, testPayload);
    List<Audit> entries = Audit.getAll(customer.uuid);
    assertEquals(entries.size(), 1);
    assertEquals(entries.get(0).getUserUUID(), user.uuid);
    assertEquals(entries.get(0).getApiCall(), "/api/customer/test/universe/test");
    assertEquals(entries.get(0).getApiMethod(), "PUT");
    assertNull(entries.get(0).getTaskUUID());
    assertEquals(entries.get(0).getPayload(), testPayload);
    assertNotNull(entries.get(0).getTimestamp());
  }

  @Test
  public void testCreateAuditEntryWithPayloadAndTaskUUID() {
    UUID randUUID = UUID.randomUUID();
    ObjectNode testPayload = Json.newObject()
            .put("foo", "bar")
            .put("abc", "xyz");
    Audit.createAuditEntry(context, request, testPayload, randUUID);
    List<Audit> entries = Audit.getAll(customer.uuid);
    assertEquals(entries.size(), 1);
    assertEquals(entries.get(0).getUserUUID(), user.uuid);
    assertEquals(entries.get(0).getApiCall(), "/api/customer/test/universe/test");
    assertEquals(entries.get(0).getApiMethod(), "PUT");
    assertEquals(entries.get(0).getTaskUUID(), randUUID);
    assertEquals(entries.get(0).getPayload(), testPayload);
    assertNotNull(entries.get(0).getTimestamp());
  }

  @Test
  public void testGetAll() {
    UUID randUUID = UUID.randomUUID();
    UUID randUUID1 = UUID.randomUUID();
    createEntry(randUUID, user);
    createEntry(randUUID1, user);
    List<Audit> entries = Audit.getAll(customer.uuid);
    assertEquals(entries.size(), 2);
  }

  @Test
  public void testGetFromTaskUUID() {
    UUID randUUID = UUID.randomUUID();
    UUID randUUID1 = UUID.randomUUID();
    createEntry(randUUID, user);
    createEntry(randUUID1, user);
    Audit entry = Audit.getFromTaskUUID(randUUID1);
    assertEquals(entry.getTaskUUID(), randUUID1);
  }

  @Test
  public void testGetAllUserEntries() {
    Users u1 = Users.create("foo@foo.com", "password", Role.Admin, customer.uuid);
    UUID randUUID = UUID.randomUUID();
    UUID randUUID1 = UUID.randomUUID();
    UUID randUUID2 = UUID.randomUUID();
    createEntry(randUUID, user);
    createEntry(randUUID1, u1);
    createEntry(randUUID2, u1);
    List<Audit> entries = Audit.getAllUserEntries(u1.uuid);
    List<Audit> entries1 = Audit.getAllUserEntries(user.uuid);
    assertEquals(entries.size(), 2);
    assertEquals(entries1.size(), 1);
  }
}
