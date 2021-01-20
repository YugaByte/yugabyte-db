// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.forms;


import play.data.validation.Constraints;
import java.util.Map;

/**
 * This class will be used by the API and UI Form Elements to validate constraints are met
 */
public class AlertingFormData {
    @Constraints.MaxLength(15)
    public String code;

    public String email;

    public String password;

    public String confirmPassword;

    public String name;

    public Map features;

    static public class AlertingData {
        @Constraints.Email
        @Constraints.MinLength(5)
        public String alertingEmail;

        public boolean sendAlertsToYb = false;

        public long checkIntervalMs = 0;

        public long statusUpdateIntervalMs = 0;

        public Boolean reportOnlyErrors = false;

        public Boolean reportBackupFailures = false;
    }

    static public class SmtpData {
        public String smtpServer = null;

        public int smtpPort = -1;

        public String emailFrom = null;

        public String smtpUsername = null;

        public String smtpPassword = null;

        public boolean useSSL = true;

        public boolean useTLS = false;
      }

    public AlertingData alertingData;
    public SmtpData smtpData;

    @Constraints.Pattern(message="Must be one of NONE, LOW, MEDIUM, HIGH",
                         value="\\b(?:NONE|LOW|MEDIUM|HIGH)\\b")
    public String callhomeLevel = "MEDIUM";
}
