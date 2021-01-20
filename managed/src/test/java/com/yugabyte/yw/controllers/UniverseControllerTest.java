// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.common.ApiUtils.getDefaultUserIntent;
import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static com.yugabyte.yw.common.AssertHelper.*;
import static com.yugabyte.yw.common.FakeApiHelper.doRequestWithAuthToken;
import static com.yugabyte.yw.common.FakeApiHelper.doRequestWithAuthTokenAndBody;
import static com.yugabyte.yw.common.PlacementInfoUtil.UNIVERSE_ALIVE_METRIC;
import static com.yugabyte.yw.common.PlacementInfoUtil.getAzUuidToNumNodes;
import static com.yugabyte.yw.common.PlacementInfoUtil.updateUniverseDefinition;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;

import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.RunInShellFormData;
import static com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterOperationType.CREATE;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertTrue;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.eq;
import static org.mockito.Matchers.anyList;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static play.inject.Bindings.bind;
import static play.test.Helpers.contentAsString;
import static play.mvc.Http.Status.FORBIDDEN;

import java.io.File;
import java.io.IOException;
import java.util.*;

import com.google.common.collect.ImmutableMap;
import com.google.common.net.HostAndPort;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.ConfigHelper;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.YsqlQueryExecutor;
import com.yugabyte.yw.common.YcqlQueryExecutor;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.forms.NodeInstanceFormData;
import com.yugabyte.yw.forms.UpgradeParams;
import com.yugabyte.yw.forms.RunQueryFormData;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.KmsConfig;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Users;
import com.yugabyte.yw.models.helpers.DeviceInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.models.helpers.PlacementInfo;

import junitparams.JUnitParamsRunner;
import junitparams.Parameters;

import com.yugabyte.yw.models.helpers.TaskType;
import org.apache.commons.io.FileUtils;
import org.apache.commons.lang3.StringUtils;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Matchers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.commissioner.CallHome;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.forms.EncryptionAtRestKeyParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.forms.UniverseTaskParams;

import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;

import org.mockito.Mock;
import org.mockito.Mockito;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;
import org.yb.client.YBClient;
import play.Application;
import play.api.Play;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

@RunWith(JUnitParamsRunner.class)
public class UniverseControllerTest extends WithApplication {
  private static Commissioner mockCommissioner;
  private static MetricQueryHelper mockMetricQueryHelper;

  @Rule
  public MockitoRule rule = MockitoJUnit.rule();

  @Mock
  private play.Configuration mockAppConfig;

  private Customer customer;
  private Users user;
  private KmsConfig kmsConfig;
  private String authToken;
  private YBClientService mockService;
  private YBClient mockClient;
  private ApiHelper mockApiHelper;
  private CallHome mockCallHome;
  private EncryptionAtRestManager mockEARManager;
  private YsqlQueryExecutor mockYsqlQueryExecutor;
  private YcqlQueryExecutor mockYcqlQueryExecutor;
  private ShellProcessHandler mockShellProcessHandler;
  protected CallbackController mockCallbackController;
  protected PlayCacheSessionStore mockSessionStore;

  @Override
  protected Application provideApplication() {
    mockCommissioner = mock(Commissioner.class);
    mockMetricQueryHelper = mock(MetricQueryHelper.class);
    mockClient = mock(YBClient.class);
    mockService = mock(YBClientService.class);
    mockApiHelper = mock(ApiHelper.class);
    mockCallHome = mock(CallHome.class);
    mockEARManager = mock(EncryptionAtRestManager.class);
    mockYsqlQueryExecutor = mock(YsqlQueryExecutor.class);
    mockYcqlQueryExecutor = mock(YcqlQueryExecutor.class);
    mockShellProcessHandler = mock(ShellProcessHandler.class);
    mockCallbackController = mock(CallbackController.class);
    mockSessionStore = mock(PlayCacheSessionStore.class);
    return new GuiceApplicationBuilder()
        .configure((Map) Helpers.inMemoryDatabase())
        .overrides(bind(Commissioner.class).toInstance(mockCommissioner))
        .overrides(bind(MetricQueryHelper.class).toInstance(mockMetricQueryHelper))
        .overrides(bind(ApiHelper.class).toInstance(mockApiHelper))
        .overrides(bind(CallHome.class).toInstance(mockCallHome))
        .overrides(bind(EncryptionAtRestManager.class).toInstance(mockEARManager))
        .overrides(bind(YsqlQueryExecutor.class).toInstance(mockYsqlQueryExecutor))
        .overrides(bind(YcqlQueryExecutor.class).toInstance(mockYcqlQueryExecutor))
        .overrides(bind(ShellProcessHandler.class).toInstance(mockShellProcessHandler))
        .overrides(bind(CallbackController.class).toInstance(mockCallbackController))
        .overrides(bind(PlaySessionStore.class).toInstance(mockSessionStore))
        .overrides(bind(play.Configuration.class).toInstance(mockAppConfig))
        .build();
  }

  private PlacementInfo constructPlacementInfoObject(Map<UUID, Integer> azToNumNodesMap) {

    Map<UUID, PlacementInfo.PlacementCloud> placementCloudMap = new HashMap<>();
    Map<UUID, PlacementInfo.PlacementRegion> placementRegionMap = new HashMap<>();
    for (UUID azUUID : azToNumNodesMap.keySet()) {
      AvailabilityZone currentAz = AvailabilityZone.get(azUUID);

      // Get existing PlacementInfo Cloud or set up a new one.
      Provider currentProvider = currentAz.getProvider();
      PlacementInfo.PlacementCloud cloudItem = placementCloudMap.getOrDefault(currentProvider.uuid, null);
      if (cloudItem == null) {
        cloudItem = new PlacementInfo.PlacementCloud();
        cloudItem.uuid = currentProvider.uuid;
        cloudItem.code = currentProvider.code;
        cloudItem.regionList = new ArrayList<>();
        placementCloudMap.put(currentProvider.uuid, cloudItem);
      }

      // Get existing PlacementInfo Region or set up a new one.
      Region currentRegion = currentAz.region;
      PlacementInfo.PlacementRegion regionItem = placementRegionMap.getOrDefault(currentRegion.uuid, null);
      if (regionItem == null) {
        regionItem = new PlacementInfo.PlacementRegion();
        regionItem.uuid = currentRegion.uuid;
        regionItem.name = currentRegion.name;
        regionItem.code = currentRegion.code;
        regionItem.azList = new ArrayList<>();
        cloudItem.regionList.add(regionItem);
        placementRegionMap.put(currentRegion.uuid, regionItem);
      }

      // Get existing PlacementInfo AZ or set up a new one.
      PlacementInfo.PlacementAZ azItem = new PlacementInfo.PlacementAZ();
      azItem.name = currentAz.name;
      azItem.subnet = currentAz.subnet;
      azItem.replicationFactor = 1;
      azItem.uuid = currentAz.uuid;
      azItem.numNodesInAZ = azToNumNodesMap.get(azUUID);
      regionItem.azList.add(azItem);
    }
    PlacementInfo placementInfo = new PlacementInfo();
    placementInfo.cloudList = ImmutableList.copyOf(placementCloudMap.values());
    return placementInfo;
  }

  private static boolean areConfigObjectsEqual(ArrayNode nodeDetailSet, Map<UUID, Integer> azToNodeMap) {
    for (JsonNode nodeDetail : nodeDetailSet) {
      UUID azUUID = UUID.fromString(nodeDetail.get("azUuid").asText());
      azToNodeMap.put(azUUID, azToNodeMap.getOrDefault(azUUID, 0)-1);
    }
    return !azToNodeMap.values().removeIf(nodeDifference -> nodeDifference != 0);
  }

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer();
    user = ModelFactory.testUser(customer);
    ObjectNode kmsConfigReq = Json.newObject()
            .put("name", "some config name")
            .put("base_url", "some_base_url")
            .put("api_key", "some_api_token");
    kmsConfig = ModelFactory.createKMSConfig(customer.uuid, "SMARTKEY", kmsConfigReq);
    authToken = user.createAuthToken();

