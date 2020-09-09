// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.CertificateHelper;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.models.CertificateInfo;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Users;
import org.apache.commons.io.FileUtils;
import org.mockito.Mock;
import org.mockito.runners.MockitoJUnitRunner;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Date;
import java.util.UUID;
import java.util.LinkedHashMap;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertNotNull;
import static play.mvc.Http.Status.OK;
import static play.test.Helpers.contentAsString;
import static org.mockito.Mockito.when;
import static com.yugabyte.yw.common.AssertHelper.*;

@RunWith(MockitoJUnitRunner.class)
public class CertificateControllerTest extends FakeDBApplication {
  @Mock
  play.Configuration mockAppConfig;

  private Customer customer;
  private Users user;
  private List<String> test_certs = Arrays.asList("test_cert1", "test_cert2", "test_cert3");
  private List<UUID> test_certs_uuids = new ArrayList<>();

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer();
    user = ModelFactory.testUser(customer);
    for (String cert: test_certs) {
      test_certs_uuids.add(CertificateHelper.createRootCA(cert, customer.uuid, "/tmp/certs"));
    }
  }

  @After
  public void tearDown() throws IOException {
    FileUtils.deleteDirectory(new File("/tmp/certs"));
  }

  private Result listCertificates(UUID customerUUID) {
    String uri = "/api/customers/" + customerUUID + "/certificates";
    return FakeApiHelper.doRequestWithAuthToken("GET", uri, user.createAuthToken());
  }

  private Result uploadCertificate(UUID customerUUID, ObjectNode bodyJson) {
    String uri = "/api/customers/" + customerUUID + "/certificates";
    return FakeApiHelper.doRequestWithAuthTokenAndBody("POST", uri,
        user.createAuthToken(), bodyJson);
  }

  private Result getCertificate(UUID customerUUID, String label) {
    String uri = "/api/customers/" + customerUUID + "/certificates/" + label;
    return FakeApiHelper.doRequestWithAuthToken("GET", uri, user.createAuthToken());
  }

  private Result createClientCertificate(UUID customerUUID, UUID rootUUID,
                                         ObjectNode bodyJson) {
    String uri = "/api/customers/" + customerUUID + "/certificates/" + rootUUID;
    return FakeApiHelper.doRequestWithAuthTokenAndBody("POST", uri, user.createAuthToken(),
                                                       bodyJson);
  }

  private Result getRootCertificate(UUID customerUUID, UUID rootUUID) {
    String uri = "/api/customers/" + customerUUID + "/certificates/" + rootUUID + "/download";
    return FakeApiHelper.doRequestWithAuthToken("GET", uri, user.createAuthToken());
  }

  @Test
  public void testListCertificates() {
    Result result = listCertificates(customer.uuid);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(OK, result.status());
    List<LinkedHashMap> certs = Json.fromJson(json, List.class);
    List<UUID> result_uuids = new ArrayList<>();
    List<String> result_labels = new ArrayList<>();
    for (LinkedHashMap e : certs) {
      result_uuids.add(UUID.fromString(e.get("uuid").toString()));
      result_labels.add(e.get("label").toString());
    }
    assertEquals(test_certs, result_labels);
    assertEquals(test_certs_uuids, result_uuids);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testGetCertificate() {
    UUID cert_uuid = test_certs_uuids.get(0);
    Result result = getCertificate(customer.uuid, test_certs.get(0));
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(OK, result.status());
    assertEquals(cert_uuid, UUID.fromString(json.asText()));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUploadCertificate() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("label", "test");
    bodyJson.put("certContent", "cert_test");
    bodyJson.put("keyContent", "key_test");
    Date date = new Date();
    bodyJson.put("certStart", date.getTime());
    bodyJson.put("certExpiry", date.getTime());
    Result result = uploadCertificate(customer.uuid, bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(OK, result.status());
    UUID certUUID = UUID.fromString(json.asText());
    CertificateInfo ci = CertificateInfo.get(certUUID);
    assertEquals(ci.label, "test");
    assertTrue(ci.certificate.contains("/tmp"));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUploadCertificateFail() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("label", "test");
    bodyJson.put("certContent", "cert_test");
    Date date = new Date();
    bodyJson.put("certStart", date.getTime());
    bodyJson.put("certExpiry", date.getTime());
    Result result = uploadCertificate(customer.uuid, bodyJson);
    assertBadRequest(result, "{\"keyContent\":[\"This field is required\"]}");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateClientCertificate() {
    ObjectNode bodyJson = Json.newObject();
    Date date = new Date();
    bodyJson.put("username", "test");
    bodyJson.put("certStart", date.getTime());
    bodyJson.put("certExpiry", date.getTime());
    UUID rootCA = CertificateHelper.createRootCA("test-universe", customer.uuid,
                                                 "/tmp");
    Result result = createClientCertificate(customer.uuid, rootCA, bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(OK, result.status());
    String clientCert = json.get("yugabytedb.crt").asText();
    String clientKey = json.get("yugabytedb.key").asText();
    assertNotNull(clientCert);
    assertNotNull(clientKey);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testGetRootCertificate() {
    UUID rootCA = CertificateHelper.createRootCA("test-universe", customer.uuid,
                                                 "/tmp");
    Result result = getRootCertificate(customer.uuid, rootCA);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(OK, result.status());
    String rootCert = json.get("root.crt").asText();
    assertNotNull(rootCert);
    assertAuditEntry(1, customer.uuid);
  }

}
