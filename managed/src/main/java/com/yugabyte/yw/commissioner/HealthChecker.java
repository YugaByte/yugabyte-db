/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner;

import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import javax.mail.MessagingException;

import java.util.Date;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.google.inject.Inject;
import com.google.inject.Singleton;

import com.yugabyte.yw.common.*;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.CustomerRegisterFormData.AlertingData;
import com.yugabyte.yw.forms.CustomerRegisterFormData.SmtpData;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Alert;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.HealthCheck;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;

import org.apache.commons.lang3.StringUtils;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.Configuration;
import play.libs.Json;

import akka.actor.ActorSystem;
import com.google.common.annotations.VisibleForTesting;
import java.util.concurrent.TimeUnit;
import scala.concurrent.ExecutionContext;
import scala.concurrent.duration.Duration;

import io.prometheus.client.Gauge;
import io.prometheus.client.CollectorRegistry;

import com.fasterxml.jackson.databind.JsonNode;

@Singleton
public class HealthChecker {
  public static final Logger LOG = LoggerFactory.getLogger(HealthChecker.class);

  public static final String kUnivMetricName = "yb_univ_health_status";
  public static final String kUnivUUIDLabel = "univ_uuid";
  public static final String kUnivNameLabel = "univ_name";
  public static final String kCheckLabel = "check_name";
  public static final String kNodeLabel = "node";

  private static final String ALERT_ERROR_CODE = "HEALTH_CHECKER_FAILURE";

  play.Configuration config;

  // Last time we sent a status update email per customer.
  private final Map<UUID, Long> lastStatusUpdateTimeMap = new HashMap<>();

  // Last time we actually ran the health check script per customer.
  private final Map<UUID, Long> lastCheckTimeMap = new HashMap<>();

  // What will run the health checking script.
  HealthManager healthManager;

  private Gauge healthMetric = null;

  private final AtomicBoolean running = new AtomicBoolean(false);

  private final ActorSystem actorSystem;

  private final ExecutionContext executionContext;

  private final CollectorRegistry promRegistry;

  private HealthCheckerReport healthCheckerReport;

  private EmailHelper emailHelper;

  @VisibleForTesting
  HealthChecker(
      ActorSystem actorSystem,
      Configuration config,
      ExecutionContext executionContext,
      HealthManager healthManager,
      CollectorRegistry promRegistry,
      HealthCheckerReport healthCheckerReport,
      EmailHelper emailHelper) {
    this.actorSystem = actorSystem;
    this.config = config;
    this.executionContext = executionContext;
    this.healthManager = healthManager;
    this.promRegistry = promRegistry;
    this.healthCheckerReport = healthCheckerReport;
    this.emailHelper = emailHelper;

    this.initialize();
  }


  @Inject
  public HealthChecker(
    ActorSystem actorSystem,
    Configuration config,
    ExecutionContext executionContext,
    HealthManager healthManager,
    HealthCheckerReport healthCheckerReport,
    EmailHelper emailHelper) {
    this(actorSystem, config, executionContext, healthManager,
        CollectorRegistry.defaultRegistry, healthCheckerReport,
        emailHelper);
  }

  private void initialize() {
    LOG.info("Scheduling health checker every " + this.healthCheckIntervalMs() + " ms");
    this.actorSystem.scheduler().schedule(
      Duration.create(0, TimeUnit.MILLISECONDS), // initialDelay
      Duration.create(this.healthCheckIntervalMs(), TimeUnit.MILLISECONDS), // interval
      this::scheduleRunner,
      this.executionContext
    );

    try {
      healthMetric = Gauge.build(kUnivMetricName, "Boolean result of health checks")
        .labelNames(kUnivUUIDLabel, kUnivNameLabel, kNodeLabel, kCheckLabel)
        .register(this.promRegistry);
    } catch (IllegalArgumentException e) {
      LOG.warn("Failed to build prometheus gauge for name: " + kUnivMetricName);
    }

  }

  // The interval at which the checker will run.
  // Can be overridden per customer.
  private long healthCheckIntervalMs() {
    Long interval = config.getLong("yb.health.check_interval_ms");
    return interval == null ? 0 : interval;
  }

