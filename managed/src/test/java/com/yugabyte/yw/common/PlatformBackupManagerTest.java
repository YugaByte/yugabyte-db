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
import com.typesafe.config.Config;
import junit.framework.TestCase;
import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;
import scala.concurrent.ExecutionContext;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static org.mockito.Mockito.*;

@RunWith(JUnitParamsRunner.class)
public class PlatformBackupManagerTest extends TestCase {
  @Mock
  Config mockConfig;

  @Mock
  ActorSystem actorSystem;

  @Mock
  ExecutionContext executionContext;

  @Mock
  ShellProcessHandler shellProcessHandler;

  @Before
  public void setUp() {
    MockitoAnnotations.initMocks(this);
    when(mockConfig.getString("yb.storage.path")).thenReturn("/tmp");
  }

  private void setupConfig(
    String prometheusHost,
    String dbUsername,
    String dbPassword,
    String dbHost,
    int dbPort
  ) {
    when(mockConfig.getString(PlatformBackupManager.PROMETHEUS_HOST_CONFIG_KEY))
      .thenReturn(prometheusHost);
    when(mockConfig.getString(PlatformBackupManager.DB_USERNAME_CONFIG_KEY)).thenReturn(dbUsername);
    when(mockConfig.getString(PlatformBackupManager.DB_PASSWORD_CONFIG_KEY)).thenReturn(dbPassword);
    when(mockConfig.getString(PlatformBackupManager.DB_HOST_CONFIG_KEY)).thenReturn(dbHost);
    when(mockConfig.getInt(PlatformBackupManager.DB_PORT_CONFIG_KEY)).thenReturn(dbPort);
  }

  private List<String> getExpectedPlatformBackupCommandArgs(
    String prometheusHost,
    String dbUsername,
    String dbHost,
    int dbPort,
    String inputPath,
    boolean isCreate
  ) {
    List<String> expectedCommandArgs = new ArrayList<>();
    expectedCommandArgs.add("bin/yb_platform_backup.sh");
    if (isCreate) {
      expectedCommandArgs.add("create");
      expectedCommandArgs.add("--exclude_prometheus");
      expectedCommandArgs.add("--exclude_releases");
      expectedCommandArgs.add("--output");
      expectedCommandArgs.add(
        PlatformBackupManager.getOutputDir(mockConfig).toAbsolutePath().toString()
      );
    } else {
      expectedCommandArgs.add("restore");
      expectedCommandArgs.add("--input");
      expectedCommandArgs.add(inputPath);
    }

    expectedCommandArgs.add("--db_username");
    expectedCommandArgs.add(dbUsername);
    expectedCommandArgs.add("--db_host");
    expectedCommandArgs.add(dbHost);
    expectedCommandArgs.add("--db_port");
    expectedCommandArgs.add(Integer.toString(dbPort));
    expectedCommandArgs.add("--prometheus_host");
    expectedCommandArgs.add(prometheusHost);
    expectedCommandArgs.add("--verbose");

    return expectedCommandArgs;
  }

  @SuppressWarnings("unused")
  private Object[] parametersToTestCreatePlatformBackupParams() {
    return new Object[][] {
      { "1.2.3.4", "postgres", "password", "localhost", 5432, "/tmp/foo.bar", true },
      { "1.2.3.4", "yugabyte", "", "5.6.7.8", 5433, "/tmp/foo.bar", true },
      { "1.2.3.4", "postgres", "password", "localhost", 5432, "/tmp/foo.bar", false },
      { "1.2.3.4", "yugabyte", "", "5.6.7.8", 5433, "/tmp/foo.bar", false },
    };
  }

  @Parameters(method = "parametersToTestCreatePlatformBackupParams")
  @Test
  public void testCreatePlatformBackupParams(
    String prometheusHost,
    String dbUsername,
    String dbPassword,
    String dbHost,
    int dbPort,
    String inputPath,
    boolean isCreate
  ) {
    setupConfig(prometheusHost, dbUsername, dbPassword, dbHost, dbPort);
    List<String> expectedCommandArgs = getExpectedPlatformBackupCommandArgs(
      prometheusHost,
      dbUsername,
      dbHost,
      dbPort,
      inputPath,
      isCreate
    );
    Map<String, String> expectedEnvVars = new HashMap<>();
    if (!dbPassword.isEmpty()) {
      expectedEnvVars.put(PlatformBackupManager.DB_PASSWORD_ENV_VAR_KEY, dbPassword);
    }

    when(shellProcessHandler.run(anyList(), anyMap())).thenReturn(new ShellResponse());
    PlatformBackupManager backupManager = spy(new PlatformBackupManager(
      mockConfig,
      actorSystem,
      executionContext,
      shellProcessHandler
    ));

    if (isCreate) {
      backupManager.createBackup();
    } else {
      backupManager.restoreBackup(inputPath);
    }

    verify(shellProcessHandler, times(1))
      .run(expectedCommandArgs, expectedEnvVars);
  }
}
