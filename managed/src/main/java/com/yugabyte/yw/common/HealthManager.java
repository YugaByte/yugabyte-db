// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.yugabyte.yw.forms.CustomerRegisterFormData.SmtpData;
import com.yugabyte.yw.models.Provider;

import com.google.inject.Inject;
import com.google.inject.Singleton;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.List;

import play.libs.Json;

@Singleton
public class HealthManager extends DevopsBase {
  @Inject
  play.Configuration appConfig;

  public static final String HEALTH_CHECK_SCRIPT = "bin/cluster_health.py";

  // TODO: we don't need this?
  private static final String YB_CLOUD_COMMAND_TYPE = "health_check";

  public static class ClusterInfo {
    public String identityFile = null;
    public int sshPort;
    // TODO: this is to be used by k8s.
    // Note: this is the same across all clusters, so maybe we should pull it out one level above.
    public Map<String, String> namespaceToConfig = new HashMap<>();
    public Map<String, String> masterNodes = new HashMap<>();
    public Map<String, String> tserverNodes = new HashMap<>();
    public String ybSoftwareVersion = null;
    public boolean enableTlsClient = false;
    public boolean enableYSQL = false;
    public int ysqlPort = 5433;
    public int ycqlPort = 9042;
    public int redisPort = 6379;
  }

  public ShellProcessHandler.ShellResponse runCommand(
      Provider provider,
      List<ClusterInfo> clusters,
      String universeName,
      String customerTag,
      String destination,
      Long potentialStartTimeMs,
      Boolean sendMailAlways,
      Boolean reportOnlyErrors,
      SmtpData smtpData) {
    List<String> commandArgs = new ArrayList<>();

    commandArgs.add(PY_WRAPPER);
    commandArgs.add(HEALTH_CHECK_SCRIPT);
    commandArgs.add("--cluster_payload");
    commandArgs.add(Json.stringify(Json.toJson(clusters)));
    commandArgs.add("--universe_name");
    commandArgs.add(universeName);
    commandArgs.add("--customer_tag");
    commandArgs.add(customerTag);
    if (destination != null) {
      commandArgs.add("--destination");
      commandArgs.add(destination);
    }
    if (potentialStartTimeMs > 0) {
      commandArgs.add("--start_time_ms");
      commandArgs.add(String.valueOf(potentialStartTimeMs));
    }
    if (sendMailAlways) {
      commandArgs.add("--send_status");
    }
    // Start with a copy of the cloud config env vars.
    HashMap extraEnvVars = new HashMap<>(provider.getConfig());

    String email = appConfig.getString("yb.health.default_email");
    if (smtpData != null) {
      if (smtpData.smtpServer != null) {
        extraEnvVars.put("SMTP_SERVER", smtpData.smtpServer);
      }
      if (smtpData.smtpPort != -1) {
        extraEnvVars.put("SMTP_PORT", String.valueOf(smtpData.smtpPort));
      }
      if (smtpData.emailFrom != null) {
        email = smtpData.emailFrom;
      }
      if (smtpData.smtpUsername != null) {
        extraEnvVars.put("YB_ALERTS_USERNAME", smtpData.smtpUsername);
      }
      if (smtpData.smtpPassword != null) {
        extraEnvVars.put("YB_ALERTS_PASSWORD", smtpData.smtpPassword);
      }
      extraEnvVars.put("SMTP_USE_SSL", String.valueOf(smtpData.useSSL));
      extraEnvVars.put("SMTP_USE_TLS", String.valueOf(smtpData.useTLS));
    } else {
      String emailUsername = appConfig.getString("yb.health.ses_email_username");
      String emailPassword = appConfig.getString("yb.health.ses_email_password");
      if (emailUsername != null) {
        extraEnvVars.put("YB_ALERTS_USERNAME", emailUsername);
      }
      if (emailPassword != null) {
        extraEnvVars.put("YB_ALERTS_PASSWORD", emailPassword);
      }
    }

    if (email != null) {
      extraEnvVars.put("YB_ALERTS_EMAIL", email);
    }
    if (reportOnlyErrors) {
      commandArgs.add("--report_only_errors");
    }

    LOG.info("Command to run: [" + String.join(" ", commandArgs) + "]");
    return shellProcessHandler.run(commandArgs, extraEnvVars, false /*logCmdOutput*/);
  }

  @Override
  protected String getCommandType() {
    return YB_CLOUD_COMMAND_TYPE;
  }
}
