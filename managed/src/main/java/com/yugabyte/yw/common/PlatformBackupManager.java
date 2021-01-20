/*
 * Copyright 2020 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common;

import akka.actor.ActorSystem;
import akka.actor.Cancellable;
import com.google.inject.Inject;
import com.google.inject.Singleton;
import com.typesafe.config.Config;
import scala.concurrent.ExecutionContext;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

@Singleton
public class PlatformBackupManager extends DevopsBase {
  static final String BACKUP_SCRIPT = "bin/yb_platform_backup.sh";
  static final String BACKUP_OUTPUT_DIR = "platformBackups";
  static final String PROMETHEUS_HOST_CONFIG_KEY = "yb.metrics.host";
  static final String DB_USERNAME_CONFIG_KEY = "db.default.username";
  static final String DB_PASSWORD_CONFIG_KEY = "db.default.password";
  static final String DB_HOST_CONFIG_KEY = "db.default.host";
  static final String DB_PORT_CONFIG_KEY = "db.default.port";
  static final String BACKUP_FREQUENCY_KEY = "yb.platform_backup_frequency";
  static final String BACKUP_OUTPUT_DIR_KEY = "yb.storage.path";
  static final String DB_PASSWORD_ENV_VAR_KEY = "PGPASSWORD";

  // The akka schedule to periodically take a backup of the platform.
  private Cancellable schedule;

  private final ActorSystem actorSystem;

  private final ExecutionContext executionContext;

  @Override
  protected String getCommandType() {
    return null;
  }

  @Inject
  public PlatformBackupManager(
    Config config,
    ActorSystem actorSystem,
    ExecutionContext executionContext,
    ShellProcessHandler shellProcessHandler
  ) {
    this.config = config;
    this.actorSystem = actorSystem;
    this.executionContext = executionContext;
    this.shellProcessHandler = shellProcessHandler;
  }

  public void start() {
    final Duration frequency = config.getDuration(BACKUP_FREQUENCY_KEY);
    if (!frequency.isNegative() && !frequency.isZero()) {
      setSchedule(frequency);
    }
  }

  public void setSchedule(long frequencyMin) {
    setSchedule(Duration.ofMinutes(frequencyMin));
  }

  private void setSchedule(Duration frequency) {
    LOG.info("Scheduling periodic platform backups every {}", frequency.toString());
    cancelSchedule();
    schedule = this.actorSystem.scheduler().schedule(
      Duration.ofMillis(0), // initialDelay
      frequency, // interval
      this::createBackup,
      this.executionContext
    );
  }

  public void cancelSchedule() {
    if (schedule != null && !schedule.isCancelled()) {
      schedule.cancel();
    }
  }

  static Path getOutputDir(Config config) {
    return Paths.get(config.getString(BACKUP_OUTPUT_DIR_KEY), BACKUP_OUTPUT_DIR);
  }

  private static abstract class PlatformBackupParams {
    // The addr that the prometheus server is running on.
    private final String prometheusHost;
    // The username that YW uses to connect to it's DB.
    private final String dbUsername;
    // The password that YW uses to authenticate connections to it's DB.
    private final String dbPassword;
    // The addr that the DB is listening to connection requests on.
    private final String dbHost;
    // The port that the DB is listening to connection requests on.
    private final int dbPort;

    protected PlatformBackupParams(Config config) {
      this.prometheusHost = config.getString(PROMETHEUS_HOST_CONFIG_KEY);
      this.dbUsername = config.getString(DB_USERNAME_CONFIG_KEY);
      this.dbPassword = config.getString(DB_PASSWORD_CONFIG_KEY);
      this.dbHost = config.getString(DB_HOST_CONFIG_KEY);
      this.dbPort = config.getInt(DB_PORT_CONFIG_KEY);
    }

    protected abstract List<String> getCommandSpecificArgs();

    public List<String> getCommandArgs() {
      final List<String> commandArgs = new ArrayList<>();
      commandArgs.add(BACKUP_SCRIPT);
      commandArgs.addAll(getCommandSpecificArgs());
      commandArgs.add("--db_username");
      commandArgs.add(dbUsername);
      commandArgs.add("--db_host");
      commandArgs.add(dbHost);
      commandArgs.add("--db_port");
      commandArgs.add(Integer.toString(dbPort));
      commandArgs.add("--prometheus_host");
      commandArgs.add(prometheusHost);
      commandArgs.add("--verbose");

      return commandArgs;
    }

    public Map<String, String> getExtraVars() {
      final Map<String, String> extraVars = new HashMap<>();
      if (!dbPassword.isEmpty()) {
        // Add PGPASSWORD env var to skip having to enter the db password for pg_dump/pg_restore.
        extraVars.put(DB_PASSWORD_ENV_VAR_KEY, dbPassword);
      }

      return extraVars;
    }
  }

  private static class CreatePlatformBackupParams extends PlatformBackupParams {
    // Whether to exclude prometheus metric data from the backup or not.
    private final boolean excludePrometheus;
    // Whether to exclude the YB release binaries from the backup or not.
    private final boolean excludeReleases;
    // Where to output the platform backup
    private final String outputDirectory;

    public CreatePlatformBackupParams(Config config) {
      super(config);
      this.excludePrometheus = true;
      this.excludeReleases = true;
      this.outputDirectory = getOutputDir(config).toAbsolutePath().toString();
    }

    @Override
    protected List<String> getCommandSpecificArgs() {
      final List<String> commandArgs = new ArrayList<>();
      commandArgs.add("create");

      if (excludePrometheus) {
        commandArgs.add("--exclude_prometheus");
      }

      if (excludeReleases) {
        commandArgs.add("--exclude_releases");
      }

      commandArgs.add("--output");
      commandArgs.add(outputDirectory);

      return commandArgs;
    }
  }

  private static class RestorePlatformBackupParams extends PlatformBackupParams {
    // Where to input a previously taken platform backup from.
    private final String input;

    public RestorePlatformBackupParams(Config config, String input) {
      super(config);
      this.input = input;
    }

    @Override
    protected List<String> getCommandSpecificArgs() {
      final List<String> commandArgs = new ArrayList<>();
      commandArgs.add("restore");
      commandArgs.add("--input");
      commandArgs.add(input);

      return commandArgs;
    }
  }

  private synchronized <T extends PlatformBackupParams> ShellResponse runCommand(T params) {
    final List<String> commandArgs = params.getCommandArgs();
    final Map<String, String> extraVars = params.getExtraVars();

    LOG.info("Command to run: [" + String.join(" ", commandArgs) + "]");
    return shellProcessHandler.run(commandArgs, extraVars);
  }

  /**
   * Create a backup of the Yugabyte Platform
   * @return the output/results of running the script
   */
  public boolean createBackup() {
    LOG.info("Creating platform backup...");

    ShellResponse response = runCommand(new CreatePlatformBackupParams(config));
    if (response.code != 0) {
      LOG.error("Backup failed: " + response.message);
    }

    return response.code == 0;
  }

  /**
   * Restore a backup of the Yugabyte Platform
   * @param input is the path to the backup to be restored
   * @return the output/results of running the script
   */
  public boolean restoreBackup(String input) {
    LOG.info("Restoring platform backup...");

    ShellResponse response = runCommand(new RestorePlatformBackupParams(config, input));
    if (response.code != 0) {
      LOG.error("Restore failed: " + response.message);
    }

    return response.code == 0;
  }
}
