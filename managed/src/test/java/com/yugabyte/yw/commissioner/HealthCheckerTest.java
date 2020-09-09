// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import akka.actor.ActorSystem;
import akka.actor.Scheduler;
import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.commissioner.HealthChecker;
import com.yugabyte.yw.commissioner.tasks.CommissionerBaseTest;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.HealthManager;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.CustomerRegisterFormData.AlertingData;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import org.junit.Before;
import org.junit.Ignore;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.runners.MockitoJUnitRunner;
import org.mockito.ArgumentCaptor;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.api.Play;
import play.Configuration;
import play.Environment;
import play.libs.Json;

import scala.concurrent.ExecutionContext;

import java.util.*;
import java.util.stream.Collectors;

import io.prometheus.client.CollectorRegistry;

import static com.yugabyte.yw.common.AssertHelper.assertJsonEqual;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Matchers.anyList;
import static org.mockito.Mockito.*;

@RunWith(MockitoJUnitRunner.class)
public class HealthCheckerTest extends FakeDBApplication {
  public static final Logger LOG = LoggerFactory.getLogger(HealthCheckerTest.class);

  private static final String YB_ALERT_TEST_EMAIL = "test@yugabyte.com";
  private static final String dummyNode = "n";
  private static final String dummyCheck = "c";

  HealthChecker healthChecker;

  @Mock
  ActorSystem mockActorSystem;
  @Mock
  play.Configuration mockConfig;
  @Mock
  Environment mockEnvironment;
  @Mock
  ExecutionContext mockExecutionContext;
  @Mock
  HealthManager mockHealthManager;
  @Mock
  Scheduler mockScheduler;

  Customer defaultCustomer;
  Provider defaultProvider;
  Provider kubernetesProvider;

  Universe universe;
  AccessKey accessKey;
  CustomerConfig customerConfig;

  CollectorRegistry testRegistry;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
    kubernetesProvider = ModelFactory.kubernetesProvider(defaultCustomer);

    when(mockActorSystem.scheduler()).thenReturn(mockScheduler);

    when(mockConfig.getString("yb.health.default_email")).thenReturn(YB_ALERT_TEST_EMAIL);
    ShellProcessHandler.ShellResponse dummyShellResponse =
      ShellProcessHandler.ShellResponse.create(
        0,
        ("{''error'': false, ''data'': [ {''node'':''" + dummyNode +
         "'', ''has_error'': true, ''message'':''" + dummyCheck +
         "'' } ] }").replace("''", "\"") );

    when(mockHealthManager.runCommand(
        any(), any(), any(), any(), any(), any(), any(), any(), any())
    ).thenReturn(dummyShellResponse);

    testRegistry = new CollectorRegistry();

