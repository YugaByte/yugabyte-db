// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import akka.stream.javadsl.FileIO;
import akka.stream.javadsl.Source;
import akka.util.ByteString;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.TemplateManager;
import com.yugabyte.yw.common.TestHelper;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Users;
import org.apache.commons.io.FileUtils;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.mockito.ArgumentCaptor;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertErrorNodeValue;
import static com.yugabyte.yw.common.AssertHelper.assertErrorResponse;
import static com.yugabyte.yw.common.AssertHelper.assertOk;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.AssertHelper.assertAuditEntry;
import static com.yugabyte.yw.common.TestHelper.createTempFile;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.containsString;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThat;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.eq;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static play.inject.Bindings.bind;
import static play.mvc.Http.Status.BAD_REQUEST;
import static play.mvc.Http.Status.OK;
import static play.test.Helpers.contentAsString;

public class AccessKeyControllerTest extends FakeDBApplication {
  Provider defaultProvider;
  Customer defaultCustomer;
  Users defaultUser;
  Region defaultRegion;

  final static Integer SSH_PORT = 12345;

  @Before
  public void before() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultUser = ModelFactory.testUser(defaultCustomer);
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
    defaultRegion = Region.create(defaultProvider, "us-west-2", "us-west-2", "yb-image");
    new File(TestHelper.TMP_PATH).mkdirs();
  }

  @After
  public void tearDown() throws IOException {
    FileUtils.deleteDirectory(new File(TestHelper.TMP_PATH));
  }

  private Result getAccessKey(UUID providerUUID, String keyCode) {
    String uri = "/api/customers/" + defaultCustomer.uuid +
        "/providers/" + providerUUID + "/access_keys/" + keyCode;
    return FakeApiHelper.doRequestWithAuthToken("GET", uri,
        defaultUser.createAuthToken());
  }

  private Result listAccessKey(UUID providerUUID) {
    String uri = "/api/customers/" + defaultCustomer.uuid +
        "/providers/" + providerUUID + "/access_keys";
    return FakeApiHelper.doRequestWithAuthToken("GET", uri, defaultUser.createAuthToken());
  }

  private Result createAccessKey(UUID providerUUID, String keyCode, boolean uploadFile,
                                 boolean useRawString) {
    return createAccessKey(providerUUID, keyCode, uploadFile, useRawString, defaultRegion, true, true);
  }

  private Result createAccessKey(UUID providerUUID, String keyCode, boolean uploadFile, boolean useRawString,
                                 Region region, boolean airGapInstall, boolean passwordlessSudoAccess) {
    String uri = "/api/customers/" + defaultCustomer.uuid + "/providers/" + providerUUID + "/access_keys";

    if (uploadFile) {
      List<Http.MultipartFormData.Part<Source<ByteString, ?>>> bodyData = new ArrayList<>();
      if (keyCode != null) {
        bodyData.add(new Http.MultipartFormData.DataPart("keyCode", keyCode));
        bodyData.add(new Http.MultipartFormData.DataPart("regionUUID", region.uuid.toString()));
        bodyData.add(new Http.MultipartFormData.DataPart("keyType", "PRIVATE"));
        bodyData.add(new Http.MultipartFormData.DataPart("sshUser", "ssh-user"));
        bodyData.add(new Http.MultipartFormData.DataPart("sshPort", SSH_PORT.toString()));
        bodyData.add(new Http.MultipartFormData.DataPart("airGapInstall", "false"));
        String tmpFile = createTempFile("PRIVATE KEY DATA");
        Source<ByteString, ?> keyFile = FileIO.fromFile(new File(tmpFile));
        bodyData.add(new Http.MultipartFormData.FilePart("keyFile", "test.pem",
            "application/octet-stream", keyFile));
      }

      return FakeApiHelper.doRequestWithAuthTokenAndMultipartData("POST", uri,
          defaultUser.createAuthToken(),
          bodyData,
          mat);
    } else {
      ObjectNode bodyJson = Json.newObject();
      if (keyCode != null) {
        bodyJson.put("keyCode", keyCode);
        bodyJson.put("regionUUID", region.uuid.toString());
      }
      if(useRawString) {
        bodyJson.put("keyType", AccessManager.KeyType.PRIVATE.toString());
        bodyJson.put("keyContent", "PRIVATE KEY DATA");
      }
      bodyJson.put("airGapInstall", airGapInstall);
      bodyJson.put("sshPort", SSH_PORT);
      bodyJson.put("passwordlessSudoAccess", passwordlessSudoAccess);
      return FakeApiHelper.doRequestWithAuthTokenAndBody("POST", uri,
          defaultUser.createAuthToken(), bodyJson);
    }
  }

  private Result deleteAccessKey(UUID providerUUID, String keyCode) {
    String uri = "/api/customers/" + defaultCustomer.uuid + "/providers/" +
                providerUUID + "/access_keys/" + keyCode;
    return FakeApiHelper.doRequestWithAuthToken("DELETE", uri, defaultUser.createAuthToken());
  }

  @Test
  public void testGetAccessKeyWithInvalidProviderUUID() {
    UUID invalidProviderUUID = UUID.randomUUID();
    Result result = getAccessKey(invalidProviderUUID, "foo");
    assertBadRequest(result, "Invalid Provider UUID: " + invalidProviderUUID);
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testGetAccessKeyWithInvalidKeyCode() {
    AccessKey accessKey = AccessKey.create(UUID.randomUUID(), "foo", new AccessKey.KeyInfo());
    Result result = getAccessKey(defaultProvider.uuid, accessKey.getKeyCode());
    assertEquals(BAD_REQUEST, result.status());
    assertBadRequest(result, "KeyCode not found: " + accessKey.getKeyCode());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testGetAccessKeyWithValidKeyCode() {
    AccessKey accessKey = AccessKey.create(defaultProvider.uuid, "foo", new AccessKey.KeyInfo());
    Result result = getAccessKey(defaultProvider.uuid, accessKey.getKeyCode());
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    JsonNode idKey = json.get("idKey");
    assertValue(idKey, "keyCode", accessKey.getKeyCode());
    assertValue(idKey, "providerUUID", accessKey.getProviderUUID().toString());
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testListAccessKeyWithInvalidProviderUUID() {
    UUID invalidProviderUUID = UUID.randomUUID();
    Result result = listAccessKey(invalidProviderUUID);
    assertBadRequest(result, "Invalid Provider UUID: " + invalidProviderUUID);
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testListAccessKeyWithEmptyData() {
    Result result = listAccessKey(defaultProvider.uuid);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertEquals(json.size(), 0);
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testListAccessKeyWithValidData() {
    AccessKey.create(defaultProvider.uuid, "key-1", new AccessKey.KeyInfo());
    AccessKey.create(defaultProvider.uuid, "key-2", new AccessKey.KeyInfo());
    Result result = listAccessKey(defaultProvider.uuid);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertEquals(json.size(), 2);
    json.forEach((key) -> {
      assertThat(key.get("idKey").get("keyCode").asText(), allOf(notNullValue(), containsString("key-")));
      assertThat(key.get("idKey").get("providerUUID").asText(), allOf(notNullValue(), equalTo(defaultProvider.uuid.toString())));
    });
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateAccessKeyWithInvalidProviderUUID() {
    Result result = createAccessKey(UUID.randomUUID(), "foo", false, false);
    assertBadRequest(result, "Invalid Provider/Region UUID");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateAccessKeyWithInvalidParams() {
    Result result = createAccessKey(defaultProvider.uuid, null, false, false);
    JsonNode node = Json.parse(contentAsString(result));
    assertErrorNodeValue(node, "keyCode", "This field is required");
    assertErrorNodeValue(node, "regionUUID", "This field is required");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateAccessKeyWithDifferentProviderUUID() {
    Provider gcpProvider = ModelFactory.gcpProvider(ModelFactory.testCustomer("fb", "foo@bar.com"));
    Result result = createAccessKey(gcpProvider.uuid, "key-code", false, false);
    assertBadRequest(result, "Invalid Provider/Region UUID");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateAccessKeyInDifferentRegion() {
    AccessKey accessKey = AccessKey.create(defaultProvider.uuid, "key-code", new AccessKey.KeyInfo());
    when(mockAccessManager.addKey(defaultRegion.uuid, "key-code", SSH_PORT, true))
        .thenReturn(accessKey);
    Result result = createAccessKey(defaultProvider.uuid, "key-code", false, false);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertAuditEntry(1, defaultCustomer.uuid);
    assertValue(json.get("idKey"), "keyCode", "key-code");
    assertValue(json.get("idKey"), "providerUUID", defaultProvider.uuid.toString());
  }

  @Test
  public void testCreateAccessKeyWithoutKeyFile() {
    AccessKey accessKey = AccessKey.create(defaultProvider.uuid, "key-code-1", new AccessKey.KeyInfo());
    when(mockAccessManager.addKey(defaultRegion.uuid, "key-code-1", SSH_PORT, true))
        .thenReturn(accessKey);
    Result result = createAccessKey(defaultProvider.uuid, "key-code-1", false, false);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertAuditEntry(1, defaultCustomer.uuid);
    assertValue(json.get("idKey"), "keyCode", "key-code-1");
    assertValue(json.get("idKey"), "providerUUID", defaultProvider.uuid.toString());
  }

  @Test
  public void testCreateAccessKeyWithKeyFile() {
    AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
    keyInfo.publicKey = "/path/to/public.key";
    keyInfo.privateKey = "/path/to/private.key";
    AccessKey accessKey = AccessKey.create(defaultProvider.uuid, "key-code-1", keyInfo);
    ArgumentCaptor<File> updatedFile = ArgumentCaptor.forClass(File.class);
    when(mockAccessManager.uploadKeyFile(eq(defaultRegion.uuid), any(File.class),
        eq("key-code-1"), eq(AccessManager.KeyType.PRIVATE), eq("ssh-user"), eq(SSH_PORT),
        eq(false))).thenReturn(accessKey);
    Result result = createAccessKey(defaultProvider.uuid, "key-code-1", true, false);
    verify(mockAccessManager, times(1)).uploadKeyFile(eq(defaultRegion.uuid),
        updatedFile.capture(), eq("key-code-1"), eq(AccessManager.KeyType.PRIVATE), eq("ssh-user"),
        eq(SSH_PORT), eq(false));
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertAuditEntry(1, defaultCustomer.uuid);
    assertNotNull(json);
    try {
      List<String> lines = Files.readAllLines(updatedFile.getValue().toPath());
      assertEquals(1, lines.size());
      assertThat(lines.get(0), allOf(notNullValue(), equalTo("PRIVATE KEY DATA")));
    } catch (IOException e) {
      assertNull(e.getMessage());
    }
  }

  @Test
  public void testCreateAccessKeyWithKeyString() {
    AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
    keyInfo.publicKey = "/path/to/public.key";
    keyInfo.privateKey = "/path/to/private.key";
    AccessKey accessKey = AccessKey.create(defaultProvider.uuid, "key-code-1", keyInfo);
    ArgumentCaptor<File> updatedFile = ArgumentCaptor.forClass(File.class);
    when(mockAccessManager.uploadKeyFile(eq(defaultRegion.uuid), any(File.class),
        eq("key-code-1"), eq(AccessManager.KeyType.PRIVATE), eq(null), eq(SSH_PORT),
        eq(true))).thenReturn(accessKey);
    Result result = createAccessKey(defaultProvider.uuid, "key-code-1", false, true);
    verify(mockAccessManager, times(1)).uploadKeyFile(eq(defaultRegion.uuid),
        updatedFile.capture(), eq("key-code-1"), eq(AccessManager.KeyType.PRIVATE), eq(null),
        eq(SSH_PORT), eq(true));
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertAuditEntry(1, defaultCustomer.uuid);
    assertNotNull(json);
    try {
      List<String> lines = Files.readAllLines(updatedFile.getValue().toPath());
      assertEquals(1, lines.size());
      assertThat(lines.get(0), allOf(notNullValue(), equalTo("PRIVATE KEY DATA")));
    } catch (IOException e) {
      assertNull(e.getMessage());
    }
  }

  @Test
  public void testCreateAccessKeyWithException() {
    when(mockAccessManager.addKey(defaultRegion.uuid, "key-code-1", SSH_PORT, true))
        .thenThrow(new RuntimeException("Something went wrong!!"));
    Result result = createAccessKey(defaultProvider.uuid, "key-code-1", false, false);
    assertErrorResponse(result, "Unable to create access key: key-code-1");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testCreateAccessKeyWithoutPasswordlessSudoAccessSuccess() {
    Provider onpremProvider = ModelFactory.newProvider(defaultCustomer, Common.CloudType.onprem);
    Region onpremRegion = Region.create(onpremProvider, "onprem-a", "onprem-a", "yb-image");
    AccessKey accessKey = AccessKey.create(onpremProvider.uuid, "key-code-1", new AccessKey.KeyInfo());
    when(mockAccessManager.addKey(onpremRegion.uuid, "key-code-1", SSH_PORT, true))
        .thenReturn(accessKey);
    Result result = createAccessKey(onpremProvider.uuid, "key-code-1", false, false, onpremRegion, true, false);
    assertOk(result);
    assertAuditEntry(1, defaultCustomer.uuid);
    verify(mockTemplateManager, times(1)).createProvisionTemplate(accessKey, true, false);
  }

  @Test
  public void testCreateAccessKeyWithoutPasswordlessSudoAccessError() {
    Provider onpremProvider = ModelFactory.newProvider(defaultCustomer, Common.CloudType.onprem);
    Region onpremRegion = Region.create(onpremProvider, "onprem-a", "onprem-a", "yb-image");
    AccessKey accessKey = AccessKey.create(onpremProvider.uuid, "key-code-1", new AccessKey.KeyInfo());
    when(mockAccessManager.addKey(onpremRegion.uuid, "key-code-1", SSH_PORT, false))
        .thenReturn(accessKey);
    doThrow(new RuntimeException("foobar")).when(mockTemplateManager).createProvisionTemplate(accessKey, false, false);
    Result result = createAccessKey(onpremProvider.uuid, "key-code-1", false, false, onpremRegion, false, false);
    assertErrorResponse(result, "Unable to create access key: key-code-1");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testDeleteAccessKeyWithInvalidProviderUUID() {
    UUID invalidProviderUUID = UUID.randomUUID();
    Result result = deleteAccessKey(invalidProviderUUID, "foo");
    assertBadRequest(result, "Invalid Provider UUID: " + invalidProviderUUID);
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testDeleteAccessKeyWithInvalidAccessKeyCode() {
    Result result = deleteAccessKey(defaultProvider.uuid, "foo");
    assertBadRequest(result, "KeyCode not found: foo");
    assertAuditEntry(0, defaultCustomer.uuid);
  }

  @Test
  public void testDeleteAccessKeyWithValidAccessKeyCode() {
    AccessKey.create(defaultProvider.uuid, "key-code-1", new AccessKey.KeyInfo());
    Result result = deleteAccessKey(defaultProvider.uuid, "key-code-1");
    assertEquals(OK, result.status());
    assertAuditEntry(1, defaultCustomer.uuid);
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("Deleted KeyCode: key-code-1")));
  }
}
