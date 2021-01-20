// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.yugabyte.yw.models.Provider;

import com.google.inject.Singleton;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.List;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.libs.Json;

@Singleton
public class HealthManager extends DevopsBase {
  public static final Logger LOG = LoggerFactory.getLogger(HealthManager.class);

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
    public boolean enableYEDIS = false;
    public int redisPort = 6379;
  }

  public ShellResponse runCommand(
    Provider provider,
    List<ClusterInfo> clusters,
    Long potentialStartTimeMs
  ) {
    List<String> commandArgs = new ArrayList<>();

    commandArgs.add(PY_WRAPPER);
    commandArgs.add(HEALTH_CHECK_SCRIPT);

    String description = String.join(" ", commandArgs);

    if (clusters != null) {
      commandArgs.add("--cluster_payload");
      commandArgs.add(Json.stringify(Json.toJson(clusters)));
    }

    if (potentialStartTimeMs > 0) {
      commandArgs.add("--start_time_ms");
      commandArgs.add(String.valueOf(potentialStartTimeMs));
    }

    // Start with a copy of the cloud config env vars.
    HashMap<String, String> extraEnvVars = provider == null ?
      new HashMap<>() : new HashMap<>(provider.getConfig());

    return shellProcessHandler.run(commandArgs, extraEnvVars, false /*logCmdOutput*/, description);
  }

  @Override
  protected String getCommandType() {
    return YB_CLOUD_COMMAND_TYPE;
  }
}