    // Finally setup the mocked instance.
    healthChecker = new HealthChecker(
        mockActorSystem,
        mockConfig,
        mockEnvironment,
        mockExecutionContext,
        mockHealthManager,
        testRegistry);
  }

  private Universe setupUniverse(String name) {
    AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
    keyInfo.sshPort = 3333;
    accessKey = AccessKey.create(
        defaultProvider.uuid,
        "key-" + name,
        keyInfo);

    universe = ModelFactory.createUniverse(name, defaultCustomer.getCustomerId());
    // Universe modifies customer, so we need to refresh our in-memory view of this reference.
    defaultCustomer = Customer.get(defaultCustomer.uuid);

    UniverseDefinitionTaskParams.UserIntent userIntent =
        universe.getUniverseDetails().getPrimaryCluster().userIntent;
    userIntent.accessKeyCode = accessKey.getKeyCode();
    Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdater(userIntent));
    return Universe.get(universe.universeUUID);
  }

  private Universe setupK8sUniverse(String name) {
    Region r = Region.create(kubernetesProvider, "region-1", "PlacementRegion-1", "default-image");
    AvailabilityZone az = AvailabilityZone.create(r, "az-1", "PlacementAZ-1", "subnet-1");
    PlacementInfo pi = new PlacementInfo();
    PlacementInfoUtil.addPlacementZoneHelper(az.uuid, pi);
    Map<String, String> config = new HashMap<>();
    config.put("KUBECONFIG", "foo");
    kubernetesProvider.setConfig(config);
    // Universe modifies customer, so we need to refresh our in-memory view of this reference.
    defaultCustomer = Customer.get(defaultCustomer.uuid);
    universe = ModelFactory.createUniverse(name, UUID.randomUUID(),
                                           defaultCustomer.getCustomerId(),
                                           Common.CloudType.kubernetes, pi);
    Universe.saveDetails(universe.universeUUID, ApiUtils.mockUniverseUpdaterWithActiveYSQLNode());
    return Universe.get(universe.universeUUID);
  }

  private void setupAlertingData(
    String alertingEmail,
    boolean sendAlertsToYb,
    boolean reportOnlyErrors) {

    AlertingData data = new AlertingData();
    data.sendAlertsToYb = sendAlertsToYb;
    data.alertingEmail = alertingEmail;
    data.reportOnlyErrors = reportOnlyErrors;

    if (null == customerConfig) {
      // Setup alerting data.
      customerConfig = CustomerConfig.createAlertConfig(defaultCustomer.uuid, Json.toJson(data));
    } else {
      customerConfig.data = Json.toJson(data);
      customerConfig.update();
    }
  }

  private Universe setupDisabledAlertsConfig(String email, long disabledUntilSecs) {
    Universe u = setupUniverse("univ1");
    setupAlertingData(email, false, false);
    Map<String, String> config = new HashMap<>();
    config.put(Universe.DISABLE_ALERTS_UNTIL, Long.toString(disabledUntilSecs));
    u.setConfig(config);
    return u;
  }

  private void verifyHealthManager(Universe u, String expectedEmail) {
    verify(mockHealthManager, times(1)).runCommand(
        eq(defaultProvider),
        any(),
        eq(u.name),
        eq(String.format("[%s][%s]", defaultCustomer.name, defaultCustomer.code)),
        eq(expectedEmail),
        eq(0L),
        eq(true),
        eq(false),
        any());
  }

  private void verifyK8sHealthManager(Universe u, String expectedEmail) {
    ArgumentCaptor<List> expectedClusters = ArgumentCaptor.forClass(List.class);
    verify(mockHealthManager, times(1)).runCommand(
        eq(kubernetesProvider),
        expectedClusters.capture(),
        eq(u.name),
        eq(String.format("[%s][%s]", defaultCustomer.name, defaultCustomer.code)),
        eq(expectedEmail),
        eq(0L),
        eq(true),
        eq(false),
        any());
    HealthManager.ClusterInfo cluster = (HealthManager.ClusterInfo) expectedClusters.getValue().get(0);
    assertEquals(cluster.namespaceToConfig.get("univ1"), "foo");
    assertEquals(cluster.ysqlPort, 5433);
    assertEquals(expectedClusters.getValue().size(), 1);
  }

  private void testSingleUniverse(Universe u, String expectedEmail, boolean shouldFail) {
    healthChecker.checkSingleUniverse(u, defaultCustomer, customerConfig, true, null);
    verifyHealthManager(u, expectedEmail);

    String[] labels = { HealthChecker.kUnivUUIDLabel, HealthChecker.kUnivNameLabel,
                        HealthChecker.kNodeLabel, HealthChecker.kCheckLabel };
    String [] labelValues = { u.universeUUID.toString(), u.name, dummyNode, dummyCheck };
    Double val = testRegistry.getSampleValue(HealthChecker.kUnivMetricName, labels, labelValues);
    if (shouldFail) {
      assertNull(val);
    } else {
      assertEquals(val.intValue(), 1);
    }
  }

  private void testSingleK8sUniverse(Universe u, String expectedEmail) {
    healthChecker.checkSingleUniverse(u, defaultCustomer, customerConfig, true, null);
    verifyK8sHealthManager(u, expectedEmail);
  }

  private void validateNoDevopsCall() {
    healthChecker.checkCustomer(defaultCustomer);

    verify(mockHealthManager, times(0)).runCommand(
        any(), any(), any(), any(), any(), any(), any(), any(), any());
  }

  @Test
  public void testSingleUniverseNoEmail() {
    Universe u = setupUniverse("univ1");
    setupAlertingData(null, false, false);
    testSingleUniverse(u, null, false);
  }

  @Test
  public void testSingleK8sUniverseNoEmail() {
    Universe u = setupK8sUniverse("univ1");
    setupAlertingData(null, false, false);
    testSingleK8sUniverse(u, null);
  }

  @Test
  public void testSingleUniverseYbEmail() {
    Universe u = setupUniverse("univ1");
    setupAlertingData(null, true, false);
    testSingleUniverse(u, YB_ALERT_TEST_EMAIL, false);
  }

  @Test
  public void testReportOnlyErrors() {
    Universe u = setupUniverse("univ1");

    // enable report only errors
    setupAlertingData(null, true, true);
    healthChecker.checkSingleUniverse(u, defaultCustomer, customerConfig, false, null);
    verify(mockHealthManager, times(1)).runCommand(
      eq(defaultProvider),
      any(),
      eq(u.name),
      eq(String.format("[%s][%s]", defaultCustomer.name, defaultCustomer.code)),
      eq(YB_ALERT_TEST_EMAIL),
      eq(0L),
      eq(false),
      eq(true),
      any());

      // disable report only errors
      setupAlertingData(null, true, false);
      healthChecker.checkSingleUniverse(u, defaultCustomer, customerConfig, false, null);
      verify(mockHealthManager, times(1)).runCommand(
        eq(defaultProvider),
        any(),
        eq(u.name),
        eq(String.format("[%s][%s]", defaultCustomer.name, defaultCustomer.code)),
        eq(YB_ALERT_TEST_EMAIL),
        eq(0L),
        eq(false),
        eq(false),
        any());
  }

  @Test
  public void testSingleUniverseCustomEmail() {
    Universe u = setupUniverse("univ1");
    String email = "foo@yugabyte.com";
    setupAlertingData(email, false, false);
    testSingleUniverse(u, email, false);
  }

  @Test
  public void testDisabledAlerts1() {
    String email = "foo@yugabyte.com";
    Universe u = setupDisabledAlertsConfig(email, 0);
    testSingleUniverse(u, email, false);
  }

  @Test
  public void testDisabledAlerts2() {
    String email = "foo@yugabyte.com";
    Universe u = setupDisabledAlertsConfig(email, Long.MAX_VALUE);
    testSingleUniverse(u, null, false);
  }

  @Test
  public void testDisabledAlerts3() {
    long now = System.currentTimeMillis() / 1000;
    String email = "foo@yugabyte.com";
    Universe u = setupDisabledAlertsConfig(email, now + 1000);
    testSingleUniverse(u, null, false);
  }

  @Test
  public void testDisabledAlerts4() {
    long now = System.currentTimeMillis() / 1000;
    String email = "foo@yugabyte.com";
    Universe u = setupDisabledAlertsConfig(email, now - 10);
    testSingleUniverse(u, email, false);
  }

  @Test
  public void testSingleUniverseMultipleEmails() {
    Universe u = setupUniverse("univ1");
    String email = "foo@yugabyte.com";
    setupAlertingData(email, true, false);
    testSingleUniverse(u, String.format("%s,%s", YB_ALERT_TEST_EMAIL, email), false);
  }

  @Test
  public void testMultipleUniversesIndividually() {
    Universe univ1 = setupUniverse("univ1");
    Universe univ2 = setupUniverse("univ2");
    setupAlertingData(null, false, false);
    testSingleUniverse(univ1, null, false);
    testSingleUniverse(univ2, null, false);
  }

  @Test
  public void testMultipleUniversesTogether() {
    Universe univ1 = setupUniverse("univ1");
    Universe univ2 = setupUniverse("univ2");
    setupAlertingData(null, false, false);
    healthChecker.checkAllUniverses(defaultCustomer, customerConfig, true, null);
    verifyHealthManager(univ1, null);
    verifyHealthManager(univ2, null);
  }

  @Test
  public void testNoUniverse() {
    validateNoDevopsCall();
  }

  @Test
  public void testNoAlertingConfig() {
    Universe u = setupUniverse("univ1");
    validateNoDevopsCall();
  }

  @Test
  public void testInvalidUniverseNullDetails() {
    Universe u = setupUniverse("test");
    // Set the details to null.
    Universe.saveDetails(u.universeUUID, new Universe.UniverseUpdater() {
      @Override
      public void run(Universe univ) {
        univ.setUniverseDetails(null);
      };
    });
    setupAlertingData(null, false, false);
    // Add a reference to this on the customer anyway.
    validateNoDevopsCall();
  }

  @Test
  public void testInvalidUniverseUpdateInProgress() {
    Universe u = setupUniverse("test");
    // Set updateInProgress to true.
    Universe.saveDetails(u.universeUUID, new Universe.UniverseUpdater() {
      @Override
      public void run(Universe univ) {
        UniverseDefinitionTaskParams details = univ.getUniverseDetails();
        details.updateInProgress = true;
        univ.setUniverseDetails(details);
      };
    });
    setupAlertingData(null, false, false);
    validateNoDevopsCall();
  }

  @Test
  public void testInvalidUniverseBadProvider() {
    Universe u = setupUniverse("test");
    // Setup an invalid provider.
    Universe.saveDetails(u.universeUUID, new Universe.UniverseUpdater() {
      @Override
      public void run(Universe univ) {
        UniverseDefinitionTaskParams details = univ.getUniverseDetails();
        UniverseDefinitionTaskParams.UserIntent userIntent = details.getPrimaryCluster().userIntent;
        userIntent.provider = UUID.randomUUID().toString();
        univ.setUniverseDetails(details);
      };
    });
    setupAlertingData(null, false, false);
    validateNoDevopsCall();
  }

  @Test
  public void testInvalidUniverseNoAccessKey() {
    Universe u = setupUniverse("test");
    setupAlertingData(null, false, false);
    accessKey.delete();
    validateNoDevopsCall();
  }

  @Test
  public void testTimingLogic() {
    // Setup some waits.
    long waitMs = 500;
    // Wait one cycle between checks.
    when(mockConfig.getLong("yb.health.check_interval_ms")).thenReturn(waitMs);
    // Wait two cycles between status updates.
    when(mockConfig.getLong("yb.health.status_interval_ms")).thenReturn(2 * waitMs);
    // Default prep.
    Universe u = setupUniverse("test");
    setupAlertingData(null, false, false);
    // First time we both check and send update.
    healthChecker.checkCustomer(defaultCustomer);
    verify(mockHealthManager, times(1)).runCommand(
        any(), any(), any(), any(), any(), any(), eq(true), eq(false), any());
    // If we run right afterwards, none of the timers should be hit again, so total hit with any
    // args should still be 1.
    healthChecker.checkCustomer(defaultCustomer);
    verify(mockHealthManager, times(1)).runCommand(
        any(), any(), any(), any(), any(), any(), any(), any(), any());
    try {
      Thread.sleep(waitMs);
    } catch (InterruptedException e) {
    }
    // One cycle later, we should be running another test, but no status update, so first time
    // running with false.
    healthChecker.checkCustomer(defaultCustomer);
    verify(mockHealthManager, times(1)).runCommand(
        any(), any(), any(), any(), any(), any(), eq(false), eq(false), any());
    // Another cycle later, we should be running yet another test, but now with status update.
    try {
      Thread.sleep(waitMs);
    } catch (InterruptedException e) {
    }
    // One cycle later, we should be running another test, but no status update, so second time
    // running with true.
    healthChecker.checkCustomer(defaultCustomer);
    verify(mockHealthManager, times(2)).runCommand(
        any(), any(), any(), any(), any(), any(), eq(true), eq(false), any());
  }

  @Test
  public void testScriptFailure() {
    ShellProcessHandler.ShellResponse dummyShellResponseFail =
      ShellProcessHandler.ShellResponse.create(
        1,
        "Should error");

    when(mockHealthManager.runCommand(
        any(), any(), any(), any(), any(), any(), any(), any(), any())
    ).thenReturn(dummyShellResponseFail);
    Universe u = setupUniverse("univ1");
    setupAlertingData(null, false, false);
    testSingleUniverse(u, null, true);
  }
}