  // The interval at which to send a status update of all the current universes.
  // Can be overridden per customer.
  private long statusUpdateIntervalMs() {
    Long interval = config.getLong("yb.health.status_interval_ms");
    return interval == null ? 0 : interval;
  }

  private void processResults(Customer c, Universe u, String response, long durationMs,
      String emailDestinations, boolean sendMailAlways, boolean reportOnlyErrors) {

    JsonNode healthJSON = null;
    try {
      healthJSON = Util.convertStringToJson(response);
    } catch (Exception e) {
      LOG.warn("Failed to convert health check response to JSON " + e.getMessage());
      createAlert(c, u, "Error converting health check response to JSON: " + e.getMessage());
    }

    if (healthJSON != null) {
      boolean hasErrors = false;
      try {
        for (JsonNode entry : healthJSON.path("data")) {
          String nodeName = entry.path("node").asText();
          String checkName = entry.path("message").asText();
          boolean checkResult = entry.path("has_error").asBoolean();
          hasErrors = checkResult || hasErrors;
          if (null == healthMetric) continue;

          Gauge.Child prometheusVal = healthMetric.labels(u.universeUUID.toString(), u.name,
              nodeName, checkName);
          prometheusVal.set(checkResult ? 1 : 0);
        }
        LOG.info("Health check for universe {} reported {}. [ {} ms ]", u.name,
            (hasErrors ? "errors" : " success"), durationMs);

      } catch (Exception e) {
        LOG.warn("Failed to convert health check response to prometheus metrics " + e.getMessage());
        createAlert(c, u,
            "Error converting health check response to prometheus metrics: " + e.getMessage());
      }

      SmtpData smtpData = emailHelper.getSmtpData(c.uuid);
      if (!StringUtils.isEmpty(emailDestinations) && (smtpData != null)
          && (sendMailAlways || hasErrors)) {
        String subject = String.format("%s - <%s> %s", hasErrors ? "ERROR" : "OK", c.getTag(),
            u.name);
        String mailError = sendEmailReport(u, c, smtpData, emailDestinations, subject, healthJSON,
            reportOnlyErrors);
        if (mailError != null) {
          LOG.warn("Health check had the following errors during mailing: " + mailError);
          createAlert(c, u, "Error sending Health check email: " + mailError);
        }
      }
    }
  }

  private String sendEmailReport(Universe u, Customer c, SmtpData smtpData,
      String emailDestinations, String subject, JsonNode report, boolean reportOnlyErrors) {

    // LinkedHashMap saves values order.
    Map<String, String> contentMap = new LinkedHashMap<>();
    contentMap.put("text/plain; charset=\"us-ascii\"",
        healthCheckerReport.asPlainText(report, reportOnlyErrors));
    contentMap.put("text/html; charset=\"us-ascii\"",
        healthCheckerReport.asHtml(u, report, reportOnlyErrors));

    try {
      emailHelper.sendEmail(c, subject, emailDestinations, smtpData, contentMap);
    } catch (MessagingException e) {
      return e.getMessage();
    }

    return null;
  }

  @VisibleForTesting
  void scheduleRunner() {
    if (running.get()) {
      LOG.info("Previous run of health checker is still underway");
      return;
    }

    LOG.info("Started running health checker");
    running.set(true);
    // TODO(bogdan): This will not be too DB friendly when we go multi-tenant.
    for (Customer c : Customer.getAll()) {
      try {
        checkCustomer(c);
      } catch (Exception ex) {
        LOG.error("Error running health check for customer " + c.uuid, ex);
      }
    }

    LOG.info("Completed running health checker.");
    running.set(false);
  }

