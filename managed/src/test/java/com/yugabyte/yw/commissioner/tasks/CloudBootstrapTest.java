// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.runners.MockitoJUnitRunner;
import play.libs.Json;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.mockito.Mockito.*;

@RunWith(MockitoJUnitRunner.class)
public class CloudBootstrapTest extends CommissionerBaseTest {
  @InjectMocks
  Commissioner commissioner;

  final String hostVpcRegion = "host-vpc-region";
  final String hostVpcId = "host-vpc-id";
  final String destVpcId = "dest-vpc-id";

  private void mockRegionMetadata(Common.CloudType cloudType) {
    Map<String, Object> regionMetadata = new HashMap<>();
    regionMetadata.put("name", "Mock Region");
    regionMetadata.put("latitude", 36.778261);
    regionMetadata.put("longitude", -119.417932);

    when(mockConfigHelper.getRegionMetadata(cloudType))
        .thenReturn(ImmutableMap.of(
              // AWS regions to use.
              "us-west-1", regionMetadata,
              "us-west-2", regionMetadata,
              "us-east-1", regionMetadata,
              // GCP regions to use.
              "us-west1", regionMetadata,
              "us-east1", regionMetadata
              ));
  }

  private UUID submitTask(CloudBootstrap.Params taskParams) {
    return commissioner.submit(TaskType.CloudBootstrap, taskParams);
  }

  private CloudBootstrap.Params getBaseTaskParams() {
    CloudBootstrap.Params taskParams = new CloudBootstrap.Params();
    taskParams.providerUUID = defaultProvider.uuid;
    taskParams.hostVpcRegion = hostVpcRegion;
    taskParams.hostVpcId = hostVpcId;
    taskParams.destVpcId = destVpcId;
    taskParams.sshPort = 12345;
    taskParams.airGapInstall = false;
    return taskParams;
  }

  private void validateCloudBootstrapSuccess(
      CloudBootstrap.Params taskParams,
      JsonNode zoneInfo,
      List<String> expectedRegions,
      String expectedProviderCode,
      boolean customAccessKey,
      boolean customAzMapping,
      boolean customSecurityGroup,
      boolean customImageId) throws InterruptedException {
    Provider provider = Provider.get(taskParams.providerUUID);
    // Mock region metadata.
    mockRegionMetadata(Common.CloudType.valueOf(provider.code));
    // TODO(bogdan): we don't really care about the output now..
    when(mockNetworkManager.bootstrap(any(), any(), anyString()))
        .thenReturn(Json.parse("{}"));
    when(mockCloudQueryHelper.getZones(any(UUID.class), anyString()))
        .thenReturn(zoneInfo);
    when(mockCloudQueryHelper.getZones(any(UUID.class), anyString(), anyString()))
        .thenReturn(zoneInfo);
    String defaultImage = "test_image_id";
    when(mockCloudQueryHelper.getDefaultImage(any(Region.class)))
        .thenReturn(defaultImage);
    taskParams.providerUUID = provider.uuid;

    UUID taskUUID = submitTask(taskParams);
    TaskInfo taskInfo = waitForTask(taskUUID);
    assertValue(Json.toJson(taskInfo), "taskState", "Success");
    if (expectedProviderCode.equals("aws")) {
      verify(mockAWSInitializer, times(1)).initialize(defaultCustomer.uuid, provider.uuid);
    } else if (expectedProviderCode.equals("gcp")) {
      verify(mockGCPInitializer, times(1)).initialize(defaultCustomer.uuid, provider.uuid);
    } else {
      // Only support AWS and GCP for now.
      assertNotNull(null);
    }
    // TODO(bogdan): do we want a different handling here?
    String customPayload = Json.stringify(Json.toJson(taskParams));
    verify(mockNetworkManager, times(1)).bootstrap(null, provider.uuid, customPayload);
    assertEquals(taskParams.perRegionMetadata.size(), expectedRegions.size());
    // Check per-region settings.
    for (Map.Entry<String, CloudBootstrap.Params.PerRegionMetadata> entry : taskParams.perRegionMetadata.entrySet()) {
      String regionName = entry.getKey();
      CloudBootstrap.Params.PerRegionMetadata metadata = entry.getValue();
      // Expected region.
      assertNotNull(expectedRegions.contains(regionName));
      Region r = Region.getByCode(provider, regionName);
      assertNotNull(r);
      // Check AccessKey info.
      if (customAccessKey) {
        // TODO: might need to add port here.
        verify(mockAccessManager, times(1)).addKey(
            eq(r.uuid), eq(taskParams.keyPairName), any(), eq(taskParams.sshUser),
            eq(taskParams.sshPort), eq(taskParams.airGapInstall));
      } else {
        String expectedAccessKeyCode = String.format(
            "yb-%s-%s-key", defaultCustomer.code, provider.name.toLowerCase());
        verify(mockAccessManager, times(1)).addKey(eq(r.uuid), eq(expectedAccessKeyCode),
               any(),eq(taskParams.sshUser), eq(taskParams.sshPort), eq(taskParams.airGapInstall));
      }
      // Check AZ info.
      Set<AvailabilityZone> zones = r.zones;
      assertNotNull(zones);
      if (customAzMapping) {
        assertEquals(metadata.azToSubnetIds.size(), zones.size());
        for (AvailabilityZone zone : zones) {
          String subnet = metadata.azToSubnetIds.get(zone.code);
          assertNotNull(subnet);
        }
      } else {
        // By default, assume the test puts just 1 in the zoneInfo.
        assertEquals(1, zones.size());
      }
      // Check SG info.
      if (customSecurityGroup) {
        assertEquals(r.getSecurityGroupId(), metadata.customSecurityGroupId);
      }
      // Check AMI info.
      if (customImageId) {
        assertEquals(r.ybImage, metadata.customImageId);
      } else {
        assertEquals(r.ybImage, defaultImage);
      }
    }
  }

