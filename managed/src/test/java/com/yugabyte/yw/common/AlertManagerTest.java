// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import static org.mockito.Mockito.anyString;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import java.util.Collections;
import java.util.UUID;

import javax.mail.MessagingException;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;

import com.yugabyte.yw.forms.CustomerRegisterFormData.SmtpData;
import com.yugabyte.yw.models.Alert;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;

@RunWith(MockitoJUnitRunner.class)
public class AlertManagerTest extends FakeDBApplication {

  private static final String ALERT_TEST_MESSAGE = "Test message";

  private Customer defaultCustomer;

  @Mock
  private EmailHelper emailHelper;

  @InjectMocks
  private AlertManager am;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
  }

  @Test
  public void testSendEmail_DoesntFail_UniverseRemoved() throws MessagingException {
    doTestSendEmail(UUID.randomUUID(), String.format("Common failure for customer '%s':\n%s.",
        defaultCustomer.name, ALERT_TEST_MESSAGE));
  }

  @Test
  public void testSendEmail_UniverseExists() throws MessagingException {
    Universe u = ModelFactory.createUniverse();
    doTestSendEmail(u.universeUUID,
        String.format("Common failure for universe '%s':\n%s.", u.name, ALERT_TEST_MESSAGE));
  }

  private void doTestSendEmail(UUID universeUUID, String expectedContent)
      throws MessagingException {
    Alert alert = Alert.create(defaultCustomer.uuid, universeUUID, Alert.TargetType.UniverseType,
        "errorCode", "Warning", ALERT_TEST_MESSAGE);
    alert.sendEmail = true;

    String destination = "to@to.com";
    SmtpData smtpData = new SmtpData();
    when(emailHelper.getDestinations(defaultCustomer.uuid))
        .thenReturn(Collections.singletonList(destination));
    when(emailHelper.getSmtpData(defaultCustomer.uuid)).thenReturn(smtpData);

    am.sendEmail(alert, "test state");

    verify(emailHelper, times(1)).sendEmail(eq(defaultCustomer), anyString(), eq(destination),
        eq(smtpData),
        eq(Collections.singletonMap("text/plain; charset=\"us-ascii\"", expectedContent)));
  }
}