  public void checkCustomer(Customer c) {
    // We need an alerting config to do work.
    CustomerConfig config = CustomerConfig.getAlertConfig(c.uuid);
    if (config == null) {
      LOG.info("Skipping customer " + c.uuid + " due to missing alerting config...");
      return;
    }

    AlertingData alertingData = Json.fromJson(config.data, AlertingData.class);
    long now = (new Date()).getTime();
    long checkIntervalMs = alertingData.checkIntervalMs <= 0
      ? healthCheckIntervalMs()
      : alertingData.checkIntervalMs;
    boolean shouldRunCheck = (now - checkIntervalMs) > lastCheckTimeMap.getOrDefault(c.uuid, 0L);
    long statusUpdateIntervalMs = alertingData.statusUpdateIntervalMs <= 0
      ? statusUpdateIntervalMs()
      : alertingData.statusUpdateIntervalMs;
    boolean shouldSendStatusUpdate = (now - statusUpdateIntervalMs) >
        lastStatusUpdateTimeMap.getOrDefault(c.uuid, 0L);
    // Always do a check if it's time for a status update OR if it's time for a check.
    if (shouldSendStatusUpdate || shouldRunCheck) {
      // Since we'll do a check, update this all the time.
      lastCheckTimeMap.put(c.uuid, now);
      if (shouldSendStatusUpdate) {
        lastStatusUpdateTimeMap.put(c.uuid, now);
      }
      checkAllUniverses(c, config, shouldSendStatusUpdate);
    }
  }

  private void createAlert(Customer c, Universe u, String details) {
    Alert.create(c.uuid, u.universeUUID, Alert.TargetType.UniverseType, ALERT_ERROR_CODE, "Warning",
        details, true, null);
  }

  public void checkAllUniverses(Customer c, CustomerConfig config, boolean shouldSendStatusUpdate) {

    AlertingData alertingData = config != null ? Json.fromJson(config.data, AlertingData.class)
        : null;
    boolean reportOnlyErrors = !shouldSendStatusUpdate && alertingData != null
        && alertingData.reportOnlyErrors;

    // Process all of a customer's universes.
    for (Universe u : c.getUniverses()) {
      String destinations = getAlertDestinations(u, c);
      try {
        checkSingleUniverse(u, c, shouldSendStatusUpdate, reportOnlyErrors, destinations);
      } catch (Exception ex) {
        LOG.error("Error running health check for universe " + u.universeUUID, ex);
        createAlert(c, u, "Error running health check: " + ex.getMessage());
      }
    }
  }

  private String getAlertDestinations(Universe u, Customer c) {
    List<String> destinations = emailHelper.getDestinations(c.uuid);
    if (destinations.size() == 0) {
      return null;
    }

    String disabledUntilStr = u.getConfig().getOrDefault(Universe.DISABLE_ALERTS_UNTIL, "0");
    long disabledUntilSecs = 0;
    try {
      disabledUntilSecs = Long.parseLong(disabledUntilStr);
    } catch (NumberFormatException ne) {
      LOG.warn("invalid universe config for disabled alerts: [ " + disabledUntilStr + " ]");
    }

    boolean silenceEmails = ((System.currentTimeMillis() / 1000) <= disabledUntilSecs);
    return silenceEmails ? null : String.join(",", destinations);
  }

