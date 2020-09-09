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
import java.util.Date;
import java.util.HashMap;
import java.util.stream.Collectors;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.google.inject.Inject;
import com.google.inject.Singleton;

import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.HealthManager;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.forms.NodeInstanceFormData;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.CustomerRegisterFormData.AlertingData;
import com.yugabyte.yw.forms.CustomerRegisterFormData.SmtpData;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.HealthCheck;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.common.Util;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.api.Play;
import play.Configuration;
import play.libs.Json;

import akka.actor.ActorSystem;
import com.google.common.annotations.VisibleForTesting;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.TimeUnit;
import play.Environment;
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

  play.Configuration config;

  private long HEALTH_CHECK_SLEEP_INTERVAL_MS = 0;

  private long STATUS_UPDATE_INTERVAL_MS = 0;

  // Last time we sent a status update email per customer.
  private Map<UUID, Long> lastStatusUpdateTimeMap = new HashMap<>();

  // Last time we actually ran the health check script per customer.
  private Map<UUID, Long> lastCheckTimeMap = new HashMap<>();

  // What will run the health checking script.
  HealthManager healthManager;

  private Gauge healthMetric = null;

  private AtomicBoolean running = new AtomicBoolean(false);

  private final ActorSystem actorSystem;

  private final ExecutionContext executionContext;

  private final Environment environment;

  private CollectorRegistry promRegistry;

  public HealthChecker(
      ActorSystem actorSystem,
      Configuration config,
      Environment environment,
      ExecutionContext executionContext,
      HealthManager healthManager,
      CollectorRegistry promRegistry) {
    this.actorSystem = actorSystem;
    this.config = config;
    this.environment = environment;
    this.executionContext = executionContext;
    this.healthManager = healthManager;
    this.promRegistry = promRegistry;

    this.initialize();
  }


  @Inject
  public HealthChecker(
      ActorSystem actorSystem,
      Configuration config,
      Environment environment,
      ExecutionContext executionContext,
      HealthManager healthManager) {
        this(actorSystem, config, environment, executionContext,
             healthManager, CollectorRegistry.defaultRegistry);
  }

  private void initialize() {
    LOG.info("Scheduling health checker every " + this.healthCheckIntervalMs() + " ms");
    this.actorSystem.scheduler().schedule(
      Duration.create(0, TimeUnit.MILLISECONDS), // initialDelay
      Duration.create(this.healthCheckIntervalMs(), TimeUnit.MILLISECONDS), // interval
      () -> scheduleRunner(),
      this.executionContext
    );

    try {
      healthMetric = Gauge.build(kUnivMetricName, "Boolean result of health checks").
                           labelNames(kUnivUUIDLabel, kUnivNameLabel, kNodeLabel, kCheckLabel).
                           register(this.promRegistry);
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

  private String ybAlertEmail() {
    return config.getString("yb.health.default_email");
  }

  private void processResults(Universe u, String response) {
    Boolean hasErrors = false;
    try {
      JsonNode healthJSON = Util.convertStringToJson(response);
      if (healthJSON.get("mail_error") != null) {
         LOG.warn("Health check had the following errors during mailing: " +
                  healthJSON.path("mail_error").asText());
      }
      for (JsonNode entry : healthJSON.path("data")) {
        String nodeName = entry.path("node").asText();
        String checkName = entry.path("message").asText();
        Boolean checkResult = entry.path("has_error").asBoolean();
        hasErrors = checkResult || hasErrors;
        if (null == healthMetric)
          continue;

        Gauge.Child prometheusVal = healthMetric.labels(
          u.universeUUID.toString(),
          u.name,
          nodeName,
          checkName
        );
        prometheusVal.set(checkResult ? 1 : 0);
      }
      LOG.info("Health check for universe " + u.name +
               (hasErrors ? " reported errors." : " reported success."));
     } catch (Exception e) {
      LOG.warn("Failed to convert health check response to prometheus metrics " + e.getMessage());
    }
  }

  @VisibleForTesting
  void scheduleRunner() {
    if (running.get()) {
      LOG.info("Previous run still underway");
      return;
    }

    LOG.info("Running health checker");
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
    boolean shouldRunCheck = (now - checkIntervalMs) >
        lastCheckTimeMap.getOrDefault(c.uuid, 0l);
    long statusUpdateIntervalMs = alertingData.statusUpdateIntervalMs <= 0
      ? statusUpdateIntervalMs()
      : alertingData.statusUpdateIntervalMs;
    boolean shouldSendStatusUpdate = (now - statusUpdateIntervalMs) >
        lastStatusUpdateTimeMap.getOrDefault(c.uuid, 0l);
    // Always do a check if it's time for a status update OR if it's time for a check.
    if (shouldSendStatusUpdate || shouldRunCheck) {
      // Since we'll do a check, update this all the time.
      lastCheckTimeMap.put(c.uuid, now);
      if (shouldSendStatusUpdate) {
        lastStatusUpdateTimeMap.put(c.uuid, now);
      }
      CustomerConfig smtpConfig = CustomerConfig.getSmtpConfig(c.uuid);
      SmtpData smtpData = null;
      if (smtpConfig != null) {
        smtpData =  Json.fromJson(smtpConfig.data, SmtpData.class);
      }
      checkAllUniverses(c, config, shouldSendStatusUpdate, smtpData);
    }
  }

  public void checkAllUniverses(
      Customer c, CustomerConfig config, boolean shouldSendStatusUpdate, SmtpData smtpData) {
    // Process all of a customer's universes.
    for (Universe u : c.getUniverses()) {
      try {
        checkSingleUniverse(u, c, config, shouldSendStatusUpdate, smtpData);
      } catch (Exception ex) {
        LOG.error("Error running health check for universe " + u.universeUUID, ex);
      }
     }
  }

  public void checkSingleUniverse(Universe u, Customer c, CustomerConfig config,
                                  boolean shouldSendStatusUpdate, SmtpData smtpData) {
    // Validate universe data and make sure nothing is in progress.
    UniverseDefinitionTaskParams details = u.getUniverseDetails();
    if (details == null) {
      LOG.warn("Skipping universe " + u.name + " due to invalid details json...");
      return;
    }
    if (details.updateInProgress) {
      LOG.warn("Skipping universe " + u.name + " due to task in progress...");
      return;
    }
    LOG.info("Doing health check for universe: " + u.name);
    Map<UUID, HealthManager.ClusterInfo> clusterMetadata = new HashMap<>();
    boolean invalidUniverseData = false;
    String providerCode = "";
    for (UniverseDefinitionTaskParams.Cluster cluster : details.clusters) {
      HealthManager.ClusterInfo info = new HealthManager.ClusterInfo();
      clusterMetadata.put(cluster.uuid, info);
      info.ybSoftwareVersion = cluster.userIntent.ybSoftwareVersion;
      info.enableYSQL = cluster.userIntent.enableYSQL;
      // Since health checker only uses CQLSH, we only care about the
      // client to node encryption flag.
      info.enableTlsClient = cluster.userIntent.enableClientToNodeEncrypt;

      Provider provider = Provider.get(UUID.fromString(cluster.userIntent.provider));
      if (provider == null) {
        LOG.warn("Skipping universe " + u.name + " due to invalid provider "
            + cluster.userIntent.provider);
        invalidUniverseData = true;
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
        info.redisPort = nd.redisServerRpcPort;
        break;
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
    List<String> destinations = new ArrayList<String>();
    AlertingData alertingData = null;
    if (config != null) {
      alertingData = Json.fromJson(config.data, AlertingData.class);
      String ybEmail = ybAlertEmail();
      if (alertingData.sendAlertsToYb && ybEmail != null && !ybEmail.isEmpty()) {
        destinations.add(ybEmail);
      }
      if (alertingData.alertingEmail != null && !alertingData.alertingEmail.isEmpty()) {
        destinations.add(alertingData.alertingEmail);
      }
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
    // Setup customer tag including name and code, for ease of email parsing.
    String customerTag = String.format("[%s][%s]", c.name, c.code);
    Provider mainProvider = Provider.get(UUID.fromString(
          details.getPrimaryCluster().userIntent.provider));

    String disabledUntilStr = u.getConfig().getOrDefault(Universe.DISABLE_ALERTS_UNTIL, "0");
    long disabledUntilSecs = 0;
    try {
      disabledUntilSecs = Long.parseLong(disabledUntilStr);
    } catch (NumberFormatException ne) {
      LOG.warn("invalid universe config for disabled alerts: [ " + disabledUntilStr + " ]");
      disabledUntilSecs = 0;
    }
    boolean silenceEmails = ((System.currentTimeMillis() / 1000) <= disabledUntilSecs);

    boolean reportOnlyErrors = !shouldSendStatusUpdate &&
                               null != alertingData &&
                               alertingData.reportOnlyErrors;
    boolean sendMailAlways = (shouldSendStatusUpdate || lastCheckHadErrors);
    // Call devops and process response.
    ShellProcessHandler.ShellResponse response = healthManager.runCommand(
        mainProvider,
        new ArrayList(clusterMetadata.values()),
        u.name,
        customerTag,
        (destinations.size() == 0 || silenceEmails) ? null : String.join(",", destinations),
        potentialStartTime,
        sendMailAlways,
        reportOnlyErrors,
        smtpData
    );

    if (response.code == 0) {
      processResults(u, response.message);
      HealthCheck.addAndPrune(u.universeUUID, u.customerId, response.message);
    } else {
      LOG.error(String.format(
          "Health check script got error: code (%s)\n%s", response.code, response.message));
    }
  }
}
