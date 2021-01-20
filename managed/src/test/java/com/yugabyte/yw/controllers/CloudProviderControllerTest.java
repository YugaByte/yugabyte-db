// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertInternalServerError;
import static com.yugabyte.yw.common.AssertHelper.assertOk;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.AssertHelper.assertValues;
import static com.yugabyte.yw.common.AssertHelper.assertAuditEntry;
import static com.yugabyte.yw.common.TestHelper.createTempFile;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static junit.framework.TestCase.assertTrue;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThat;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyBoolean;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Matchers.anyString;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static play.test.Helpers.contentAsString;

import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.CloudBootstrap;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.NetworkManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.common.TemplateManager;
import com.yugabyte.yw.common.TestHelper;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;
import org.apache.commons.io.FileUtils;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Users;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Result;
import play.test.Helpers;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.HashMap;
import java.util.UUID;

public class CloudProviderControllerTest extends FakeDBApplication {
  public static final Logger LOG = LoggerFactory.getLogger(CloudProviderControllerTest.class);
  Customer customer;
  Users user;

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer();
    user = ModelFactory.testUser(customer);
    new File(TestHelper.TMP_PATH).mkdirs();
    try{
      String kubeFile = createTempFile("test2.conf", "test5678");
      when(mockAccessManager.createKubernetesConfig(anyString(), anyMap(), anyBoolean())).thenReturn(kubeFile);
    } catch (Exception e) {
      // Do nothing
    }
  }

  @After
  public void tearDown() throws IOException {
    FileUtils.deleteDirectory(new File(TestHelper.TMP_PATH));
  }

  private Result listProviders() {
    return FakeApiHelper.doRequestWithAuthToken("GET",
        "/api/customers/" + customer.uuid  + "/providers", user.createAuthToken());
  }

  private Result createProvider(JsonNode bodyJson) {
    return FakeApiHelper.doRequestWithAuthTokenAndBody("POST",
        "/api/customers/" + customer.uuid + "/providers", user.createAuthToken(), bodyJson);
  }

  private Result createKubernetesProvider(JsonNode bodyJson) {
    return FakeApiHelper.doRequestWithAuthTokenAndBody("POST",
        "/api/customers/" + customer.uuid + "/providers/kubernetes",
        user.createAuthToken(), bodyJson);
  }

  private Result deleteProvider(UUID providerUUID) {
    return FakeApiHelper.doRequestWithAuthToken("DELETE",
        "/api/customers/" + customer.uuid + "/providers/" + providerUUID, user.createAuthToken());
  }

  private Result editProvider(JsonNode bodyJson, UUID providerUUID) {
    return FakeApiHelper.doRequestWithAuthTokenAndBody("PUT",
            "/api/customers/" + customer.uuid + "/providers/"+ providerUUID + "/edit",
            user.createAuthToken(), bodyJson);
  }

  private Result bootstrapProvider(JsonNode bodyJson, Provider provider) {
    return FakeApiHelper.doRequestWithAuthTokenAndBody("POST",
        "/api/customers/" + customer.uuid + "/providers/" + provider.uuid + "/bootstrap",
        user.createAuthToken(),
        bodyJson);
  }

  @Test
  public void testListEmptyProviders() {
    Result result = listProviders();
    JsonNode json = Json.parse(contentAsString(result));

    assertOk(result);
    assertTrue(json.isArray());
    assertEquals(0, json.size());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testListProviders() {
    Provider p1 = ModelFactory.awsProvider(customer);
    p1.setConfig(ImmutableMap.of("MY_KEY_DATA", "SENSITIVE_DATA", "MY_SECRET_DATA", "SENSITIVE_DATA"));
    p1.save();
    Provider p2 = ModelFactory.gcpProvider(customer);
    p2.setConfig(ImmutableMap.of("FOO", "BAR"));
    p2.save();
    Result result = listProviders();
    JsonNode json = Json.parse(contentAsString(result));

    assertOk(result);
    assertAuditEntry(0, customer.uuid);
    assertEquals(2, json.size());
    assertValues(json, "uuid", (List) ImmutableList.of(p1.uuid.toString(), p2.uuid.toString()));
    assertValues(json, "name", (List) ImmutableList.of(p1.name, p2.name));
    json.forEach((providerJson) -> {
      JsonNode config = providerJson.get("config");
      if (UUID.fromString(providerJson.get("uuid").asText()).equals(p1.uuid)) {
        assertValue(config, "MY_KEY_DATA", "SE**********TA");
        assertValue(config, "MY_SECRET_DATA", "SE**********TA");
      } else {
        assertValue(config, "FOO", "BAR");
      }
    });
  }

  @Test
  public void testListProvidersWithValidCustomer() {
    Provider.create(UUID.randomUUID(), Common.CloudType.aws, "Amazon");
    Provider p = ModelFactory.gcpProvider(customer);
    Result result = listProviders();
    JsonNode json = Json.parse(contentAsString(result));

    assertOk(result);
    assertEquals(1, json.size());
    assertValues(json, "uuid", (List) ImmutableList.of(p.uuid.toString()));
    assertValues(json, "name", (List) ImmutableList.of(p.name.toString()));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateProvider() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "azu");
    bodyJson.put("name", "Microsoft");
    Result result = createProvider(bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertValue(json, "name", "Microsoft");
    assertValue(json, "customerUUID", customer.uuid.toString());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateDuplicateProvider() {
    ModelFactory.awsProvider(customer);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "aws");
    bodyJson.put("name", "Amazon");
    Result result = createProvider(bodyJson);
    assertBadRequest(result, "Duplicate provider code: aws");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateProviderWithDifferentCustomer() {
    Provider.create(UUID.randomUUID(), Common.CloudType.aws, "Amazon");
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "aws");
    bodyJson.put("name", "Amazon");
    Result result = createProvider(bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertValue(json, "name", "Amazon");
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateWithInvalidParams() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "aws");
    Result result = createProvider(bodyJson);
    assertBadRequest(result, "\"name\":[\"This field is required\"]}");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateProviderWithConfig() {
    List<String> providerCodes = ImmutableList.of("aws", "gcp");
    for (String code : providerCodes) {
      String providerName = code + "-Provider";
      ObjectNode bodyJson = Json.newObject();
      bodyJson.put("code", code);
      bodyJson.put("name", providerName);
      ObjectNode configJson = Json.newObject();
      ObjectNode configFileJson = Json.newObject();
      if (code.equals("gcp")) {
        // Technically this is not the input format of the file, but we're using this to match the
        // number of elements...
        configFileJson.put("GCE_EMAIL", "email");
        configFileJson.put("GCE_PROJECT", "project");
        configFileJson.put("GOOGLE_APPLICATION_CREDENTIALS", "credentials");
        configJson.put("config_file_contents", configFileJson);
      } else if (code.equals("aws")) {
        configJson.put("foo", "bar");
        configJson.put("foo2", "bar2");
      }
      bodyJson.set("config", configJson);
      Result result = createProvider(bodyJson);
      JsonNode json = Json.parse(contentAsString(result));
      assertOk(result);
      assertValue(json, "name", providerName);
      Provider provider = Provider.get(customer.uuid, UUID.fromString(json.path("uuid").asText()));
      Map<String, String> config = provider.getConfig();
      assertFalse(config.isEmpty());
      // We should technically check the actual content, but the keys are different between the
      // input payload and the saved config.
      if (code.equals("gcp")) {
        assertEquals(configFileJson.size(), config.size());
      } else {
        assertEquals(configJson.size(), config.size());
      }
    }
    assertAuditEntry(2, customer.uuid);
  }

  @Test
  public void testCreateProviderWithHostVpcGcp() {
    String providerName = "gcp-Provider";
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "gcp");
    bodyJson.put("name", providerName);
    ObjectNode configJson = Json.newObject();
    configJson.put("use_host_vpc", true);
    configJson.put("project_id", "project");
    bodyJson.set("config", configJson);
    Result result = createProvider(bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertValue(json, "name", providerName);
    Provider provider = Provider.get(customer.uuid, UUID.fromString(json.path("uuid").asText()));
    Map<String, String> config = provider.getConfig();
    assertTrue(config.isEmpty());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateProviderWithHostCredentialsGcp() {
    String providerName = "gcp-Provider";
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "gcp");
    bodyJson.put("name", providerName);
    ObjectNode configJson = Json.newObject();
    ObjectNode configFileJson = Json.newObject();
    configFileJson.put("client_email", "email");
    configFileJson.put("project_id", "project");
    configFileJson.put("GOOGLE_APPLICATION_CREDENTIALS", "credentials");
    configJson.put("config_file_contents", configFileJson);
    configJson.put("use_host_credentials", true);
    bodyJson.set("config", configJson);
    Result result = createProvider(bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertValue(json, "name", providerName);
    Provider provider = Provider.get(customer.uuid, UUID.fromString(json.path("uuid").asText()));
    Map<String, String> config = provider.getConfig();
    assertTrue(config.isEmpty());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateKubernetesMultiRegionProvider() {
    ObjectMapper mapper = new ObjectMapper();

    String providerName = "Kubernetes-Provider";
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "kubernetes");
    bodyJson.put("name", providerName);
    ObjectNode configJson = Json.newObject();
    configJson.put("KUBECONFIG_NAME", "test");
    configJson.put("KUBECONFIG_CONTENT", "test");
    bodyJson.set("config", configJson);

    ArrayNode regions = mapper.createArrayNode();
    ObjectNode regionJson = Json.newObject();
    regionJson.put("code", "US-West");
    regionJson.put("name", "US West");
    ArrayNode azs = mapper.createArrayNode();
    ObjectNode azJson = Json.newObject();
    azJson.put("code", "us-west1-a");
    azJson.put("name", "us-west1-a");
    azs.add(azJson);
    regionJson.putArray("zoneList").addAll(azs);
    regions.add(regionJson);

    bodyJson.putArray("regionList").addAll(regions);

    Result result = createKubernetesProvider(bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);
    assertValue(json, "name", providerName);
    Provider provider = Provider.get(customer.uuid, UUID.fromString(json.path("uuid").asText()));
    Map<String, String> config = provider.getConfig();
    assertFalse(config.isEmpty());
    List<Region> createdRegions = Region.getByProvider(provider.uuid);
    assertEquals(1, createdRegions.size());
    List<AvailabilityZone> createdZones = AvailabilityZone.getAZsForRegion(createdRegions.get(0).uuid);
    assertEquals(1, createdZones.size());
    assertAuditEntry(1, customer.uuid);
  }


  @Test
  public void testCreateKubernetesMultiRegionProviderFailure() {
    ObjectMapper mapper = new ObjectMapper();

    String providerName = "Kubernetes-Provider";
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "kubernetes");
    bodyJson.put("name", providerName);
    ObjectNode configJson = Json.newObject();
    configJson.put("KUBECONFIG_NAME", "test");
    configJson.put("KUBECONFIG_CONTENT", "test");
    bodyJson.set("config", configJson);

    ArrayNode regions = mapper.createArrayNode();
    ObjectNode regionJson = Json.newObject();
    regionJson.put("code", "US-West");
    regionJson.put("name", "US West");
    ArrayNode azs = mapper.createArrayNode();
    ObjectNode azJson = Json.newObject();
    azJson.put("code", "us-west1-a");
    azJson.put("name", "us-west1-a");
    azJson.put("config", configJson);
    azs.add(azJson);
    regionJson.putArray("zoneList").addAll(azs);
    regions.add(regionJson);

    bodyJson.putArray("regionList").addAll(regions);

    Result result = createKubernetesProvider(bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertBadRequest(result, "Kubeconfig can't be at two levels");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testDeleteProviderWithAccessKey() {
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "region 1", "yb image");
    AccessKey ak = AccessKey.create(p.uuid, "access-key-code", new AccessKey.KeyInfo());
    Result result = deleteProvider(p.uuid);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertThat(json.asText(),
        allOf(notNullValue(), equalTo("Deleted provider: " + p.uuid)));
    assertEquals(0, AccessKey.getAll(p.uuid).size());
    assertNull(Provider.get(p.uuid));
    verify(mockAccessManager, times(1)).deleteKey(r.uuid, ak.getKeyCode());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testDeleteProviderWithMultiRegionAccessKey() {
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "region 1", "yb image");
    Region r1 = Region.create(p, "region-2", "region 2", "yb image");
    AccessKey ak = AccessKey.create(p.uuid, "access-key-code", new AccessKey.KeyInfo());
    Result result = deleteProvider(p.uuid);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertThat(json.asText(),
        allOf(notNullValue(), equalTo("Deleted provider: " + p.uuid)));
    assertEquals(0, AccessKey.getAll(p.uuid).size());
    assertNull(Provider.get(p.uuid));
    verify(mockAccessManager, times(1)).deleteKey(r.uuid, ak.getKeyCode());
    verify(mockAccessManager, times(1)).deleteKey(r1.uuid, ak.getKeyCode());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testDeleteProviderWithInvalidProviderUUID() {
    UUID providerUUID = UUID.randomUUID();
    Result result = deleteProvider(providerUUID);
    assertBadRequest(result, "Invalid Provider UUID: " + providerUUID);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testDeleteProviderWithUniverses() {
    Provider p = ModelFactory.awsProvider(customer);
    Universe universe = createUniverse(customer.getCustomerId());
    UniverseDefinitionTaskParams.UserIntent userIntent = new UniverseDefinitionTaskParams.UserIntent();
    userIntent.provider = p.uuid.toString();
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone az2 = AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    userIntent.regionList = new ArrayList<UUID>();
    userIntent.regionList.add(r.uuid);
    universe = Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater(userIntent));
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();
    Result result = deleteProvider(p.uuid);
    assertBadRequest(result, "Cannot delete Provider with Universes");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testDeleteProviderWithoutAccessKey() {
    Provider p = ModelFactory.awsProvider(customer);
    Result result = deleteProvider(p.uuid);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertThat(json.asText(),
        allOf(notNullValue(), equalTo("Deleted provider: " + p.uuid)));
    assertNull(Provider.get(p.uuid));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testDeleteProviderWithProvisionScript() {
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.onprem);
    AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
    String scriptFile = createTempFile("provision_instance.py", "some script");
    keyInfo.provisionInstanceScript = scriptFile;
    AccessKey.create(p.uuid, "access-key-code", keyInfo);
    Result result = deleteProvider(p.uuid);
    assertOk(result);
    assertFalse(new File(scriptFile).exists());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testEditProviderKubernetes() {
    Map<String, String> config = new HashMap<String, String>();
    config.put("KUBECONFIG_PROVIDER", "gke");
    config.put("KUBECONFIG_SERVICE_ACCOUNT", "yugabyte-helm");
    config.put("KUBECONFIG_STORAGE_CLASSES", "");
    config.put("KUBECONFIG", "test.conf");
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.kubernetes, config);

    ObjectNode bodyJson = Json.newObject();
    config.put("KUBECONFIG_STORAGE_CLASSES", "slow");
    bodyJson.put("config", Json.toJson(config));

    Result result = editProvider(bodyJson, p.uuid);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(p.uuid, UUID.fromString(json.get("uuid").asText()));
    p.refresh();
    assertEquals("slow", p.getConfig().get("KUBECONFIG_STORAGE_CLASSES"));
    assertAuditEntry(1, customer.uuid);
  }

   @Test
  public void testEditProviderKubernetesConfigEdit() {
    Map<String, String> config = new HashMap<String, String>();
    config.put("KUBECONFIG_PROVIDER", "gke");
    config.put("KUBECONFIG_SERVICE_ACCOUNT", "yugabyte-helm");
    config.put("KUBECONFIG_STORAGE_CLASSES", "");
    config.put("KUBECONFIG", "test.conf");
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.kubernetes, config);

    ObjectNode bodyJson = Json.newObject();
    config.put("KUBECONFIG_NAME", "test2.conf");
    config.put("KUBECONFIG_CONTENT", "test5678");
    bodyJson.put("config", Json.toJson(config));

    Result result = editProvider(bodyJson, p.uuid);
    assertOk(result);
    assertAuditEntry(1, customer.uuid);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(p.uuid, UUID.fromString(json.get("uuid").asText()));
    p.refresh();
    assertTrue(p.getConfig().get("KUBECONFIG").contains("test2.conf"));
    Path path = Paths.get(p.getConfig().get("KUBECONFIG"));
    try {
      List contents = Files.readAllLines(path);
      assertEquals(contents.get(0), "test5678");
    } catch(IOException e) {
      // Do nothing
    }
  }

  @Test
  public void testEditProviderWithAWSProviderType() {
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.aws);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("hostedZoneId", "1234");
    mockDnsManagerListSuccess();
    Result result = editProvider(bodyJson, p.uuid);
    verify(mockDnsManager, times(1)).listDnsRecord(any(), any());
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(p.uuid, UUID.fromString(json.get("uuid").asText()));
    p.refresh();
    assertEquals("1234", p.getConfig().get("AWS_HOSTED_ZONE_ID"));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testEditProviderWithInvalidProviderType()  {
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.onprem);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("hostedZoneId", "1234");
    Result result = editProvider(bodyJson, p.uuid);
    verify(mockDnsManager, times(0)).listDnsRecord(any(), any());
    assertBadRequest(result, "Expected aws/k8s, but found providers with code: onprem");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testEditProviderWithEmptyHostedZoneId() {
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.aws);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("hostedZoneId", "");
    Result result = editProvider(bodyJson, p.uuid);
    verify(mockDnsManager, times(0)).listDnsRecord(any(), any());
    assertBadRequest(result, "Required field hosted zone id");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateAwsProviderWithValidHostedZoneId() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "aws");
    bodyJson.put("name", "aws-Provider");
    ObjectNode configJson = Json.newObject();
    configJson.put("AWS_HOSTED_ZONE_ID", "1234");
    bodyJson.set("config", configJson);

    mockDnsManagerListSuccess("test");
    Result result = createProvider(bodyJson);
    verify(mockDnsManager, times(1)).listDnsRecord(any(), any());
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));

    Provider provider = Provider.get(customer.uuid, UUID.fromString(json.path("uuid").asText()));
    assertNotNull(provider);
    assertEquals("1234", provider.getAwsHostedZoneId());
    assertEquals("test", provider.getAwsHostedZoneName());
    assertEquals("1234", provider.getConfig().get("AWS_HOSTED_ZONE_ID"));
    assertEquals("test", provider.getConfig().get("AWS_HOSTED_ZONE_NAME"));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateAwsProviderWithInvalidDevopsReply() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "aws");
    bodyJson.put("name", "aws-Provider");
    ObjectNode configJson = Json.newObject();
    configJson.put("AWS_HOSTED_ZONE_ID", "1234");
    bodyJson.set("config", configJson);

    mockDnsManagerListFailure("fail", 0);
    Result result = createProvider(bodyJson);
    verify(mockDnsManager, times(1)).listDnsRecord(any(), any());
    assertInternalServerError(result, "Invalid devops API response: ");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateAwsProviderWithInvalidHostedZoneId() {
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("code", "aws");
    bodyJson.put("name", "aws-Provider");
    ObjectNode configJson = Json.newObject();
    configJson.put("AWS_HOSTED_ZONE_ID", "1234");
    bodyJson.set("config", configJson);

    mockDnsManagerListFailure("fail", 1);
    Result result = createProvider(bodyJson);
    verify(mockDnsManager, times(1)).listDnsRecord(any(), any());
    assertInternalServerError(result, "Invalid devops API response: ");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testGcpBootstrapMultiRegionNoRegionInput() {
    Provider provider = ModelFactory.gcpProvider(customer);
    ObjectNode bodyJson = Json.newObject();
    prepareBootstrap(bodyJson, provider, true);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testGcpBootstrapMultiRegionSomeRegionInput() {
    Provider provider = ModelFactory.gcpProvider(customer);
    ObjectNode bodyJson = Json.newObject();
    ObjectNode perRegionMetadata = Json.newObject();
    perRegionMetadata.put("region1", Json.newObject());
    bodyJson.put("perRegionMetadata", perRegionMetadata);
    prepareBootstrap(bodyJson, provider, false);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testAwsBootstrapWithDestVpcId() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class),
            any(CloudBootstrap.Params.class))).thenReturn(fakeTaskUUID);
    Provider provider = ModelFactory.awsProvider(customer);
    ObjectNode bodyJson = Json.newObject();
    bodyJson.put("destVpcId", "nofail");
    Result result = bootstrapProvider(bodyJson, provider);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertNotNull(json);
    assertNotNull(json.get("taskUUID"));
    assertAuditEntry(1, customer.uuid);
  }

  private void prepareBootstrap(
      ObjectNode bodyJson, Provider provider, boolean expectCallToGetRegions) {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class),
            any(CloudBootstrap.Params.class))).thenReturn(fakeTaskUUID);
    when(mockCloudQueryHelper.getRegions(provider.uuid))
                             .thenReturn(Json.parse("[\"region1\",\"region2\"]"));
    Result result = bootstrapProvider(bodyJson, provider);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertNotNull(json);
    assertNotNull(json.get("taskUUID"));
    // TODO(bogdan): figure out a better way to inspect what tasks and with what params get started.
    verify(mockCloudQueryHelper, times(expectCallToGetRegions ? 1 : 0)).getRegions(provider.uuid);
  }

  private void mockDnsManagerListSuccess() {
    mockDnsManagerListSuccess("test");
  }

  private void mockDnsManagerListSuccess(String mockDnsName) {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"name\": \"" + mockDnsName + "\"}";
    shellResponse.code = 0;
    when(mockDnsManager.listDnsRecord(any(), any())).thenReturn(shellResponse);
  }

  private void mockDnsManagerListFailure(String mockFailureMessage, int successCode) {
    ShellResponse shellResponse =  new ShellResponse();
    shellResponse.message = "{\"wrong_key\": \"" + mockFailureMessage + "\"}";
    shellResponse.code = successCode;
    when(mockDnsManager.listDnsRecord(any(), any())).thenReturn(shellResponse);
  }
}
