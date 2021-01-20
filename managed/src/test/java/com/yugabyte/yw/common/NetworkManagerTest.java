// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;


import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.Mockito;
import org.mockito.runners.MockitoJUnitRunner;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static org.junit.Assert.assertEquals;
import static org.mockito.Matchers.anyList;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Matchers.anyString;
import static org.mockito.Mockito.description;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.when;


@RunWith(MockitoJUnitRunner.class)
public class NetworkManagerTest extends FakeDBApplication {
  @InjectMocks
  NetworkManager networkManager;

  @Mock
  ShellProcessHandler shellProcessHandler;

  private Provider defaultProvider;
  private Region defaultRegion;
  ArgumentCaptor<ArrayList> command;
  ArgumentCaptor<HashMap> cloudCredentials;

  @Before
  public void beforeTest() {
    defaultProvider = ModelFactory.awsProvider(ModelFactory.testCustomer());
    defaultRegion = Region.create(defaultProvider, "us-west-2", "US West 2", "yb-image");
    command = ArgumentCaptor.forClass(ArrayList.class);
    cloudCredentials = ArgumentCaptor.forClass(HashMap.class);
  }

  private JsonNode runBootstrap(UUID regionUUID, UUID providerUUID, String customPayload, boolean mimicError) {
    ShellResponse response = new ShellResponse();
    if (mimicError) {
      response.message = "{\"error\": \"Unknown Error\"}";
      response.code = 99;
    } else {
      response.code = 0;
      response.message = "{\"foo\": \"bar\"}";
    }
    when(shellProcessHandler.run(anyList(), anyMap(), anyString())).thenReturn(response);
    return networkManager.bootstrap(regionUUID, providerUUID, customPayload);
  }

  private JsonNode runCommand(UUID regionUUID, String commandType, boolean mimicError) {
    ShellResponse response = new ShellResponse();
    if (mimicError) {
      response.message = "{\"error\": \"Unknown Error\"}";
      response.code = 99;
    } else {
      response.code = 0;
      response.message = "{\"foo\": \"bar\"}";
    }
    when(shellProcessHandler.run(anyList(), anyMap(), anyString())).thenReturn(response);

    if (commandType.equals("query")) {
      return networkManager.query(regionUUID, "");
    } else if (commandType.equals("cleanup")) {
      return networkManager.cleanup(regionUUID);
    }
    return null;
  }

  @Test
  public void testCommandSuccess() {
    List<String> commandTypes = Arrays.asList("query", "cleanup");
    commandTypes.forEach(commandType -> {
      JsonNode json = runCommand(defaultRegion.uuid, commandType, false);
      assertValue(json, "foo", "bar");
    });
  }

  @Test
  public void testCommandFailure() {
    List<String> commandTypes = Arrays.asList("query", "cleanup");
    commandTypes.forEach(commandType -> {
      try {
        runCommand(defaultRegion.uuid, commandType, true);
      } catch (RuntimeException re) {
        assertEquals(re.getMessage(), "YBCloud command network (" + commandType + ") failed to execute.");
      }
    });
  }

  @Test
  public void testBootstrapCommandWithProvider() {
    JsonNode json = runBootstrap(null, defaultRegion.provider.uuid, "{}", false);
    Mockito.verify(shellProcessHandler, times(1)).run((List<String>) command.capture(),
        (Map<String, String>) cloudCredentials.capture(), anyString());
    assertEquals(String.join(" ", command.getValue()),
        "bin/ybcloud.sh aws network bootstrap --custom_payload {}");
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testGcpBootstrapCommandWithProvider() {
    Provider gcpProvider = ModelFactory.gcpProvider(ModelFactory.testCustomer());
    Region gcpRegion = Region.create(gcpProvider, "us-west1", "US West1", "yb-image");
    JsonNode json = runBootstrap(null, gcpRegion.provider.uuid, "{}", false);
    Mockito.verify(shellProcessHandler, times(1)).run((List<String>) command.capture(),
        (Map<String, String>) cloudCredentials.capture(), anyString());
    assertEquals(String.join(" ", command.getValue()),
        "bin/ybcloud.sh gcp network bootstrap --custom_payload {}");
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testGcpBootstrapCommandWithPayload() {
    Provider gcpProvider = ModelFactory.gcpProvider(ModelFactory.testCustomer());
    Region gcpRegion = Region.create(gcpProvider, "us-west1", "US West1", "yb-image");
    String payload = "{\"region\": \"gcptest\"}";
    JsonNode json = runBootstrap(null, gcpRegion.provider.uuid, payload, false);
    Mockito.verify(shellProcessHandler, times(1)).run((List<String>) command.capture(),
        (Map<String, String>) cloudCredentials.capture(), anyString());
    assertEquals(String.join(" ", command.getValue()),
        "bin/ybcloud.sh gcp network bootstrap --custom_payload " + payload);
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testBootstrapCommandWithRegion() {
    JsonNode json = runBootstrap(defaultRegion.uuid, null, "{}", false);
    Mockito.verify(shellProcessHandler, times(1)).run((List<String>) command.capture(),
        (Map<String, String>) cloudCredentials.capture(), anyString());
    assertEquals(String.join(" ", command.getValue()),
        "bin/ybcloud.sh aws --region us-west-2 network bootstrap --custom_payload {}");
    assertValue(json, "foo", "bar");
  }

  @Test
  public void testBootstrapCommandWithRegionAndProvider() {
    // If both are provided, we first check for region and use --region if available.
    JsonNode json = runBootstrap(defaultRegion.uuid, defaultRegion.provider.uuid, "{}", false);
    Mockito.verify(shellProcessHandler, times(1)).run((List<String>) command.capture(),
        (Map<String, String>) cloudCredentials.capture(), anyString());
    assertEquals(String.join(" ", command.getValue()),
        "bin/ybcloud.sh aws --region us-west-2 network bootstrap --custom_payload {}");
    assertValue(json, "foo", "bar");
  }
}