  @Test
  public void testCloudBootstrapSuccessAwsDefaultSingleRegion() throws InterruptedException {
    JsonNode zoneInfo = Json.parse("{\"us-west-1\": {\"zone-1\": \"subnet-1\"}}");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.perRegionMetadata.put("us-west-1", new CloudBootstrap.Params.PerRegionMetadata());
    validateCloudBootstrapSuccess(
        taskParams, zoneInfo, ImmutableList.of("us-west-1"), "aws", false, false, false, false);
  }

  @Test
  public void testCloudBootstrapSuccessAwsDefaultSingleRegionCustomAccess() throws InterruptedException {
    JsonNode zoneInfo = Json.parse("{\"us-west-1\": {\"zone-1\": \"subnet-1\"}}");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.perRegionMetadata.put("us-west-1", new CloudBootstrap.Params.PerRegionMetadata());
    // Add in the keypair info.
    taskParams.keyPairName = "keypair-name";
    taskParams.sshPrivateKeyContent = "ssh-content";
    taskParams.sshUser = "ssh-user";
    validateCloudBootstrapSuccess(
        taskParams, zoneInfo, ImmutableList.of("us-west-1"), "aws", true, false, false, false);
  }

  @Test
  public void testCloudBootstrapSuccessAwsDefaultSingleRegionCustomAccessIncomplete() throws InterruptedException {
    JsonNode zoneInfo = Json.parse("{\"us-west-1\": {\"zone-1\": \"subnet-1\"}}");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.perRegionMetadata.put("us-west-1", new CloudBootstrap.Params.PerRegionMetadata());
    // Add in the keypair info.
    taskParams.keyPairName = "keypair-name";
    // Leave out one required component, expect to ignore.
    // taskParams.sshPrivateKeyContent = "ssh-content";
    taskParams.sshUser = "ssh-user";
    validateCloudBootstrapSuccess(
        taskParams, zoneInfo, ImmutableList.of("us-west-1"), "aws", false, false, false, false);
  }

