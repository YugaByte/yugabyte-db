// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableMap;
import com.google.common.collect.Sets;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.cloud.UniverseResourceDetails;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.NodeActionType;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.models.helpers.CloudSpecificInfo;
import com.yugabyte.yw.models.helpers.DeviceInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

import junitparams.JUnitParamsRunner;
import junitparams.Parameters;

import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.google.common.util.concurrent.ThreadFactoryBuilder;

import org.apache.commons.lang3.StringUtils;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import play.libs.Json;

import java.util.*;
import java.util.concurrent.*;

import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.hamcrest.CoreMatchers.*;
import static org.junit.Assert.*;
import static org.mockito.Matchers.anyList;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.spy;
import static org.mockito.Mockito.when;

@RunWith(JUnitParamsRunner.class)
public class UniverseTest extends FakeDBApplication {
  private Provider defaultProvider;
  private Customer defaultCustomer;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
  }

  @Test
  public void testCreate() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    assertNotNull(u);
    assertThat(u.universeUUID, is(allOf(notNullValue(), equalTo(u.universeUUID))));
    assertThat(u.version, is(allOf(notNullValue(), equalTo(1))));
    assertThat(u.name, is(allOf(notNullValue(), equalTo("Test Universe"))));
    assertThat(u.getUniverseDetails(), is(notNullValue()));
  }

  @Test
  public void testConfig() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    assertNotNull(u);
    Map<String, String> config = new HashMap<>();
    config.put(Universe.TAKE_BACKUPS, "true");
    u.setConfig(config);
    assertEquals(config, u.getConfig());
  }

  @Test
  public void testGetSingleUniverse() {
    Universe newUniverse = createUniverse(defaultCustomer.getCustomerId());
    assertNotNull(newUniverse);
    Universe fetchedUniverse = Universe.get(newUniverse.universeUUID);
    assertNotNull(fetchedUniverse);
    assertEquals(fetchedUniverse, newUniverse);
  }

  @Test
  public void testCheckIfUniverseExists() {
    Universe newUniverse = createUniverse(defaultCustomer.getCustomerId());
    assertNotNull(newUniverse);
    assertThat(Universe.checkIfUniverseExists("Test Universe"), equalTo(true));
    assertThat(Universe.checkIfUniverseExists("Fake Universe"), equalTo(false));
  }

  @Test
  public void testGetMultipleUniverse() {
    Universe u1 = createUniverse("Universe1", defaultCustomer.getCustomerId());
    Universe u2 = createUniverse("Universe2", defaultCustomer.getCustomerId());
    Universe u3 = createUniverse("Universe3", defaultCustomer.getCustomerId());
    Set<UUID> uuids = Sets.newHashSet(u1.universeUUID, u2.universeUUID, u3.universeUUID);

    Set<Universe> universes = Universe.get(uuids);
    assertNotNull(universes);
    assertEquals(universes.size(), 3);
  }

  @Test(expected = RuntimeException.class)
  public void testGetUnknownUniverse() {
    UUID unknownUUID = UUID.randomUUID();
    Universe u = Universe.get(unknownUUID);
  }

  @Test
  public void testParallelSaveDetails() {
    int numNodes = 100;
    ThreadFactory namedThreadFactory =
        new ThreadFactoryBuilder().setNameFormat("TaskPool-%d").build();
    ThreadPoolExecutor executor =
        new ThreadPoolExecutor(numNodes, numNodes, 60L, TimeUnit.SECONDS,
                               new LinkedBlockingQueue<Runnable>(), namedThreadFactory);
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    assertEquals(0, u.getNodes().size());
    for (int i = 0; i < numNodes; i++) {
      SaveNode sn = new SaveNode(u.universeUUID, i);
      executor.execute(sn);
    }
    executor.shutdown();
    try {
      executor.awaitTermination(120, TimeUnit.SECONDS);
    } catch (InterruptedException e1) { }
    Universe updUniv = Universe.get(u.universeUUID);
    assertEquals(numNodes, updUniv.getNodes().size());
    assertEquals(numNodes + 1, updUniv.version);
  }

  @Test
  public void testSaveDetails() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        universeDetails = new UniverseDefinitionTaskParams();
        UserIntent userIntent = new UserIntent();

        // Create some subnets.
        List<String> subnets = new ArrayList<String>();
        subnets.add("subnet-1");
        subnets.add("subnet-2");
        subnets.add("subnet-3");

        // Add a desired number of nodes.
        userIntent.numNodes = 5;
        universeDetails.nodeDetailsSet = new HashSet<NodeDetails>();
        for (int idx = 1; idx <= userIntent.numNodes; idx++) {
          NodeDetails node = new NodeDetails();
          node.nodeName = "host-n" + idx;
          node.cloudInfo = new CloudSpecificInfo();
          node.cloudInfo.cloud = "aws";
          node.cloudInfo.az = "az-" + idx;
          node.cloudInfo.region = "test-region";
          node.cloudInfo.subnet_id = subnets.get(idx % subnets.size());
          node.cloudInfo.private_ip = "host-n" + idx;
          node.state = NodeDetails.NodeState.Live;
          node.isTserver = true;
          if (idx <= 3) {
            node.isMaster = true;
          }
          node.nodeIdx = idx;
          universeDetails.nodeDetailsSet.add(node);
        }
        universeDetails.upsertPrimaryCluster(userIntent, null);
        universe.setUniverseDetails(universeDetails);
      }
    };
    u = Universe.saveDetails(u.universeUUID, updater);

    int nodeIdx;
    for (NodeDetails node : u.getMasters()) {
      assertTrue(node.isMaster);
      assertNotNull(node.nodeName);
      nodeIdx = Character.getNumericValue(node.nodeName.charAt(node.nodeName.length() - 1));
      assertTrue(nodeIdx <= 3);
    }

    for (NodeDetails node : u.getTServers()) {
      assertTrue(node.isTserver);
      assertNotNull(node.nodeName);
      nodeIdx = Character.getNumericValue(node.nodeName.charAt(node.nodeName.length() - 1));
      assertTrue(nodeIdx <= 5);
    }

    assertTrue(u.getTServers().size() > u.getMasters().size());
    assertEquals(u.getMasters().size(), 3);
    assertEquals(u.getTServers().size(), 5);
  }

  @Test
  public void testVerifyIsTrue() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    List<NodeDetails> masters = new LinkedList<>();
    NodeDetails mockNode1 = mock(NodeDetails.class);
    masters.add(mockNode1);
    NodeDetails mockNode2 = mock(NodeDetails.class);
    masters.add(mockNode2);
    when(mockNode1.isQueryable()).thenReturn(true);
    when(mockNode2.isQueryable()).thenReturn(true);
    assertTrue(u.verifyMastersAreQueryable(masters));
  }

  @Test
  public void testMastersListEmptyVerifyIsFalse() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    assertFalse(u.verifyMastersAreQueryable(null));
    List<NodeDetails> masters = new LinkedList<>();
    assertFalse(u.verifyMastersAreQueryable(masters));
  }

  @Test
  public void testMastersInBadStateVerifyIsFalse() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    List<NodeDetails> masters = new LinkedList<>();
    NodeDetails mockNode1 = mock(NodeDetails.class);
    masters.add(mockNode1);
    NodeDetails mockNode2 = mock(NodeDetails.class);
    masters.add(mockNode2);
    when(mockNode1.isQueryable()).thenReturn(false);
    when(mockNode2.isQueryable()).thenReturn(true);
    assertFalse(u.verifyMastersAreQueryable(masters));
    when(mockNode1.isQueryable()).thenReturn(true);
    when(mockNode2.isQueryable()).thenReturn(false);
    assertFalse(u.verifyMastersAreQueryable(masters));
  }

  @Test
  public void testGetMasterAddresses() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());

    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        universeDetails = new UniverseDefinitionTaskParams();
        UserIntent userIntent = new UserIntent();

        // Add a desired number of nodes.
        userIntent.numNodes = 3;
        universeDetails.nodeDetailsSet = new HashSet<NodeDetails>();
        for (int idx = 1; idx <= userIntent.numNodes; idx++) {
          NodeDetails node = new NodeDetails();
          node.nodeName = "host-n" + idx;
          node.cloudInfo = new CloudSpecificInfo();
          node.cloudInfo.cloud = "aws";
          node.cloudInfo.az = "az-" + idx;
          node.cloudInfo.region = "test-region";
          node.cloudInfo.subnet_id = "subnet-" + idx;
          node.cloudInfo.private_ip = "host-n" + idx;
          node.state = NodeDetails.NodeState.Live;
          node.isTserver = true;
          if (idx <= 3) {
            node.isMaster = true;
          }
          node.nodeIdx = idx;
          universeDetails.upsertPrimaryCluster(userIntent, null);
          universeDetails.nodeDetailsSet.add(node);
        }
        universe.setUniverseDetails(universeDetails);
      }
    };
    u = Universe.saveDetails(u.universeUUID, updater);
    String masterAddrs = u.getMasterAddresses();
    assertNotNull(masterAddrs);
    for (int idx = 1; idx <= 3; idx++) {
      assertThat(masterAddrs, containsString("host-n" + idx));
    }
  }

  @Test
  public void testGetMasterAddressesFails() {
    Universe u = spy(createUniverse(defaultCustomer.getCustomerId()));
    when(u.verifyMastersAreQueryable(anyList())).thenReturn(false);
    assertEquals("", u.getMasterAddresses());
  }

  @Test
  public void testToJSONSuccess() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    Map<String, String> config = new HashMap<>();
    config.put(Universe.TAKE_BACKUPS, "true");
    u.setConfig(config);

    // Create regions
    Region r1 = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    Region r2 = Region.create(defaultProvider, "region-2", "Region 2", "yb-image-1");
    Region r3 = Region.create(defaultProvider, "region-3", "Region 3", "yb-image-1");
    AvailabilityZone.create(r1, "az-1", "AZ 1", "subnet-1");
    AvailabilityZone.create(r2, "az-2", "AZ 2", "subnet-2");
    AvailabilityZone.create(r3, "az-3", "AZ 3", "subnet-3");
    List<UUID> regionList = new ArrayList<>();
    regionList.add(r1.uuid);
    regionList.add(r2.uuid);
    regionList.add(r3.uuid);

    // Add non-EBS instance type with price to each region
    String instanceType = "c3.xlarge";
    double instancePrice = 0.1;
    InstanceType.upsert(defaultProvider.code, instanceType, 1, 20.0, null);
    PriceComponent.PriceDetails instanceDetails = new PriceComponent.PriceDetails();
    instanceDetails.pricePerHour = instancePrice;
    PriceComponent.upsert(defaultProvider.code, r1.code, instanceType, instanceDetails);
    PriceComponent.upsert(defaultProvider.code, r2.code, instanceType, instanceDetails);
    PriceComponent.upsert(defaultProvider.code, r3.code, instanceType, instanceDetails);

    // Create userIntent
    UserIntent userIntent = new UserIntent();
    userIntent.replicationFactor = 3;
    userIntent.regionList = regionList;
    userIntent.instanceType = instanceType;
    userIntent.provider = defaultProvider.uuid.toString();
    userIntent.deviceInfo = new DeviceInfo();
    userIntent.deviceInfo.storageType = PublicCloudConstants.StorageType.IO1;
    userIntent.deviceInfo.numVolumes = 2;
    userIntent.deviceInfo.diskIops = 1000;
    userIntent.deviceInfo.volumeSize = 100;

    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater(userIntent));

    JsonNode universeJson = u.toJson();
    assertThat(universeJson.get("universeUUID").asText(), allOf(notNullValue(),
        equalTo(u.universeUUID.toString())));
    JsonNode resources = Json.toJson(UniverseResourceDetails.create(u.getNodes(),
        u.getUniverseDetails()));
    assertThat(universeJson.get("resources").asText(), allOf(notNullValue(),
        equalTo(resources.asText())));
    JsonNode universeConfig = universeJson.get("universeConfig");
    assertEquals(universeConfig.toString(), "{\"takeBackups\":\"true\"}");
    JsonNode clustersListJson = universeJson.get("universeDetails").get("clusters");
    assertThat(clustersListJson, notNullValue());
    assertTrue(clustersListJson.isArray());
    assertEquals(1, clustersListJson.size());
    JsonNode clusterJson = clustersListJson.get(0);
    JsonNode userIntentJson = clusterJson.get("userIntent");
    assertTrue(userIntentJson.get("regionList").isArray());
    assertEquals(3, userIntentJson.get("regionList").size());
    JsonNode masterGFlags = userIntentJson.get("masterGFlags");
    assertThat(masterGFlags, is(notNullValue()));
    assertTrue(masterGFlags.isObject());

    JsonNode providerNode = userIntentJson.get("provider");
    assertThat(providerNode, notNullValue());
    assertThat(providerNode.asText(), allOf(notNullValue(),
        equalTo(defaultProvider.uuid.toString())));

    JsonNode regionsNode = clusterJson.get("regions");
    assertThat(regionsNode, is(notNullValue()));
    assertTrue(regionsNode.isArray());
    assertEquals(3, regionsNode.size());
    assertNull(universeJson.get("dnsName"));
  }

  @Test
  public void testToJSONWithNullRegionList() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    UserIntent ui = u.getUniverseDetails().getPrimaryCluster().userIntent;
    ui.provider = Provider.get(defaultCustomer.uuid, Common.CloudType.aws).uuid.toString();
    u.getUniverseDetails().upsertPrimaryCluster(ui, null);

    JsonNode universeJson = u.toJson();
    assertThat(universeJson.get("universeUUID").asText(), allOf(notNullValue(),
        equalTo(u.universeUUID.toString())));
    JsonNode clusterJson = universeJson.get("universeDetails").get("clusters").get(0);
    assertTrue(clusterJson.get("userIntent").get("regionList").isNull());
    assertNull(clusterJson.get("regions"));
    assertNull(clusterJson.get("provider"));
  }

  @Test
  public void testToJSONWithNullGFlags() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    UserIntent userIntent = new UserIntent();
    userIntent.replicationFactor = 3;
    userIntent.regionList = new ArrayList<>();
    userIntent.masterGFlags = null;
    userIntent.provider = Provider.get(defaultCustomer.uuid, Common.CloudType.aws).uuid.toString();

    // SaveDetails in order to generate universeDetailsJson with null gflags
    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater(userIntent));

    // Update in-memory user intent so userDetails no longer has null gflags, but json still does
    UniverseDefinitionTaskParams udtp = u.getUniverseDetails();
    udtp.getPrimaryCluster().userIntent.masterGFlags = new HashMap<>();
    u.setUniverseDetails(udtp);

    // Verify returned json is generated from the non-json userDetails object
    JsonNode universeJson = u.toJson();
    assertThat(universeJson.get("universeUUID").asText(), allOf(notNullValue(),
        equalTo(u.universeUUID.toString())));
    JsonNode clusterJson = universeJson.get("universeDetails").get("clusters").get(0);
    JsonNode masterGFlags = clusterJson.get("userIntent").get("masterGFlags");
    assertThat(masterGFlags, is(notNullValue()));
    assertTrue(masterGFlags.isObject());
    JsonNode providerNode = universeJson.get("provider");
    assertNull(providerNode);
  }

  @Test
  public void testToJSONWithEmptyRegionList() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());

    UserIntent userIntent = new UserIntent();
    userIntent.replicationFactor = 3;
    userIntent.regionList = new ArrayList<>();
    userIntent.provider = Provider.get(defaultCustomer.uuid, Common.CloudType.aws).uuid.toString();

    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater(userIntent));

    JsonNode universeJson = u.toJson();
    assertThat(universeJson.get("universeUUID").asText(), allOf(notNullValue(), equalTo(u.universeUUID.toString())));
    JsonNode clusterJson = universeJson.get("universeDetails").get("clusters").get(0);
    assertTrue(clusterJson.get("userIntent").get("regionList").isArray());
    assertNull(clusterJson.get("regions"));
    assertNull(clusterJson.get("provider"));
  }

  @Test
  public void testToJSONOfGFlags() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    UserIntent ui = u.getUniverseDetails().getPrimaryCluster().userIntent;
    ui.provider = Provider.get(defaultCustomer.uuid, Common.CloudType.aws).uuid.toString();
    u.getUniverseDetails().upsertPrimaryCluster(ui, null);

    JsonNode universeJson = u.toJson();
    assertThat(universeJson.get("universeUUID").asText(),
               allOf(notNullValue(), equalTo(u.universeUUID.toString())));
    JsonNode clusterJson = universeJson.get("universeDetails").get("clusters").get(0);
    JsonNode masterGFlags = clusterJson.get("userIntent").get("masterGFlags");
    assertThat(masterGFlags, is(notNullValue()));
    assertEquals(0, masterGFlags.size());
  }

  @Test
  public void testFromJSONWithFlags() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    UserIntent userIntent = getBaseIntent();
    userIntent.masterGFlags = new HashMap<>();
    userIntent.masterGFlags.put("emulate_redis_responses", "false");
    taskParams.upsertPrimaryCluster(userIntent, null);
    JsonNode clusterJson = Json.toJson(taskParams).get("clusters").get(0);

    assertThat(clusterJson.get("userIntent").get("masterGFlags").get("emulate_redis_responses"), notNullValue());
  }

  @Test
  public void testAreTagsSame() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    UserIntent userIntent = getBaseIntent();
    userIntent.providerType = CloudType.aws;
    userIntent.instanceTags = ImmutableMap.of("Cust", "Test", "Dept", "Misc");
    Cluster cluster = taskParams.upsertPrimaryCluster(userIntent, null);

    UserIntent newUserIntent = getBaseIntent();
    newUserIntent.providerType = CloudType.aws;
    newUserIntent.instanceTags = ImmutableMap.of("Cust", "Test", "Dept", "Misc");
    Cluster newCluster = new Cluster(ClusterType.PRIMARY, newUserIntent);
    assertTrue(cluster.areTagsSame(newCluster));

    newUserIntent = getBaseIntent();
    newUserIntent.providerType = CloudType.aws;
    newCluster = new Cluster(ClusterType.PRIMARY, newUserIntent);
    newUserIntent.instanceTags = ImmutableMap.of("Cust", "Test");
    assertFalse(cluster.areTagsSame(newCluster));

    newUserIntent = getBaseIntent();
    newUserIntent.providerType = CloudType.aws;
    newCluster = new Cluster(ClusterType.PRIMARY, newUserIntent);
    assertFalse(cluster.areTagsSame(newCluster));
  }

  // Tags do not apply to non-AWS provider. This checks that tags check are always
  // considered 'same' for those providers.
  @Test
  public void testAreTagsSameOnGCP() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    UserIntent userIntent = getBaseIntent();
    userIntent.providerType = CloudType.gcp;
    userIntent.instanceTags = ImmutableMap.of("Cust", "Test", "Dept", "Misc");
    Cluster cluster = taskParams.upsertPrimaryCluster(userIntent, null);

    UserIntent newUserIntent = getBaseIntent();
    newUserIntent.providerType = CloudType.gcp;
    newUserIntent.instanceTags = ImmutableMap.of("Cust", "Test");
    Cluster newCluster = new Cluster(ClusterType.PRIMARY, newUserIntent);
    assertTrue(cluster.areTagsSame(newCluster));

    newUserIntent = getBaseIntent();
    newUserIntent.providerType = CloudType.gcp;
    newCluster = new Cluster(ClusterType.PRIMARY, newUserIntent);
    assertTrue(cluster.areTagsSame(newCluster));
  }

  @Test
  public void testAreTagsSameErrors() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    UserIntent userIntent = getBaseIntent();
    userIntent.providerType = CloudType.gcp;
    userIntent.instanceTags = ImmutableMap.of("Cust", "Test", "Dept", "Misc");
    Cluster cluster = taskParams.upsertPrimaryCluster(userIntent, null);

    UserIntent newUserIntent = getBaseIntent();
    newUserIntent.providerType = CloudType.aws;
    Cluster newCluster = new Cluster(ClusterType.PRIMARY, newUserIntent);
    newUserIntent.instanceTags = ImmutableMap.of("Cust", "Test");
    try {
      cluster.areTagsSame(newCluster);
    } catch (IllegalArgumentException iae) {
      assertThat(iae.getMessage(), allOf(notNullValue(), containsString("Mismatched provider types")));
    }
  }

  private UserIntent getBaseIntent() {

    // Create regions
    Region r1 = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    Region r2 = Region.create(defaultProvider, "region-2", "Region 2", "yb-image-1");
    Region r3 = Region.create(defaultProvider, "region-3", "Region 3", "yb-image-1");
    List<UUID> regionList = new ArrayList<>();
    regionList.add(r1.uuid);
    regionList.add(r2.uuid);
    regionList.add(r3.uuid);
    String instanceType = "c3.xlarge";
    // Create userIntent
    UserIntent userIntent = new UserIntent();
    userIntent.replicationFactor = 3;
    userIntent.regionList = regionList;
    userIntent.instanceType = instanceType;
    userIntent.provider = defaultProvider.uuid.toString();
    userIntent.deviceInfo = new DeviceInfo();
    userIntent.deviceInfo.storageType = PublicCloudConstants.StorageType.IO1;
    userIntent.deviceInfo.numVolumes = 2;
    userIntent.deviceInfo.diskIops = 1000;
    userIntent.deviceInfo.volumeSize = 100;
    return userIntent;
  }

  @Test
  public void testUpdateNodesDynamicActions_UnderReplicatedMaster_WithoutReadOnlyCluster() {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    UserIntent userIntent = new UserIntent();
    userIntent.replicationFactor = 3;
    userIntent.regionList = new ArrayList<>();
    userIntent.provider = Provider.get(defaultCustomer.uuid, Common.CloudType.aws).uuid.toString();
    userIntent.numNodes = 3;
    // All nodes are created with t-server only. So they are all
    // areMastersUnderReplicated.
    u = Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater(userIntent));

    ObjectNode json = (ObjectNode) Json.toJson(u.getUniverseDetails());
    u.updateNodesDynamicActions(json, u.getNodes());

    JsonNode nodeDetailsSet = json.get("nodeDetailsSet");
    assertNotNull(nodeDetailsSet);
    assertTrue(nodeDetailsSet.isArray());
    for (int i = 0; i < nodeDetailsSet.size(); i++) {
      assertTrue(jsonArrayHasItem(nodeDetailsSet.get(i).get("allowedActions"),
          NodeActionType.START_MASTER.name()));
    }
  }

  @Test
  // @formatter:off
  @Parameters({ "host-n4,      true,  true",  // underReplicated, node from primary cluster
                "yb-tserver-0, true,  false", // underReplicated, node from read only cluster
                "host-n4,      false, false", // not underReplicated, node from primary cluster
                "yb-tserver-0, false, false"  // not underReplicated, node from read only cluster
              })
  // @formatter:on
  public void testUpdateNodesDynamicActions_WithReadOnlyCluster(String nodeToTest,
      boolean isMasterUnderReplicated, boolean expectedResult) {
    Universe u = createUniverse(defaultCustomer.getCustomerId());
    UserIntent userIntent = new UserIntent();
    userIntent.replicationFactor = 3;
    userIntent.regionList = new ArrayList<>();
    userIntent.provider = Provider.get(defaultCustomer.uuid, Common.CloudType.aws).uuid.toString();
    userIntent.numNodes = 3;
    u = Universe.saveDetails(u.universeUUID,
        ApiUtils.mockUniverseUpdaterWithInactiveAndReadReplicaNodes(true, 3));

    if (isMasterUnderReplicated) {
      // Stopping master.
      updateNode(u, "host-n1", NodeState.Stopped, false);
      // One t-server is alive.
      updateNode(u, "host-n4", NodeState.Live, false);
      assertTrue(Util.areMastersUnderReplicated(u.getNode("host-n4"), u));
    }

    ObjectNode json = (ObjectNode) Json.toJson(u.getUniverseDetails());
    u.updateNodesDynamicActions(json, u.getUniverseDetails().nodeDetailsSet);

    JsonNode nodeDetailsSet = json.get("nodeDetailsSet");
    assertNotNull(nodeDetailsSet);
    assertTrue(nodeDetailsSet.isArray());
    assertEquals(expectedResult,
        nodeHasAction(nodeDetailsSet, nodeToTest, NodeActionType.START_MASTER.name()));
    assertEquals(expectedResult, u.isNodeActionAllowed(nodeToTest, NodeActionType.START_MASTER));
  }

  // Updates the node state
  private void updateNode(Universe u, String nodeName, NodeState newState, boolean newIsMaster) {
    Universe.UniverseUpdater updater = new Universe.UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        for (NodeDetails node : u.getUniverseDetails().nodeDetailsSet) {
          if (StringUtils.equals(node.nodeName, nodeName)) {
            node.state = newState;
            node.isMaster = newIsMaster;
            break;
          }
        }
      }
    };
    Universe.saveDetails(u.universeUUID, updater);
  }

  private static boolean nodeHasAction(JsonNode nodeDetailsSet, String nodeName,
      String actionName) {
    boolean nodeFound = false;
    boolean actionFound = false;
    for (int i = 0; i < nodeDetailsSet.size(); i++) {
      JsonNode jsonNode = nodeDetailsSet.get(i);
      assertNotNull(jsonNode.get("nodeName"));
      if (StringUtils.equals(jsonNode.get("nodeName").asText(), nodeName)) {
        nodeFound = true;
        actionFound = jsonArrayHasItem(jsonNode.get("allowedActions"), actionName);
      }
    }
    assertTrue(nodeFound);
    return actionFound;
  }

  private static boolean jsonArrayHasItem(JsonNode arr, String value) {
    assertNotNull(arr);
    assertTrue(arr.isArray());
    for (JsonNode item : (ArrayNode) arr) {
      if (StringUtils.equals(item.asText(), value)) {
        return true;
      }
    }
    return false;
  }
}
