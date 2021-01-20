// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Schedule;
import com.yugabyte.yw.models.Users;
import org.junit.Before;
import org.junit.Test;
import org.junit.Ignore;
import play.libs.Json;
import play.mvc.Result;

import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static play.mvc.Http.Status.BAD_REQUEST;
import static play.test.Helpers.contentAsString;

public class CustomerConfigControllerTest extends FakeDBApplication {
  Customer defaultCustomer;
  Users defaultUser;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultUser = ModelFactory.testUser(defaultCustomer);
  }

  @Test
  public void testCreateWithInvalidParams() {
    ObjectNode bodyJson = Json.newObject();
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs";
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST", url,
        defaultUser.createAuthToken(), bodyJson);

    JsonNode node = Json.parse(contentAsString(result));
    assertErrorNodeValue(node, "data", "This field is required");
    assertErrorNodeValue(node, "name", "This field is required");
    assertErrorNodeValue(node, "type", "This field is required");
    assertEquals(BAD_REQUEST, result.status());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateWithInvalidTypeParam() {
    ObjectNode bodyJson = Json.newObject();
    JsonNode data = Json.parse("{\"foo\":\"bar\"}");
    bodyJson.put("name", "test");
    bodyJson.set("data", data);
    bodyJson.put("type", "foo");
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs";
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST", url,
        defaultUser.createAuthToken(), bodyJson);

    JsonNode node = Json.parse(contentAsString(result));
    assertEquals(BAD_REQUEST, result.status());
    assertErrorNodeValue(node, "type", "Invalid type provided");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateWithInvalidDataParam() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("name", "test");
    bodyJson.put("data", "foo");
    bodyJson.put("type", "STORAGE");
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs";
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST", url,
        defaultUser.createAuthToken(), bodyJson);

    JsonNode node = Json.parse(contentAsString(result));
    assertEquals(BAD_REQUEST, result.status());
    assertErrorNodeValue(node, "data", "Invalid data provided, expected a object.");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateWithValidParam() {
    ObjectNode bodyJson = Json.newObject();
    JsonNode data = Json.parse("{\"foo\":\"bar\"}");
    bodyJson.put("name", "test");
    bodyJson.set("data", data);
    bodyJson.put("type", "STORAGE");
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs";
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST", url,
        defaultUser.createAuthToken(), bodyJson);

    JsonNode node = Json.parse(contentAsString(result));
    assertOk(result);
    assertNotNull(node.get("configUUID"));
    assertEquals(1, CustomerConfig.getAll(defaultCustomer.uuid).size());
    assertAuditEntry(1, defaultCustomer.uuid);
  }

  @Test
  public void testListCustomeWithData() {
    ModelFactory.createS3StorageConfig(defaultCustomer);
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs";
    Result result = FakeApiHelper.doRequestWithAuthToken("GET", url,
        defaultUser.createAuthToken());
    JsonNode node = Json.parse(contentAsString(result));
    assertEquals(1, node.size());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testListCustomerWithoutData() {
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs";
    Result result = FakeApiHelper.doRequestWithAuthToken("GET", url,
        defaultUser.createAuthToken());
    JsonNode node = Json.parse(contentAsString(result));
    assertEquals(0, node.size());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testDeleteValidCustomerConfig() {
    UUID configUUID = ModelFactory.createS3StorageConfig(defaultCustomer).configUUID;
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs/" + configUUID;
    Result result = FakeApiHelper.doRequestWithAuthToken("DELETE", url,
        defaultUser.createAuthToken());
    JsonNode node = Json.parse(contentAsString(result));
    assertOk(result);
    assertEquals(0, CustomerConfig.getAll(defaultCustomer.uuid).size());
    assertAuditEntry(1, defaultCustomer.uuid);
  }

  @Test
  public void testDeleteInvalidCustomerConfig() {
    Customer customer = ModelFactory.testCustomer("nc", "New Customer");
    UUID configUUID = ModelFactory.createS3StorageConfig(customer).configUUID;
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs/" + configUUID;
    Result result = FakeApiHelper.doRequestWithAuthToken("DELETE", url,
        defaultUser.createAuthToken());
    assertBadRequest(result, "Invalid configUUID: " + configUUID);
    assertEquals(1, CustomerConfig.getAll(customer.uuid).size());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testDeleteInUseStorageConfig() {
    UUID configUUID = ModelFactory.createS3StorageConfig(defaultCustomer).configUUID;
    Backup backup = ModelFactory.createBackup(defaultCustomer.uuid, UUID.randomUUID(),
                                              configUUID);
    String url = "/api/customers/" + defaultCustomer.uuid + "/configs/" + configUUID;
    Result result = FakeApiHelper.doRequestWithAuthToken("DELETE", url,
        defaultUser.createAuthToken());
    assertInternalServerError(result, "Customer Configuration could not be deleted.");
    backup.delete();
    Schedule schedule = ModelFactory.createScheduleBackup(defaultCustomer.uuid, UUID.randomUUID(),
                                                          configUUID);
    result = FakeApiHelper.doRequestWithAuthToken("DELETE", url,
        defaultUser.createAuthToken());
    assertInternalServerError(result, "Customer Configuration could not be deleted.");
    schedule.delete();
    result = FakeApiHelper.doRequestWithAuthToken("DELETE", url,
        defaultUser.createAuthToken());
    assertOk(result);
    assertEquals(0, CustomerConfig.getAll(defaultCustomer.uuid).size());
    assertAuditEntry(1, defaultCustomer.uuid);
  }
}