    when(mockAppConfig.getString("yb.storage.path")).thenReturn("/tmp");
  }

  @After
  public void tearDown() throws IOException {
    FileUtils.deleteDirectory(new File("/tmp/certs"));
  }

  @Test
  public void testEmptyUniverseListWithValidUUID() {
    Result result = doRequestWithAuthToken("GET", "/api/customers/" + customer.uuid + "/universes", authToken);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.isArray());
    assertEquals(json.size(), 0);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseListWithValidUUID() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();

    Result result = doRequestWithAuthToken("GET", "/api/customers/" + customer.uuid + "/universes", authToken);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertNotNull(json);
    assertTrue(json.isArray());
    assertEquals(1, json.size());
    assertValue(json.get(0), "universeUUID", u.universeUUID.toString());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseListWithInvalidUUID() {
    UUID invalidUUID = UUID.randomUUID();
    Result result = doRequestWithAuthToken("GET", "/api/customers/" + invalidUUID + "/universes", authToken);
    assertEquals(FORBIDDEN, result.status());

    String resultString = contentAsString(result);
    assertThat(resultString, allOf(notNullValue(),
        equalTo("Unable To Authenticate User")));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseBackupFlagSuccess() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
                 "/update_backup_state?markActive=true";
    Result result = doRequestWithAuthToken("PUT", url, authToken);
    assertOk(result);
    assertThat(Universe.get(u.universeUUID).getConfig().get(Universe.TAKE_BACKUPS),
               allOf(notNullValue(), equalTo("true")));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseBackupFlagFailure() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
                 "/update_backup_state";
    Result result = doRequestWithAuthToken("PUT", url, authToken);
    assertBadRequest(result, "Invalid Query: Need to specify markActive value");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGetWithInvalidCustomerUUID() {
    UUID invalidUUID = UUID.randomUUID();
    String url = "/api/customers/" + invalidUUID + "/universes/" + UUID.randomUUID();
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertEquals(FORBIDDEN, result.status());

    String resultString = contentAsString(result);
    assertThat(resultString, allOf(notNullValue(),
        equalTo("Unable To Authenticate User")));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGetWithInvalidUniverseUUID() {
    UUID invalidUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + invalidUUID;
    Result result = doRequestWithAuthToken("GET", url, authToken);
    String expectedResult = String.format("No universe found with UUID: %s", invalidUUID);
    assertBadRequest(result, expectedResult);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGetWithValidUniverseUUID() {
    UserIntent ui = getDefaultUserIntent(customer);
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater(ui));

    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID;
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    JsonNode universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    JsonNode clustersJson = universeDetails.get("clusters");
    assertNotNull(clustersJson);
    JsonNode primaryClusterJson = clustersJson.get(0);
    assertNotNull(primaryClusterJson);
    JsonNode userIntentJson = primaryClusterJson.get("userIntent");
    assertNotNull(userIntentJson);
    assertThat(userIntentJson.get("replicationFactor").asInt(), allOf(notNullValue(), equalTo(3)));

    JsonNode nodeDetailsMap = universeDetails.get("nodeDetailsSet");
    assertNotNull(nodeDetailsMap);
    assertNotNull(json.get("resources"));
    for (Iterator<JsonNode> it = nodeDetailsMap.elements(); it.hasNext(); ) {
      JsonNode node = it.next();
      int nodeIdx = node.get("nodeIdx").asInt();
      assertValue(node, "nodeName", "host-n" + nodeIdx);
    }
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testGetMasterLeaderWithValidParams() {
    Universe universe = createUniverse(customer.getCustomerId());
    String host = "1.2.3.4";
    HostAndPort hostAndPort = HostAndPort.fromParts(host, 9000);
    when(mockClient.getLeaderMasterHostAndPort()).thenReturn(hostAndPort);
    when(mockService.getClient(any(), any())).thenReturn(mockClient);
    UniverseController universeController = new UniverseController(mockService);

    Result result = universeController.getMasterLeaderIP(customer.uuid, universe.universeUUID);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "privateIP", host);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testGetMasterLeaderWithInvalidCustomerUUID() {
    UniverseController universeController = new UniverseController(mockService);
    UUID invalidUUID = UUID.randomUUID();
    Result result = universeController.getMasterLeaderIP(invalidUUID, UUID.randomUUID());
    assertBadRequest(result, "No customer found with UUID: " + invalidUUID);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testGetMasterLeaderWithInvalidUniverseUUID() {
    UniverseController universeController = new UniverseController(mockService);
    UUID invalidUUID = UUID.randomUUID();
    Result result = universeController.getMasterLeaderIP(customer.uuid, invalidUUID);
    assertBadRequest(result, "No universe found with UUID: " + invalidUUID);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithInvalidParams() {
    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, Json.newObject());
    assertBadRequest(result, "clusters: This field is required");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithInvalidUniverseName() {
    String url = "/api/customers/" + customer.uuid + "/universes";
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
        .put("universeName", "Foo_Bar")
        .put("instanceType", i.getInstanceTypeCode())
        .put("replicationFactor", 3)
        .put("numNodes", 3)
        .put("provider", p.uuid.toString());
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, "Invalid universe name format, valid characters [a-zA-Z0-9-].");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithoutAvailabilityZone() {
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "SingleUserUniverse")
      .put("instanceType", "a-instance")
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString());
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.put("currentClusterType", "PRIMARY");
    bodyJson.put("clusterOperation", "CREATE");

    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertInternalServerError(result, "No AZ found across regions: [" + r.uuid + "]");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithSingleAvailabilityZones() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class), Matchers.any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "SingleUserUniverse")
      .put("instanceType", i.getInstanceTypeCode())
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertNotNull(json.get("universeUUID"));
    assertNotNull(json.get("universeDetails"));
    assertNotNull(json.get("universeConfig"));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("SingleUserUniverse")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Create)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithSelfSignedTLS() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class), Matchers.any(UniverseDefinitionTaskParams.class)))
      .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "SingleUserUniverse")
      .put("instanceType", i.getInstanceTypeCode())
      .put("enableNodeToNodeEncrypt", true)
      .put("enableClientToNodeEncrypt", true)
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    ObjectNode bodyJson = Json.newObject()
      .put("nodePrefix", "demo-node");
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    ArgumentCaptor<UniverseTaskParams> taskParams = ArgumentCaptor.forClass(UniverseTaskParams.class);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.CreateUniverse), taskParams.capture());
    UniverseDefinitionTaskParams taskParam = (UniverseDefinitionTaskParams) taskParams.getValue();
    assertNotNull(taskParam.rootCA);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithoutSelfSignedTLS() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class), Matchers.any(UniverseDefinitionTaskParams.class)))
      .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "SingleUserUniverse")
      .put("instanceType", i.getInstanceTypeCode())
      .put("enableNodeToNodeEncrypt", false)
      .put("enableClientToNodeEncrypt", false)
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    ArgumentCaptor<UniverseTaskParams> taskParams = ArgumentCaptor.forClass(UniverseTaskParams.class);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.CreateUniverse), taskParams.capture());
    UniverseDefinitionTaskParams taskParam = (UniverseDefinitionTaskParams) taskParams.getValue();
    assertNull(taskParam.rootCA);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseUpdateWithInvalidParams() {
    Universe u = createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, Json.newObject());
    assertBadRequest(result, "clusters: This field is required");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseUpdateWithValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class), Matchers.any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", u.name)
      .put("instanceType", i.getInstanceTypeCode())
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    assertNotNull(json.get("universeDetails"));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Update)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithInvalidTServerJson() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class), Matchers.any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("masterGFlags", "abcd")
      .put("universeName", u.name)
      .put("instanceType", i.getInstanceTypeCode())
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    assertNotNull(json.get("universeDetails"));
    assertNotNull(json.get("universeConfig"));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Update)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseExpand() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class), Matchers.any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", u.name)
      .put("numNodes", 5)
      .put("instanceType", i.getInstanceTypeCode())
      .put("replicationFactor", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    JsonNode universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    JsonNode clustersJson = universeDetails.get("clusters");
    assertNotNull(clustersJson);
    JsonNode primaryClusterJson = clustersJson.get(0);
    assertNotNull(primaryClusterJson);
    assertNotNull(primaryClusterJson.get("userIntent"));
    assertAuditEntry(1, customer.uuid);

    fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
         Matchers.any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    // Try universe expand only, and re-check.
    userIntentJson.put("numNodes", 9);
    result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);
    assertOk(result);
    json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    clustersJson = universeDetails.get("clusters");
    assertNotNull(clustersJson);
    primaryClusterJson = clustersJson.get(0);
    assertNotNull(primaryClusterJson);
    assertNotNull(primaryClusterJson.get("userIntent"));
    assertAuditEntry(2, customer.uuid);
  }

  @Test
  public void testUniverseDestroyInvalidUUID() {
    UUID randomUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + randomUUID;
    Result result = doRequestWithAuthToken("DELETE", url, authToken);
    assertBadRequest(result, "No universe found with UUID: " + randomUUID);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseDestroyValidUUID() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any()))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    // Add the cloud info into the universe.
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = new UniverseDefinitionTaskParams();
        UserIntent userIntent = new UserIntent();
        userIntent.providerType = CloudType.aws;
        universeDetails.upsertPrimaryCluster(userIntent, null);
        universe.setUniverseDetails(universeDetails);
      }
    };
    // Save the updates to the universe.
    Universe.saveDetails(u.universeUUID, updater);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthToken("DELETE", url, authToken);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Delete)));

    assertTrue(customer.getUniverseUUIDs().isEmpty());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseDestroyValidUUIDIsForceDelete() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any()))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    UUID randUUID = UUID.randomUUID();
    CustomerTask.create(customer, u.universeUUID, randUUID, CustomerTask.TargetType.Backup,
        CustomerTask.TaskType.Create, "test");

    // Add the cloud info into the universe.
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = new UniverseDefinitionTaskParams();
        UserIntent userIntent = new UserIntent();
        userIntent.providerType = CloudType.aws;
        universeDetails.upsertPrimaryCluster(userIntent, null);
        universe.setUniverseDetails(universeDetails);
      }
    };
    // Save the updates to the universe.
    Universe.saveDetails(u.universeUUID, updater);

    String url = "/api/customers/" + customer.uuid + "/universes/"
        + u.universeUUID + "?isForceDelete=true";
    Result result = doRequestWithAuthToken("DELETE", url, authToken);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Delete)));
    assertNotNull(CustomerTask.findByTaskUUID(randUUID).getCompletionTime());

    assertTrue(customer.getUniverseUUIDs().isEmpty());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseUpgradeWithEmptyParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, Json.newObject());
    assertBadRequest(result, "clusters: This field is required");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  private ObjectNode getValidPayload(UUID univUUID, String upgradeOption) {
    ObjectNode bodyJson = Json.newObject()
                              .put("universeUUID", univUUID.toString())
                              .put("taskType", "Software")
                              .put("upgradeOption", upgradeOption)
                              .put("ybSoftwareVersion", "0.0.1");
    ObjectNode userIntentJson = Json.newObject()
       .put("universeName", "Single UserUniverse")
       .put("ybSoftwareVersion", "0.0.1");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    return bodyJson;
  }

  // Change the node state to removed, for one of the nodes in the given universe uuid.
  private void setInTransitNode(UUID universeUUID) {
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        NodeDetails node = universeDetails.nodeDetailsSet.iterator().next();
        node.state = NodeState.Removed;
        universe.setUniverseDetails(universeDetails);
      }
    };
    Universe.saveDetails(universeUUID, updater);
  }

  private void testUniverseUpgradeWithNodesInTransitHelper(String upgradeOption) {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    setInTransitNode(uUUID);

    ObjectNode bodyJson = getValidPayload(uUUID, upgradeOption);
    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    if (upgradeOption.equals("Rolling")) {
      assertBadRequest(result, "as it has nodes in one of");
      assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
      assertAuditEntry(0, customer.uuid);
    } else {
      assertOk(result);
      JsonNode json = Json.parse(contentAsString(result));
      assertValue(json, "taskUUID", fakeTaskUUID.toString());
      assertAuditEntry(1, customer.uuid);
    }
  }

  @Test
  public void testUniverseUpgradeWithNodesInTransit() {
    testUniverseUpgradeWithNodesInTransitHelper("Rolling");
  }

  @Test
  public void testUniverseUpgradeWithNodesInTransitNonRolling() {
    testUniverseUpgradeWithNodesInTransitHelper("Non-Rolling");
  }

  @Test
  public void testUniverseUpgradeWithNodesInTransitNonRestart() {
    testUniverseUpgradeWithNodesInTransitHelper("Non-Restart");
  }

  @Test
  public void testUniverseExpandWithTransitNodes() {
    UUID fakeTaskUUID = UUID.randomUUID();

    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    setInTransitNode(u.universeUUID);

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", u.name)
      .put("numNodes", 5)
      .put("instanceType", i.getInstanceTypeCode())
      .put("replicationFactor", 3)
      .put("provider", p.uuid.toString());
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);
    assertBadRequest(result, "as it has nodes in one of");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseSoftwareUpgradeValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = getValidPayload(u.universeUUID, "Rolling");
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.UpgradeSoftware)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseRollingRestartValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "Restart")
        .put("upgradeOption", "Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject()
                                                 .set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Restart)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseRollingRestartNonRolling() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "Restart")
        .put("upgradeOption", "Non-Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject()
                                                 .set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertBadRequest(result, "Rolling restart has to be a ROLLING UPGRADE.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseSoftwareUpgradeWithInvalidParams() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "Software");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertBadRequest(result, "ybSoftwareVersion param is required for taskType: Software");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \"master-flag\", \"value\": \"123\"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \"tserver-flag\", \"value\": \"456\"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.UpgradeGflags)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithInvalidParams() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Test Universe");
    userIntentJson.set("masterGFlags", Json.parse("[\"gflag1\", \"123\"]"));
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithSameGFlags() {
    Universe u = createUniverse(customer.getCustomerId());

    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        UserIntent userIntent = universeDetails.getPrimaryCluster().userIntent;
        userIntent.masterGFlags = ImmutableMap.of("master-flag", "123");
        userIntent.tserverGFlags = ImmutableMap.of("tserver-flag", "456");
        universe.setUniverseDetails(universeDetails);
      }
    };
    Universe.saveDetails(u.universeUUID, updater);

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "GFlags")
        .put("upgradeOption", "Non-Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", u.name);
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \"master-flag\", \"value\": \"123\"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \"tserver-flag\", \"value\": \"456\"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertBadRequest(result, "Neither master nor tserver gflags changed");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithMissingGflags() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJsonMissingGFlags = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJsonMissingGFlags.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJsonMissingGFlags);

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithMalformedTServerFlags() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject()
        .put("universeName", "Single UserUniverse")
        .put("tserverGFlags", "abcd");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithMalformedMasterGFlags() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
        .put("universeUUID", u.universeUUID.toString())
        .put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject()
        .put("universeName", "Single UserUniverse")
        .put("masterGFlags", "abcd");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseNonRollingGFlagsUpgrade() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
      .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
      .put("universeUUID", u.universeUUID.toString())
      .put("taskType", "GFlags")
      .put("upgradeOption", "Non-Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \"master-flag\", \"value\": \"123\"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \"tserver-flag\", \"value\": \"456\"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    ArgumentCaptor<UniverseTaskParams> taskParams = ArgumentCaptor.forClass(UniverseTaskParams.class);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), taskParams.capture());
    UpgradeParams taskParam = (UpgradeParams) taskParams.getValue();
    assertEquals(taskParam.upgradeOption, UpgradeParams.UpgradeOption.NON_ROLLING_UPGRADE);
    assertEquals(taskParam.masterGFlags, ImmutableMap.of("master-flag", "123"));
    assertEquals(taskParam.tserverGFlags, ImmutableMap.of("tserver-flag", "456"));
    UserIntent primaryClusterIntent = taskParam.getPrimaryCluster().userIntent;
    assertEquals(primaryClusterIntent.masterGFlags, taskParam.masterGFlags);
    assertEquals(primaryClusterIntent.tserverGFlags, taskParam.tserverGFlags);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseNonRollingSoftwareUpgrade() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
      .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = Json.newObject()
      .put("universeUUID", u.universeUUID.toString())
      .put("taskType", "Software")
      .put("upgradeOption", "Non-Rolling")
      .put("ybSoftwareVersion", "new-version");
    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "Single UserUniverse")
      .put("ybSoftwareVersion", "new-version");
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    ArgumentCaptor<UniverseTaskParams> taskParams = ArgumentCaptor.forClass(UniverseTaskParams.class);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), taskParams.capture());
    UpgradeParams taskParam = (UpgradeParams) taskParams.getValue();
    assertEquals(taskParam.upgradeOption, UpgradeParams.UpgradeOption.NON_ROLLING_UPGRADE);
    assertEquals("new-version", taskParam.ybSoftwareVersion);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseStatusSuccess() {
    JsonNode fakeReturn = Json.newObject().set(UNIVERSE_ALIVE_METRIC, Json.newObject());
    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(fakeReturn);
    Universe u = createUniverse("TestUniverse", customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/status";
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertOk(result);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseStatusError() {
    ObjectNode fakeReturn = Json.newObject().put("error", "foobar");
    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(fakeReturn);

    Universe u = createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/status";
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertBadRequest(result, "foobar");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseStatusBadParams() {
    UUID universeUUID = UUID.randomUUID();
    String url = "/api/customers/" + customer.uuid + "/universes/" + universeUUID + "/status";
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertBadRequest(result, "No universe found with UUID: " + universeUUID);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testFindByNameWithUniverseNameExists() {
    Universe u = createUniverse("TestUniverse", customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/find/" + u.name;
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertBadRequest(result, "Universe already exists");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testFindByNameWithUniverseDoesNotExist() {
    createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/find/FakeUniverse";
    Result result = doRequestWithAuthToken("GET", url, authToken);
    assertOk(result);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCustomConfigureCreateWithMultiAZMultiRegion() {
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5,
        new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.nodePrefix = "univConfCreate";
    taskParams.upsertPrimaryCluster(getTestUserIntent(r, p, i, 5), null);
    PlacementInfoUtil.updateUniverseDefinition(taskParams, customer.getCustomerId(),
        taskParams.getPrimaryCluster().uuid, CREATE);
    Cluster primaryCluster = taskParams.getPrimaryCluster();
    // Needed for the universe_resources call.
    DeviceInfo di = new DeviceInfo();
    di.volumeSize = 100;
    di.numVolumes = 2;
    primaryCluster.userIntent.deviceInfo = di;

    List<PlacementInfo.PlacementAZ> azList =
        primaryCluster.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 3);

    PlacementInfo.PlacementAZ paz = azList.get(0);
    paz.numNodesInAZ += 2;
    primaryCluster.userIntent.numNodes += 2;
    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    topJson.put("currentClusterType", "PRIMARY");
    topJson.put("clusterOperation", "CREATE");

    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.get("nodeDetailsSet").isArray());
    ArrayNode nodeDetailJson = (ArrayNode) json.get("nodeDetailsSet");
    assertEquals(7, nodeDetailJson.size());
    // Now test the resource endpoint also works.
    // TODO: put this in its own test once we refactor the provider+region+az creation and payload
    // generation...
    url = "/api/customers/" + customer.uuid + "/universe_resources";
    result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);
    assertOk(result);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testConfigureCreateWithReadOnlyClusters() {
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");

    Region rReadOnly = Region.create(p, "region-readOnly-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(rReadOnly, "az-readOnly-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(rReadOnly, "az-readOnly-2", "PlacementAZ 2", "subnet-2");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.nodePrefix = "univWithReadOnlyCreate";
    UUID readOnlyUuid0 = UUID.randomUUID();
    UUID readOnlyUuid1 = UUID.randomUUID();
    taskParams.upsertPrimaryCluster(getTestUserIntent(r, p, i, 5), null);
    taskParams.upsertCluster(getTestUserIntent(rReadOnly, p, i, 5), null, readOnlyUuid0);
    taskParams.upsertCluster(getTestUserIntent(rReadOnly, p, i, 5), null, readOnlyUuid1);

    PlacementInfoUtil.updateUniverseDefinition(taskParams, customer.getCustomerId(),
        taskParams.getPrimaryCluster().uuid, CREATE);
    PlacementInfoUtil.updateUniverseDefinition(taskParams, customer.getCustomerId(), readOnlyUuid0, CREATE);
    PlacementInfoUtil.updateUniverseDefinition(taskParams, customer.getCustomerId(), readOnlyUuid1, CREATE);

    Cluster primaryCluster = taskParams.getPrimaryCluster();
    List<PlacementInfo.PlacementAZ> azList = primaryCluster.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 2);

    Cluster readOnlyCluster0 = taskParams.getClusterByUuid(readOnlyUuid0);
    azList = readOnlyCluster0.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 2);

    Cluster readOnlyCluster1 = taskParams.getClusterByUuid(readOnlyUuid1);
    azList = readOnlyCluster1.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 2);

    Map<UUID, Integer> azUUIDToNumNodeMap = getAzUuidToNumNodes(primaryCluster.placementInfo);
    Map<UUID, Integer> azUUIDToNumNodeMapReadOnly0 = getAzUuidToNumNodes(readOnlyCluster0.placementInfo);
    Map<UUID, Integer> azUUIDToNumNodeMapReadOnly1 = getAzUuidToNumNodes(readOnlyCluster1.placementInfo);
    for (Map.Entry<UUID, Integer> entry : azUUIDToNumNodeMapReadOnly0.entrySet()) {
      UUID uuid = entry.getKey();
      int numNodes = entry.getValue();
      if (azUUIDToNumNodeMap.containsKey(uuid)) {
        int prevNumNodes = azUUIDToNumNodeMap.get(uuid);
        azUUIDToNumNodeMap.put(uuid, prevNumNodes + numNodes);
      } else {
        azUUIDToNumNodeMap.put(uuid, numNodes);
      }
    }
    for (Map.Entry<UUID, Integer> entry : azUUIDToNumNodeMapReadOnly1.entrySet()) {
      UUID uuid = entry.getKey();
      int numNodes = entry.getValue();
      if (azUUIDToNumNodeMap.containsKey(uuid)) {
        int prevNumNodes = azUUIDToNumNodeMap.get(uuid);
        azUUIDToNumNodeMap.put(uuid, prevNumNodes + numNodes);
      } else {
        azUUIDToNumNodeMap.put(uuid, numNodes);
      }
    }
    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    topJson.put("currentClusterType", "ASYNC");
    topJson.put("clusterOperation", "CREATE");
    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.get("nodeDetailsSet").isArray());
    ArrayNode nodeDetailJson = (ArrayNode) json.get("nodeDetailsSet");
    assertEquals(15, nodeDetailJson.size());
    assertTrue(areConfigObjectsEqual(nodeDetailJson, azUUIDToNumNodeMap));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCustomConfigureEditWithPureExpand() {
    Provider p = ModelFactory.awsProvider(customer);
    Universe u = createUniverse(customer.getCustomerId());

    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams utd = new UniverseDefinitionTaskParams();
    utd.universeUUID= u.universeUUID;
    UserIntent ui = getTestUserIntent(r, p, i, 5);
    ui.universeName = u.name;
    ui.ybSoftwareVersion = "1.0";
    ui.preferredRegion = ui.regionList.get(0);
    utd.upsertPrimaryCluster(ui, null);
    PlacementInfoUtil.updateUniverseDefinition(utd, customer.getCustomerId(), utd.getPrimaryCluster().uuid,
        UniverseDefinitionTaskParams.ClusterOperationType.CREATE);
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        universe.setUniverseDetails(utd);
      }
    };
    Universe.saveDetails(u.universeUUID, updater);
    u = Universe.get(u.universeUUID);
    int totalNumNodesAfterExpand = 0;
    Map<UUID, Integer> azUuidToNumNodes = getAzUuidToNumNodes(u.getUniverseDetails().nodeDetailsSet);
    for (Map.Entry<UUID, Integer> entry : azUuidToNumNodes.entrySet()) {
      totalNumNodesAfterExpand += entry.getValue() + 1;
      azUuidToNumNodes.put(entry.getKey(), entry.getValue() + 1);
    }
    UniverseDefinitionTaskParams editTestUTD = u.getUniverseDetails();
    Cluster primaryCluster = editTestUTD.getPrimaryCluster();
    primaryCluster.userIntent.numNodes = totalNumNodesAfterExpand;
    primaryCluster.placementInfo = constructPlacementInfoObject(azUuidToNumNodes);

    ObjectNode editJson = (ObjectNode) Json.toJson(editTestUTD);
    editJson.put("currentClusterType", "PRIMARY");
    editJson.put("clusterOperation", "EDIT");
    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, editJson);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.get("nodeDetailsSet").isArray());
    ArrayNode nodeDetailJson = (ArrayNode) json.get("nodeDetailsSet");
    assertEquals(nodeDetailJson.size(), totalNumNodesAfterExpand);
    assertTrue(areConfigObjectsEqual(nodeDetailJson, azUuidToNumNodes));
    assertAuditEntry(0, customer.uuid);
  }

  public UniverseDefinitionTaskParams setupOnPremTestData(int numNodesToBeConfigured, Provider p, Region r, List<AvailabilityZone> azList) {
    int numAZsToBeConfigured = azList.size();
    InstanceType i = InstanceType.upsert(p.code, "type.small", 10, 5.5, new InstanceType.InstanceTypeDetails());

    for (int k = 0; k < numNodesToBeConfigured; ++k) {
      NodeInstanceFormData.NodeInstanceData details = new NodeInstanceFormData.NodeInstanceData();
      details.ip = "10.255.67." + k;
      details.region = r.code;

      if (numAZsToBeConfigured == 2) {
        if (k % 2 == 0) {
          details.zone = azList.get(0).code;
        } else {
          details.zone = azList.get(1).code;
        }
      } else {
        details.zone = azList.get(0).code;
      }
      details.instanceType = "type.small";
      details.nodeName = "test_name" + k;


      if (numAZsToBeConfigured == 2) {
        if (k % 2 == 0) {
          NodeInstance.create(azList.get(0).uuid, details);
        } else {
          NodeInstance.create(azList.get(0).uuid, details);
        }
      } else {
        NodeInstance.create(azList.get(0).uuid, details);
      }
    }

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    UserIntent userIntent = getTestUserIntent(r, p, i, 3);
    userIntent.providerType = CloudType.onprem;
    userIntent.instanceType = "type.small";
    taskParams.nodeDetailsSet = new HashSet<NodeDetails>();

    taskParams.upsertPrimaryCluster(userIntent, null);

    return taskParams;
  }

  @Test
  public void testOnPremConfigureCreateWithValidAZInstanceTypeComboNotEnoughNodes() {
    Provider p = ModelFactory.newProvider(customer, CloudType.onprem);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    InstanceType i = InstanceType.upsert(p.code, "type.small", 10, 5.5, new InstanceType.InstanceTypeDetails());
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    UserIntent userIntent = getTestUserIntent(r, p, i, 5);
    userIntent.providerType = CloudType.onprem;
    taskParams.upsertPrimaryCluster(userIntent, null);

    taskParams.nodeDetailsSet = new HashSet<NodeDetails>();

    for (int k = 0; k < 4; ++k) {
      NodeInstanceFormData.NodeInstanceData details = new NodeInstanceFormData.NodeInstanceData();
      details.ip = "10.255.67." + i;
      details.region = r.code;
      details.zone = az1.code;
      details.instanceType = "test_instance_type";
      details.nodeName = "test_name";
      NodeInstance.create(az1.uuid, details);
    }

    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    topJson.put("currentClusterType", "PRIMARY");
    topJson.put("clusterOperation", "CREATE");
    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);

    assertBadRequest(result, "Invalid Node/AZ combination for given instance type type.small");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testOnPremConfigureCreateInvalidAZNodeComboNonEmptyNodeDetailsSet() {
    Provider p = ModelFactory.newProvider(customer, CloudType.onprem);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone az2 = AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    List<AvailabilityZone> azList = new ArrayList<AvailabilityZone>();
    azList.add(az1);
    azList.add(az2);

    InstanceType i = InstanceType.upsert(p.code, "type.small", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams taskParams = setupOnPremTestData(6, p, r, azList);

    UserIntent userIntent = getTestUserIntent(r, p, i, 5);
    userIntent.providerType = CloudType.onprem;
    userIntent.instanceType = "type.small";
    taskParams.upsertPrimaryCluster(userIntent, null);
    taskParams.nodeDetailsSet = new HashSet<NodeDetails>();
    Cluster primaryCluster = taskParams.getPrimaryCluster();

    updateUniverseDefinition(taskParams, customer.getCustomerId(), primaryCluster.uuid,
        CREATE);

    // Set placement info with number of nodes valid but
    for (int k = 0; k < 5; k++) {
      NodeDetails nd= new NodeDetails();
      nd.state = NodeDetails.NodeState.ToBeAdded;
      nd.azUuid = az1.uuid;
      nd.placementUuid = primaryCluster.uuid;
      taskParams.nodeDetailsSet.add(nd);
    }

    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    topJson.put("currentClusterType", "PRIMARY");
    topJson.put("clusterOperation", "CREATE");

    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);
    assertBadRequest(result, "Invalid Node/AZ combination for given instance type type.small");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testOnPremConfigureValidAZNodeComboNonEmptyNodeDetailsSet() {
    Provider p = ModelFactory.newProvider(customer, CloudType.onprem);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");

    List<AvailabilityZone> azList = new ArrayList<>();
    azList.add(az1);

    UniverseDefinitionTaskParams taskParams = setupOnPremTestData(6, p, r, azList);

    Cluster primaryCluster = taskParams.getPrimaryCluster();

    updateUniverseDefinition(taskParams, customer.getCustomerId(), primaryCluster.uuid, CREATE);

    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    topJson.put("clusterOperation", "CREATE");
    topJson.put("currentClusterType", "PRIMARY");
    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);
    assertOk(result);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testConfigureEditOnPremInvalidNodeAZCombo() {
    Provider p = ModelFactory.newProvider(customer, CloudType.onprem);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone az2 = AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");

    List<AvailabilityZone> azList = new ArrayList<AvailabilityZone>();
    azList.add(az1);
    azList.add(az2);

    UniverseDefinitionTaskParams taskParams = setupOnPremTestData(6, p, r, azList);

    InstanceType i = InstanceType.upsert(p.code, "type.small", 10, 5.5, new InstanceType.InstanceTypeDetails());

    taskParams.nodePrefix = "test_uni";
    UserIntent userIntent = getTestUserIntent(r, p, i, 5);
    userIntent.providerType = CloudType.onprem;
    userIntent.instanceType = "type.small";
    taskParams.upsertPrimaryCluster(userIntent, null);
    Cluster primaryCluster = taskParams.getPrimaryCluster();
    updateUniverseDefinition(taskParams, customer.getCustomerId(), primaryCluster.uuid, CREATE);

    // Set the nodes state to inUse
    int k = 0;
    for (NodeInstance ni: NodeInstance.listByProvider(p.uuid)){
      if (k < 5) {
        k++;
        ni.inUse = true;
        ni.save();
      } else {
        break;
      }
    }

    // Simulate a running universe by setting existing nodes to Live state.
    for (NodeDetails nd: taskParams.nodeDetailsSet) {
      nd.state = NodeDetails.NodeState.Live;
    }

    // Set placement info with addition of nodes that is more than what has been configured
    for (int m = 0; m < 7; m++) {
      NodeDetails nd= new NodeDetails();
      nd.state = NodeDetails.NodeState.ToBeAdded;
      nd.azUuid = az1.uuid;
      nd.placementUuid = primaryCluster.uuid;
      taskParams.nodeDetailsSet.add(nd);
    }

    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    topJson.put("clusterOperation", "EDIT");
    topJson.put("currentClusterType", "PRIMARY");
    String url = "/api/customers/" + customer.uuid + "/universe_configure";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, topJson);
    assertBadRequest(result, "Invalid Node/AZ combination for given instance type type.small");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCreateUniverseEncryptionAtRestNoKMSConfig() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
            Matchers.any(UniverseDefinitionTaskParams.class)))
            .thenReturn(fakeTaskUUID);
    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());

    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    ObjectNode bodyJson = (ObjectNode) Json.toJson(taskParams);

    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "encryptionAtRestUniverse")
      .put("instanceType", i.getInstanceTypeCode())
      .put("enableNodeToNodeEncrypt", true)
      .put("enableClientToNodeEncrypt", true)
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    ArrayNode nodeDetails = Json.newArray().add(Json.newObject().put("nodeName", "testing-1"));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetails);
    bodyJson.put("nodePrefix", "demo-node");

    // TODO: (Daniel) - Add encryptionAtRestConfig to the payload to actually
    //  test what this unit test says it is testing for

    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);

    // Check that the encryption key file was not created in file system
    File key = new File("/tmp/certs/" +
            customer.uuid.toString() +
            "/universe." +
            json.get("universeUUID").asText() +
            "-1.key"
    );
    assertTrue(!key.exists());
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    ArgumentCaptor<UniverseTaskParams> argCaptor = ArgumentCaptor
        .forClass(UniverseTaskParams.class);
    verify(mockCommissioner).submit(eq(TaskType.CreateUniverse), argCaptor.capture());

    // The KMS provider service should not begin to make any requests since there is no KMS config
    verify(mockApiHelper, times(0)).postRequest(any(String.class), any(JsonNode.class), anyMap());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testCreateUniverseEncryptionAtRestWithKMSConfigExists() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
            Matchers.any(UniverseDefinitionTaskParams.class)))
            .thenReturn(fakeTaskUUID);
    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    ObjectNode bodyJson = (ObjectNode) Json.toJson(taskParams);

    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "encryptionAtRestUniverse")
      .put("instanceType", i.getInstanceTypeCode())
      .put("enableNodeToNodeEncrypt", true)
      .put("enableClientToNodeEncrypt", true)
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    ArrayNode nodeDetails = Json.newArray().add(Json.newObject().put("nodeName", "testing-1"));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetails);
    bodyJson.put("nodePrefix", "demo-node");
    bodyJson.put(
            "encryptionAtRestConfig",
            Json.newObject()
                    .put("configUUID", kmsConfig.configUUID.toString())
                    .put("key_op", "ENABLE")
    );
    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    JsonNode json = Json.parse(contentAsString(result));
    assertOk(result);

    // Check that the encryption was enabled successfully
    JsonNode userIntent = json.get("universeDetails").get("clusters").get(0).get("userIntent");
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    ArgumentCaptor<UniverseTaskParams> argCaptor = ArgumentCaptor
        .forClass(UniverseTaskParams.class);
    verify(mockCommissioner).submit(eq(TaskType.CreateUniverse), argCaptor.capture());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseSetKey() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);

    // Create the universe with encryption enabled through SMARTKEY KMS provider
    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.create(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.create(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i = InstanceType
            .upsert(p.code, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams createTaskParams = new UniverseDefinitionTaskParams();
    ObjectNode createBodyJson = (ObjectNode) Json.toJson(createTaskParams);

    ObjectNode userIntentJson = Json.newObject()
      .put("universeName", "encryptionAtRestUniverse")
      .put("instanceType", i.getInstanceTypeCode())
      .put("enableNodeToNodeEncrypt", true)
      .put("enableClientToNodeEncrypt", true)
      .put("replicationFactor", 3)
      .put("numNodes", 3)
      .put("provider", p.uuid.toString())
      .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray = Json.newArray()
            .add(Json.newObject().set("userIntent", userIntentJson));
    ArrayNode nodeDetails = Json.newArray().add(Json.newObject().put("nodeName", "testing-1"));
    createBodyJson.set("clusters", clustersJsonArray);
    createBodyJson.set("nodeDetailsSet", nodeDetails);
    createBodyJson.put("nodePrefix", "demo-node");

    String createUrl = "/api/customers/" + customer.uuid + "/universes";

    final ArrayNode keyOps = Json.newArray()
            .add("EXPORT")
            .add("APPMANAGEABLE");
    ObjectNode createPayload = Json.newObject()
            .put("name", "some name")
            .put("obj_type", "AES")
            .put("key_size", "256");
    createPayload.set("key_ops", keyOps);

    Result createResult = doRequestWithAuthTokenAndBody(
            "POST",
            createUrl,
            authToken,
            createBodyJson
    );
    assertOk(createResult);
    JsonNode json = Json.parse(contentAsString(createResult));
    assertNotNull(json.get("universeUUID"));
    String testUniUUID = json.get("universeUUID").asText();

    fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(), any())).thenReturn(fakeTaskUUID);
    // Rotate the universe key
    EncryptionAtRestKeyParams taskParams = new EncryptionAtRestKeyParams();
    ObjectNode bodyJson = (ObjectNode) Json.toJson(taskParams);
    bodyJson.put("kmsConfigUUID", kmsConfig.configUUID.toString());
    bodyJson.put("algorithm", "AES");
    bodyJson.put("key_size", "256");
    bodyJson.put("key_op", "ENABLE");
    String url = "/api/customers/" + customer.uuid + "/universes/" + testUniUUID + "/set_key";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertOk(result);
    ArgumentCaptor<UniverseTaskParams> argCaptor = ArgumentCaptor
            .forClass(UniverseTaskParams.class);
    verify(mockCommissioner).submit(eq(TaskType.SetUniverseKey), argCaptor.capture());
    assertAuditEntry(2, customer.uuid);
  }

  @Test
  public void testRunQueryWithInvalidUniverse() {
    // Setting platform type as correct.
    when(mockAppConfig.getString("yb.mode", "PLATFORM")).thenReturn("OSS");
    // Setting insecure mode.
    ConfigHelper configHelper = new ConfigHelper();
    configHelper.loadConfigToDB(ConfigHelper.ConfigType.Security,
        ImmutableMap.of("level", "insecure"));

    Customer c2 = ModelFactory.testCustomer("tc2", "Test Customer 2");
    Universe u = createUniverse(c2.getCustomerId());
    ObjectNode bodyJson = Json.newObject();
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
        "/run_query";
    Http.RequestBuilder request = Helpers.fakeRequest("POST", url).header("X-AUTH-TOKEN", authToken)
        .bodyJson(bodyJson).header("Origin", "https://" + UniverseController.LEARN_DOMAIN_NAME);
    Result result = Helpers.route(request);
    assertBadRequest(result, String.format("Universe UUID: %s doesn't belong to Customer UUID: %s",
        u.universeUUID, customer.uuid));
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testRunInShellWithInvalidUniverse() {
    Customer c2 = ModelFactory.testCustomer("tc2", "Test Customer 2");
    Universe u = createUniverse(c2.getCustomerId());
    ObjectNode bodyJson = Json.newObject();
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
        "/run_in_shell";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, UniverseController.DEPRECATED);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testRunInShellWithoutInsecureMode() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    ObjectNode bodyJson = Json.newObject()
        .put("query", "select * from product limit 1")
        .put("db_name", "demo");
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
        "/run_in_shell";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, UniverseController.DEPRECATED);
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testRunInShellWithInsecureMode() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdaterWithYSQLNodes(true));

    ConfigHelper configHelper = new ConfigHelper();
    configHelper.loadConfigToDB(ConfigHelper.ConfigType.Security,
        ImmutableMap.of("level", "insecure"));

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID
        + "/run_in_shell";
    for (RunInShellFormData.ShellType shellType : RunInShellFormData.ShellType.values()) {
      ObjectNode bodyJson = Json.newObject().put("db_name", "demo")
          .put("shell_type", shellType.name()).put("command", "select * from product limit 1");
      Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
      assertBadRequest(result, UniverseController.DEPRECATED);
      assertAuditEntry(0, customer.uuid);
    }
  }

  @Test
  // @formatter:off
  @Parameters({
                // cloud customer, normal username
                "true,  foo, foo, baz, baz, true, true,",
                // not cloud customer
                "false, foo, foo, baz, baz, false, false, Invalid Customer type",
                // cloud customer, double quotes in username
                "true,  foo, foo, ba\"z, baz, false, false, Invalid username",
                // cloud customer, username surrounded by double quotes
                "true,  foo, foo, \"baz\", baz, true, true,",
                // cloud customer, username surrounded by double quotes + double quotes inside
                "true,  foo, foo, \"ba\"z\", baz, false, false, Invalid username",
                // cloud customer, backslash in username
                "true,  foo, foo, ba\\z, baz, true, true,",
                // cloud customer, only YSQL user
                "true, foo,, baz, baz, true, false,",
                // cloud customer, only YCQL user
                "true,, foo, baz, baz, false, true,",
                // cloud customer, neither YSQL nor YCQL user
                "true,,, baz, baz, false, false, Need to provide YSQL and/or YCQL username.",
              })
  // @formatter:on
  public void testCreateUserInDB(boolean isCloudCustomer, String ysqlAdminUsername,
      String ycqlAdminUsername, String username, String password, boolean ysqlProcessed,
      boolean ycqlProcessed, String responseError) {
    Universe u = createUniverse(customer.getCustomerId());
    if (isCloudCustomer) {
      customer.code = "cloud";
    }
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    ObjectNode bodyJson = Json.newObject()
        .put("ycqlAdminUsername", ycqlAdminUsername)
        .put("ysqlAdminUsername", ysqlAdminUsername)
        .put("ycqlAdminPassword", "bar")
        .put("ysqlAdminPassword", "bar")
        .put("dbName", "test")
        .put("username", username)
        .put("password", password);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
        "/create_db_credentials";
    when(mockYsqlQueryExecutor.executeQuery(any(), any(), any(), any()))
        .thenReturn(Json.newObject().put("foo", "bar"));
    when(mockYcqlQueryExecutor.executeQuery(any(), any(), any(), any(), any()))
        .thenReturn(Json.newObject().put("foo", "bar"));
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    ArgumentCaptor<Universe> universe = ArgumentCaptor.forClass(Universe.class);
    ArgumentCaptor<RunQueryFormData> info = ArgumentCaptor.forClass(RunQueryFormData.class);
    ArgumentCaptor<Boolean> auth = ArgumentCaptor.forClass(Boolean.class);
    ArgumentCaptor<String> usernameCaptor = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> passwordCaptor = ArgumentCaptor.forClass(String.class);
    Mockito.verify(mockYcqlQueryExecutor, times(ycqlProcessed ? 1 : 0)).executeQuery(
        universe.capture(), info.capture(), auth.capture(), usernameCaptor.capture(),
        passwordCaptor.capture());
    Mockito.verify(mockYsqlQueryExecutor, times(ysqlProcessed ? 1 : 0)).executeQuery(
        universe.capture(), info.capture(), usernameCaptor.capture(), passwordCaptor.capture());
    if (ycqlProcessed || ysqlProcessed) {
      assertOk(result);
    } else {
      assertErrorResponse(result, responseError);
    }
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  // @formatter:off
  @Parameters({
                // cloud customer, normal username
                "true,  baz, baz, baz, baz, true, true,",
                // not cloud customer
                "false, baz, baz, baz, baz, false, false, Invalid Customer type",
                // cloud customer, double quotes in username
                "true,  ba\"z, baz, baz, baz, false, false, Invalid username",
                // cloud customer, usernames surrounded by double quotes
                "true,  \"baz\", baz, \"baz\", baz, true, true,",
                // cloud customer, double quotes in username which surrounded by double quotes
                "true,  \"ba\"z\", baz, baz, baz, false, false, Invalid username",
                // cloud customer, backslash in username
                "true,  ba\\z, baz, baz, baz, true, true,",
                // cloud customer, only YSQL user
                "true,  baz, baz,,, true, false,",
                // cloud customer, only YSQL user, YCQL user is set as ""
                "true,  baz, baz, \"\", baz, true, false,",
                // cloud customer, only YCQL user
                "true,,, baz, baz, false, true,",
                // cloud customer, only YCQL user, YSQL user is set as ""
                "true, \"\", baz, baz, baz, false, true,",
                // cloud customer, neither YSQL nor YCQL user
                "true,,,,, false, false, Need to provide YSQL and/or YCQL username.",
              })
  // @formatter:on
  public void testSetDatabaseCredentials(boolean isCloudCustomer, String ysqlAdminUsername,
      String ysqlPassword, String ycqlAdminUsername, String ycqlPassword, boolean ysqlProcessed,
      boolean ycqlProcessed, String responseError) {
    Universe u = createUniverse(customer.getCustomerId());
    if (isCloudCustomer) {
      customer.code = "cloud";
    }
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    ObjectNode bodyJson = Json.newObject()
        .put("ycqlAdminUsername", ycqlAdminUsername)
        .put("ysqlAdminUsername", ysqlAdminUsername)
        .put("ycqlCurrAdminPassword", "foo")
        .put("ysqlCurrAdminPassword", "foo")
        .put("ycqlAdminPassword", ycqlPassword)
        .put("ysqlAdminPassword", ysqlPassword)
        .put("dbName", "test");
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID +
        "/update_db_credentials";
    when(mockYsqlQueryExecutor.executeQuery(any(), any(), any(), any()))
        .thenReturn(Json.newObject().put("foo", "bar"));
    when(mockYcqlQueryExecutor.executeQuery(any(), any(), any(), any(), any()))
        .thenReturn(Json.newObject().put("foo", "bar"));
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    ArgumentCaptor<Universe> universe = ArgumentCaptor.forClass(Universe.class);
    ArgumentCaptor<RunQueryFormData> info = ArgumentCaptor.forClass(RunQueryFormData.class);
    ArgumentCaptor<Boolean> auth = ArgumentCaptor.forClass(Boolean.class);
    ArgumentCaptor<String> usernameCaptor = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> passwordCaptor = ArgumentCaptor.forClass(String.class);
    Mockito.verify(mockYcqlQueryExecutor, times(ycqlProcessed ? 1 : 0)).executeQuery(
        universe.capture(), info.capture(), auth.capture(), usernameCaptor.capture(),
        passwordCaptor.capture());
    Mockito.verify(mockYsqlQueryExecutor, times(ysqlProcessed ? 1 : 0)).executeQuery(
        universe.capture(), info.capture(), usernameCaptor.capture(), passwordCaptor.capture());
    if (ycqlProcessed || ysqlProcessed) {
      assertOk(result);
    } else {
      assertErrorResponse(result, responseError);
    }
    assertAuditEntry(0, customer.uuid);
  }

  private void setupDiskUpdateTest(int diskSize, String instanceType,
                                   PublicCloudConstants.StorageType storageType,
                                   Universe u) {

    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = new UniverseDefinitionTaskParams();
        UserIntent userIntent = new UserIntent();
        userIntent.instanceType = instanceType;
        userIntent.providerType = CloudType.aws;
        DeviceInfo di = new DeviceInfo();
        di.volumeSize = diskSize;
        di.numVolumes = 2;
        di.storageType = storageType;
        userIntent.deviceInfo = di;
        universeDetails.upsertPrimaryCluster(userIntent, null);
        universe.setUniverseDetails(universeDetails);
      }
    };
    // Save the updates to the universe.
    Universe.saveDetails(u.universeUUID, updater);
  }

  @Test
  public void testExpandDiskSizeFailureInvalidSize() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    setupDiskUpdateTest(100, "c4.xlarge", PublicCloudConstants.StorageType.GP2, u);
    u = Universe.get(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 50);

    String url = "/api/customers/" + customer.uuid + "/universes/" +
                 u.universeUUID + "/disk_update";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, "Size can only be increased.");
  }

  @Test
  public void testExpandDiskSizeFailureInvalidStorage() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    setupDiskUpdateTest(100, "c4.xlarge", PublicCloudConstants.StorageType.Scratch, u);
    u = Universe.get(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 150);

    String url = "/api/customers/" + customer.uuid + "/universes/" +
                 u.universeUUID + "/disk_update";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, "Scratch type disk cannot be modified.");
  }

  @Test
  public void testExpandDiskSizeFailureInvalidInstance() {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    setupDiskUpdateTest(100, "i3.xlarge", PublicCloudConstants.StorageType.GP2, u);
    u = Universe.get(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 150);

    String url = "/api/customers/" + customer.uuid + "/universes/" +
                 u.universeUUID + "/disk_update";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertBadRequest(result, "Cannot modify instance volumes.");
  }

  @Test
  public void testExpandDiskSizeSuccess() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
            Matchers.any(UniverseDefinitionTaskParams.class)))
            .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();
    setupDiskUpdateTest(100, "c4.xlarge", PublicCloudConstants.StorageType.GP2, u);
    u = Universe.get(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 150);

    String url = "/api/customers/" + customer.uuid + "/universes/" +
                 u.universeUUID + "/disk_update";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertOk(result);
    ArgumentCaptor<UniverseTaskParams> argCaptor = ArgumentCaptor
            .forClass(UniverseTaskParams.class);
    verify(mockCommissioner).submit(eq(TaskType.UpdateDiskSize), argCaptor.capture());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithDisabledYedis() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(Matchers.any(TaskType.class),
        Matchers.any(UniverseDefinitionTaskParams.class))).thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ 1", "subnet-1");
    InstanceType i = InstanceType.upsert(p.code, "c3.xlarge", 10, 5.5,
        new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson = Json.newObject().put("universeName", "SingleUserUniverse")
        .put("instanceType", i.getInstanceTypeCode()).put("replicationFactor", 3).put("numNodes", 3)
        .put("enableYEDIS", "false").put("provider", p.uuid.toString());
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    userIntentJson.put("accessKeyCode", accessKeyCode);
    ArrayNode clustersJsonArray = Json.newArray()
        .add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertOk(result);

    JsonNode json = Json.parse(contentAsString(result));
    assertNotNull(json.get("universeUUID"));
    assertNotNull(json.get("universeConfig"));

    JsonNode universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    JsonNode clustersJson = universeDetails.get("clusters");
    assertNotNull(clustersJson);
    JsonNode primaryClusterJson = clustersJson.get(0);
    assertNotNull(primaryClusterJson);
    JsonNode userIntentJsonNode = primaryClusterJson.get("userIntent");
    assertNotNull(userIntentJsonNode);

    assertEquals("false", userIntentJsonNode.get("enableYEDIS").toString());
  }

  @Test
  // @formatter:off
  @Parameters({
               // not insecure, wrong origin, wrong ybmode => failure
               "false,,, false",
               // insecure, wrong origin, wrong ybmode => failure
               "true,,, false",
               // insecure, correct origin, wrong ybmode => failure
               "true, https://learn.yugabyte.com,, false",
               // insecure, correct origin, wrong ybmode => failure
               "true, https://learn.yugabyte.com, PLATFORM, false",
               // insecure, correct origin, correct ybmode => success
               "true, https://learn.yugabyte.com, OSS, true",
              })
  // @formatter:on
  public void testRunQuery_ValidPlatform(boolean insecure, String origin, String ybmode,
      boolean isGoodResult) {
    Universe u = createUniverse(customer.getCustomerId());
    customer.addUniverseUUID(u.universeUUID);
    customer.save();

    if (insecure) {
      ConfigHelper configHelper = new ConfigHelper();
      configHelper.loadConfigToDB(ConfigHelper.ConfigType.Security,
          ImmutableMap.of("level", "insecure"));
    }
    when(mockAppConfig.getString("yb.mode", "PLATFORM")).thenReturn(ybmode == null ? "" : ybmode);

    ObjectNode bodyJson = Json.newObject().put("query", "select * from product limit 1")
        .put("db_name", "demo");
    when(mockYsqlQueryExecutor.executeQuery(any(), any()))
        .thenReturn(Json.newObject().put("foo", "bar"));

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/run_query";
    Http.RequestBuilder request = Helpers.fakeRequest("POST", url).header("X-AUTH-TOKEN", authToken)
        .bodyJson(bodyJson);
    if (!StringUtils.isEmpty(origin)) {
      request = request.header("Origin", origin);
    }
    Result result = Helpers.route(request);

    JsonNode json = Json.parse(contentAsString(result));
    if (isGoodResult) {
      assertOk(result);
      assertEquals("bar", json.get("foo").asText());
      assertAuditEntry(1, customer.uuid);
    } else {
      assertBadRequest(result, UniverseController.RUN_QUERY_ISNT_ALLOWED);
      assertAuditEntry(0, customer.uuid);
    }
  }

  @Test
  public void testRemoveEnclosingDoubleQuotes() {
    // Removes, happy path.
    assertEquals("baz", UniverseController.removeEnclosingDoubleQuotes("\"baz\""));
    // Doesn't remove single internal quotes
    assertEquals("ba\"z", UniverseController.removeEnclosingDoubleQuotes("\"ba\"z\""));
    // Doesn't remove pair of internal quotes.
    assertEquals("a\"ba\"z", UniverseController.removeEnclosingDoubleQuotes("a\"ba\"z"));
    // Doesn't remove only starting quotes.
    assertEquals("\"baz", UniverseController.removeEnclosingDoubleQuotes("\"baz"));
    // Doesn't remove only ending quotes.
    assertEquals("baz\"", UniverseController.removeEnclosingDoubleQuotes("baz\""));
    // Empty string
    assertEquals("", UniverseController.removeEnclosingDoubleQuotes(""));
    // Null string
    assertEquals(null, UniverseController.removeEnclosingDoubleQuotes(null));
  }
}
