// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import com.yugabyte.yw.commissioner.AbstractTaskBase;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;

import junitparams.JUnitParamsRunner;
import junitparams.Parameters;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.*;
import static org.hamcrest.CoreMatchers.*;
import static org.junit.Assert.*;

@RunWith(JUnitParamsRunner.class)
public class AlertTest extends FakeDBApplication {
  private Customer cust1;
  private Customer cust2;

  @Before
  public void setUp() {
    cust1 = ModelFactory.testCustomer("Customer 1");
    cust2 = ModelFactory.testCustomer("Customer 2");
  }

  @Test
  public void testAdd() {
      Alert alert = Alert.create(cust1.uuid, "TEST_ALERT_1", "Warning", "Testing alert.");
      assertNotNull(alert.uuid);
      assertEquals(cust1.uuid, alert.customerUUID);
      assertEquals("Warning", alert.type);
      assertEquals("Testing alert.", alert.message);
  }

  @Test
  public void testAlertsSameCustomer() {
    Alert alert1 = Alert.create(cust1.uuid, "TEST_ALERT_1", "Warning", "Testing alert 1.");
    Alert alert2 = Alert.create(cust1.uuid, "TEST_ALERT_2", "Warning", "Testing alert 2.");
    List<Alert> cust1Alerts = Alert.list(cust1.uuid);
    List<Alert> cust2Alerts = Alert.list(cust2.uuid);

    assertThat(cust1Alerts, hasItem(alert1));
    assertThat(cust1Alerts, hasItem(alert2));

    assertThat(cust2Alerts, not(hasItem(alert1)));
    assertThat(cust2Alerts, not(hasItem(alert2)));
  }

  @Test
  public void testAlertsDiffCustomer() {
    Alert alert1 = Alert.create(cust1.uuid, "TEST_ALERT_1", "Warning", "Testing alert 1.");
    Alert alert2 = Alert.create(cust2.uuid, "TEST_ALERT_2", "Warning", "Testing alert 2.");
    List<Alert> cust1Alerts = Alert.list(cust1.uuid);
    List<Alert> cust2Alerts = Alert.list(cust2.uuid);

    assertThat(cust1Alerts, hasItem(alert1));
    assertThat(cust1Alerts, not(hasItem(alert2)));

    assertThat(cust2Alerts, not(hasItem(alert1)));
    assertThat(cust2Alerts, hasItem(alert2));
  }

  @Test
  @Parameters(method = "parametersToTestAlertsTypes")
  public void testAlertsTypes(Alert.TargetType alertType, UUID uuid, Class claz) {
    Alert alert = Alert.create(cust1.uuid, uuid, alertType,
                               "TEST_ALERT_1", "Warning", "Testing alert.");
    assertNotNull(alert.uuid);
    assertEquals(cust1.uuid, alert.customerUUID);
    assertEquals(uuid, alert.targetUUID);
    assertEquals("Warning", alert.type);
    assertEquals("Testing alert.", alert.message);
  }

  private Object[] parametersToTestAlertsTypes() {
    // @formatter:off
    return new Object[] {
        new Object[] { Alert.TargetType.TaskType, UUID.randomUUID(), AbstractTaskBase.class },
        new Object[] { Alert.TargetType.UniverseType, UUID.randomUUID(), Universe.class },
    };
    // @formatter:on
  }
}
