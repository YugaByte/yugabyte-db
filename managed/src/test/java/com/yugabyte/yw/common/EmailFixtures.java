// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.icegreen.greenmail.util.GreenMail;
import com.icegreen.greenmail.util.ServerSetup;
import com.yugabyte.yw.forms.CustomerRegisterFormData;
import com.yugabyte.yw.forms.CustomerRegisterFormData.SmtpData;

public class EmailFixtures {
  public static final String EMAIL_USER_NAME = "Billy";

  public static final String EMAIL_USER_PASSWORD = "1234-4321";

  public static final String EMAIL_USER_ADDRESS = "from@mail.com";

  public static final String EMAIL_HOST = "127.0.0.1";

  public static final int EMAIL_TEST_PORT = 2525;

  public static final int EMAIL_TEST_PORT_SSL = ServerSetup.PORT_SMTPS;

  public static GreenMail setupMailServer(String smtpHost, int smtpPort) {
    return setupMailServer(smtpHost, smtpPort, EMAIL_USER_ADDRESS, EMAIL_USER_NAME,
        EMAIL_USER_PASSWORD);
  }

  public static GreenMail setupMailServer(String smtpHost, int smtpPort, String EmailFrom,
      String userName, String userPassword) {
    GreenMail mailServer = new GreenMail(new ServerSetup(smtpPort, smtpHost, "smtp"));
    mailServer.start();
    mailServer.setUser(EmailFrom, userName, userPassword);
    return mailServer;
  }

  public static SmtpData createSmtpData() {
    CustomerRegisterFormData.SmtpData smtpData = new CustomerRegisterFormData.SmtpData();
    smtpData.smtpUsername = EMAIL_USER_NAME;
    smtpData.smtpPassword = EMAIL_USER_PASSWORD;
    smtpData.smtpServer = EMAIL_HOST;
    smtpData.smtpPort = EMAIL_TEST_PORT;
    smtpData.emailFrom = EMAIL_USER_ADDRESS;
    smtpData.useSSL = false;
    smtpData.useTLS = false;
    return smtpData;
  }
}
