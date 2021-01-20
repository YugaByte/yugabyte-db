// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.runners.MockitoJUnitRunner;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.assertErrorNodeValue;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static org.mockito.Matchers.anyList;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Matchers.anyString;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
public class CloudQueryHelperTest extends FakeDBApplication {

  @InjectMocks
  CloudQueryHelper cloudQueryHelper;

  @Mock
  ShellProcessHandler shellProcessHandler;
  private Customer defaultCustomer;
  private Provider defaultProvider;
  private Region defaultRegion;
  ArgumentCaptor<ArrayList> command;
  ArgumentCaptor<HashMap> cloudCredentials;

  private enum CommandType {
    zones,
    instance_types,
    host_info
  };

  @Before
  public void beforeTest() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
    defaultRegion = Region.create(defaultProvider, "us-west-2", "US West 2", "yb-image");
  }

  private JsonNode runCommand(UUID regionUUID, boolean mimicError, CommandType command) {
    ShellResponse response = new ShellResponse();
    if (mimicError) {
      response.message = "{\"error\": \"Unknown Error\"}";
      response.code = 99;
    } else {
      response.code = 0;
      response.message = "{\"foo\": \"bar\"}";
    }
    when(shellProcessHandler.run(anyList(), anyMap(), anyString())).thenReturn(response);

    switch (command) {
      case zones:
        return cloudQueryHelper.getZones(regionUUID);
      case instance_types:
        ArrayList<Region> regionList = new ArrayList<Region>();
        regionList.add(Region.get(regionUUID));
        return cloudQueryHelper.getInstanceTypes(regionList, "");
      default:
        return cloudQueryHelper.currentHostInfo(Common.CloudType.aws, ImmutableList.of("vpc-id"));
    }
  }

  @Test
  public void testGetZonesSuccess() {
    Provider gcpProvider = ModelFactory.gcpProvider(defaultCustomer);
    Region gcpRegion = Region.create(gcpProvider, "us-west1", "Gcp US West 1", "yb-image");
    JsonNode json = runCommand(gcpRegion.uuid, false, CommandType.zones);
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testGetZonesFailure() {
    Provider gcpProvider = ModelFactory.gcpProvider(defaultCustomer);
    Region gcpRegion = Region.create(gcpProvider, "us-west1", "Gcp US West 1", "yb-image");
    JsonNode json = runCommand(gcpRegion.uuid, true, CommandType.zones);
    assertErrorNodeValue(json, "YBCloud command query (zones) failed to execute.");
  }

  @Test
  public void testGetInstanceTypesSuccess() {
    Provider gcpProvider = ModelFactory.gcpProvider(defaultCustomer);
    Region gcpRegion = Region.create(gcpProvider, "us-west1", "Gcp US West 1", "yb-image");
    ArrayList<Region> regionList = new ArrayList<>();
    regionList.add(gcpRegion);
    JsonNode json = runCommand(gcpRegion.uuid, false, CommandType.instance_types);
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testGetInstanceTypesFailure() {
    Provider gcpProvider = ModelFactory.gcpProvider(defaultCustomer);
    Region gcpRegion = Region.create(gcpProvider, "us-west1", "Gcp US West 1", "yb-image");
    JsonNode json = runCommand(gcpRegion.uuid, true, CommandType.instance_types);
    assertErrorNodeValue(json, "YBCloud command query (instance_types) failed to execute.");
  }

  @Test
  public void testGetHostInfoSuccess() {
    JsonNode json = runCommand(defaultRegion.uuid, false, CommandType.host_info);
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testGetHostInfoFailure() {
    JsonNode json = runCommand(defaultRegion.uuid, true, CommandType.host_info);
    assertErrorNodeValue(json, "YBCloud command query (current-host) failed to execute.");
  }
}