  public void checkSingleUniverse(Universe u, Customer c, boolean shouldSendStatusUpdate,
      boolean reportOnlyErrors, String emailDestinations) {
    // Validate universe data and make sure nothing is in progress.
    UniverseDefinitionTaskParams details = u.getUniverseDetails();
    if (details == null) {
      LOG.warn("Skipping universe " + u.name + " due to invalid details json...");
      createAlert(c, u, "Health check skipped due to invalid details json.");
      return;
    }
    if (details.updateInProgress) {
      LOG.warn("Skipping universe " + u.name + " due to task in progress...");
      return;
    }
    long startMs = System.currentTimeMillis();
    LOG.info("Doing health check for universe: " + u.name);

    Map<UUID, HealthManager.ClusterInfo> clusterMetadata = new HashMap<>();
    boolean invalidUniverseData = false;
    String providerCode;
    for (UniverseDefinitionTaskParams.Cluster cluster : details.clusters) {
      HealthManager.ClusterInfo info = new HealthManager.ClusterInfo();
      clusterMetadata.put(cluster.uuid, info);
      info.ybSoftwareVersion = cluster.userIntent.ybSoftwareVersion;
      info.enableYSQL = cluster.userIntent.enableYSQL;
      info.enableYEDIS = cluster.userIntent.enableYEDIS;
      // Since health checker only uses CQLSH, we only care about the
      // client to node encryption flag.
      info.enableTlsClient = cluster.userIntent.enableClientToNodeEncrypt;

      Provider provider = Provider.get(UUID.fromString(cluster.userIntent.provider));
      if (provider == null) {
        LOG.warn("Skipping universe " + u.name + " due to invalid provider "
            + cluster.userIntent.provider);
        invalidUniverseData = true;
        createAlert(c, u, "Health check skipped due to invalid provider data.");
        break;
      }

      providerCode = provider.code;
      if (providerCode.equals(Common.CloudType.kubernetes.toString())) {
        info.namespaceToConfig = PlacementInfoUtil.getConfigPerNamespace(
            cluster.placementInfo, details.nodePrefix, provider);
      }

      AccessKey accessKey = AccessKey.get(provider.uuid, cluster.userIntent.accessKeyCode);
      if (accessKey == null || accessKey.getKeyInfo() == null) {
        if (!providerCode.equals(CloudType.kubernetes.toString())) {
          LOG.warn("Skipping universe " + u.name + " due to invalid access key...");
          invalidUniverseData = true;
          createAlert(c, u, "Health check skipped due to invalid access key.");
          break;
        }
      } else {
        info.identityFile = accessKey.getKeyInfo().privateKey;
        info.sshPort = accessKey.getKeyInfo().sshPort;
      }

      if (info.enableYSQL) {
        for (NodeDetails nd : details.nodeDetailsSet) {
          if (nd.isYsqlServer) {
           info.ysqlPort = nd.ysqlServerRpcPort;
           break;
          }
        }
      }

      for (NodeDetails nd : details.nodeDetailsSet) {
        info.ycqlPort = nd.yqlServerRpcPort;
        break;
      }

      if (info.enableYEDIS) {
        for (NodeDetails nd : details.nodeDetailsSet) {
          if (nd.isRedisServer) {
            info.redisPort = nd.redisServerRpcPort;
            break;
          }
        }
      }
    }

    // If any clusters were invalid, abort for this universe.
    if (invalidUniverseData) {
      return;
    }

    for (NodeDetails nd : details.nodeDetailsSet) {
      HealthManager.ClusterInfo info = clusterMetadata.get(nd.placementUuid);
      if (info == null) {
        invalidUniverseData = true;
        LOG.warn(String.format(
              "Universe %s has node %s with invalid placement %s", u.name, nd.nodeName,
              nd.placementUuid));
        createAlert(c, u, String.format("Universe has node %s with invalid placement %s.",
            nd.nodeName, nd.placementUuid));
        break;
      }

      // TODO: we do not have a good way of marking the whole universe as k8s only.
      if (nd.isMaster) {
        info.masterNodes.put(nd.cloudInfo.private_ip, nd.nodeName);
      }

      if (nd.isTserver) {
        info.tserverNodes.put(nd.cloudInfo.private_ip, nd.nodeName);
      }
    }
    // If any nodes were invalid, abort for this universe.
    if (invalidUniverseData) {
      return;
    }

    CustomerTask lastTask = CustomerTask.getLatestByUniverseUuid(u.universeUUID);
    long potentialStartTime = 0;
    if (lastTask != null && lastTask.getCompletionTime()!= null) {
      potentialStartTime = lastTask.getCompletionTime().getTime();
    }

    // If last check had errors, set the flag to send an email. If this check will have an error,
    // we would send an email anyway, but if this check shows a healthy universe, let's send an
    // email about it.
    HealthCheck lastCheck = HealthCheck.getLatest(u.universeUUID);
    boolean lastCheckHadErrors = lastCheck != null && lastCheck.hasError();
    Provider mainProvider = Provider.get(UUID.fromString(
          details.getPrimaryCluster().userIntent.provider));

    // Call devops and process response.
    ShellResponse response = healthManager.runCommand(
        mainProvider,
        new ArrayList<>(clusterMetadata.values()),
        potentialStartTime
    );

    long durationMs = System.currentTimeMillis() - startMs;
    boolean sendMailAlways = (shouldSendStatusUpdate || lastCheckHadErrors);

    if (response.code == 0) {
      processResults(c, u, response.message, durationMs, emailDestinations, sendMailAlways,
          reportOnlyErrors);
      HealthCheck.addAndPrune(u.universeUUID, u.customerId, response.message);
    } else {
      LOG.error("Health check script got error: {} code ({}) [ {} ms ]",
                response.message, response.code, durationMs);
      createAlert(c, u, String.format("Health check script got error: %s code (%d) [ %d ms ]",
          response.message, response.code, durationMs));
    }
  }
}