  @Test
  public void testCloudBootstrapSuccessAwsCustomMultiRegion() throws InterruptedException {
    // Zone information should not be used if we are passing in custom azToSubnetIds mapping.
    JsonNode zoneInfo = Json.parse("{}");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    // Add region west.
    CloudBootstrap.Params.PerRegionMetadata westRegion = new CloudBootstrap.Params.PerRegionMetadata();
    westRegion.vpcId = "west-id";
    westRegion.azToSubnetIds = new HashMap<>();
    westRegion.azToSubnetIds.put("us-west-1a", "subnet-1");
    westRegion.azToSubnetIds.put("us-west-1b", "subnet-2");
    westRegion.customImageId = "west-image";
    westRegion.customSecurityGroupId = "west-sg-id";
    // Add region east.
    CloudBootstrap.Params.PerRegionMetadata eastRegion = new CloudBootstrap.Params.PerRegionMetadata();
    eastRegion.vpcId = "east-id";
    eastRegion.azToSubnetIds = new HashMap<>();
    eastRegion.azToSubnetIds.put("us-east-1a", "subnet-1");
    eastRegion.azToSubnetIds.put("us-east-1b", "subnet-2");
    eastRegion.customImageId = "east-image";
    eastRegion.customSecurityGroupId = "east-sg-id";
    // Add all the regions in the taskParams and validate.
    taskParams.perRegionMetadata.put("us-west-1", westRegion);
    taskParams.perRegionMetadata.put("us-east-1", eastRegion);
    // Add in the keypair info.
    taskParams.keyPairName = "keypair-name";
    taskParams.sshPrivateKeyContent = "ssh-content";
    taskParams.sshUser = "ssh-user";
    validateCloudBootstrapSuccess(
        taskParams, zoneInfo, ImmutableList.of("us-west-1", "us-east-1"), "aws", true, true, true,
        true);
  }

  @Test
  public void testCloudBootstrapSuccessAwsCustomSingleRegionJustAzs() throws InterruptedException {
    // Zone information should not be used if we are passing in custom azToSubnetIds mapping.
    JsonNode zoneInfo = Json.parse("{}");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    // Add region west.
    CloudBootstrap.Params.PerRegionMetadata westRegion = new CloudBootstrap.Params.PerRegionMetadata();
    westRegion.vpcId = "west-id";
    westRegion.azToSubnetIds = new HashMap<>();
    westRegion.azToSubnetIds.put("us-west-1a", "subnet-1");
    westRegion.azToSubnetIds.put("us-west-1b", "subnet-2");
    // Add all the regions in the taskParams and validate.
    taskParams.perRegionMetadata.put("us-west-1", westRegion);
    validateCloudBootstrapSuccess(
        taskParams, zoneInfo, ImmutableList.of("us-west-1"), "aws", false, true, false, false);
  }

  @Test
  public void testCloudBootstrapSuccessGcp() throws InterruptedException {
    JsonNode zoneInfo = Json.parse("{\"us-west1\": {\"zones\": [\"zone-1\"], \"subnetworks\": [\"subnet-0\"]}}");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.providerUUID = gcpProvider.uuid;
    taskParams.perRegionMetadata.put("us-west1", new CloudBootstrap.Params.PerRegionMetadata());
    validateCloudBootstrapSuccess(
        taskParams, zoneInfo, ImmutableList.of("us-west1"), "gcp", false, false, false, false);
  }

  @Test
  public void testCloudBootstrapWithInvalidRegion() throws InterruptedException {
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.perRegionMetadata.put("fake-region", new CloudBootstrap.Params.PerRegionMetadata());
    UUID taskUUID = submitTask(taskParams);
    TaskInfo taskInfo = waitForTask(taskUUID);
    assertValue(Json.toJson(taskInfo), "taskState", "Failure");
  }

  @Test
  public void testCloudBootstrapWithExistingRegion() throws InterruptedException {
    Region.create(defaultProvider, "us-west-1", "US west 1", "yb-image");
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.perRegionMetadata.put("us-west-1", new CloudBootstrap.Params.PerRegionMetadata());
    UUID taskUUID = submitTask(taskParams);
    TaskInfo taskInfo = waitForTask(taskUUID);
    assertValue(Json.toJson(taskInfo), "taskState", "Failure");
  }

  @Test
  public void testCloudBootstrapWithNetworkBootstrapError() throws InterruptedException {
    JsonNode vpcInfo = Json.parse("{\"error\": \"Something failed\"}");
    when(mockNetworkManager.bootstrap(any(), any(), anyString()))
        .thenReturn(vpcInfo);
    CloudBootstrap.Params taskParams = getBaseTaskParams();
    taskParams.perRegionMetadata.put("us-west-1", new CloudBootstrap.Params.PerRegionMetadata());
    UUID taskUUID = submitTask(taskParams);
    TaskInfo taskInfo = waitForTask(taskUUID);
    assertValue(Json.toJson(taskInfo), "taskState", "Failure");
    Region r = Region.getByCode(defaultProvider, "us-west-1");
    assertNull(r);
  }
}
