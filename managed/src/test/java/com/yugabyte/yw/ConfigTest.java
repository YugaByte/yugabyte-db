/*
 * Copyright 2021 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * http://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw;

import com.google.common.base.Strings;
import com.google.common.collect.ImmutableMap;
import com.google.common.collect.ImmutableSet;
import com.typesafe.config.Config;
import com.typesafe.config.ConfigFactory;
import com.typesafe.config.ConfigRenderOptions;
import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TestWatcher;
import org.junit.runner.Description;
import org.junit.runner.RunWith;
import play.Environment;
import play.Mode;
import play.api.Configuration;
import play.libs.Scala;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.stream.Collectors;

import static java.util.stream.Collectors.joining;
import static junit.framework.TestCase.assertEquals;
import static junit.framework.TestCase.assertSame;

@RunWith(JUnitParamsRunner.class)
public class ConfigTest {

  // Whole config under these roots are generated in output file
  private static final Set<String> ALWAYS_GENERATE_PATH_PREFIXES =
    ImmutableSet.of("yb.", "db.", "play.filters.cors.", "play.filters.csrf.");

  private static final ConfigRenderOptions CONFIG_RENDER_OPTIONS =
    ConfigRenderOptions.concise().setJson(false).setFormatted(true);

  private String expectedConfigFilename = null;
  private final Set<String> candidateGeneratedFromActualConfig = new HashSet<>();

  @Rule
  public final TestWatcher watcher = new TestWatcher() {
    @Override
    protected void succeeded(Description description) {
      if (Boolean.parseBoolean(System.getProperty("always_generate_candidate_conf"))) {
        generateCandidateConfig();
      }
    }

    @Override
    protected void failed(Throwable e, Description description) {
      generateCandidateConfig();
    }

  };

  private void generateCandidateConfig() {
    try {
      File candidateFile = File.createTempFile(expectedConfigFilename + "-", ".candidate");
      try (FileWriter fileWriter = new FileWriter(candidateFile)) {
        String blob = candidateGeneratedFromActualConfig.stream().sorted()
          .collect(joining(System.lineSeparator(), "", System.lineSeparator()));
        fileWriter.write(blob);
      }
      System.err.println(Strings.repeat("-", 100));
      System.err.println("  Actual config written to: " + candidateFile.getAbsolutePath());
      System.err.println(Strings.repeat("-", 100));
    } catch (IOException ioException) {
      ioException.printStackTrace();
    }
  }

  // This test compares entire resolved config one entry at a time.
  // This will generate a filtered view of the actual config key=value
  // An entry is added to generated file if any of these conditions are met:
  //    - There is existing entry for that path in expected config
  //    - The path is one of the special (ALWAYS_GENERATE_PATH_PREFIXES
  //       like "yb. or "db." )
  //    - The path exist in both configs but the values do not match
  @Test
  @Parameters({
    "test.replicated.params.conf, replicated.expected.conf, envRepl",
    "test.helm.params.conf, helm.expected.conf, envHelm",
    "test.yugabundle.conf, yugabundle.expected.conf, envYugabundle",
    "application.yugabyted.conf, yugabyted.expected.conf, envYugabyted",
    "application.test.conf, test.expected.conf, envTest",
    "application.conf, dev.expected.conf, envDev",
  })
  public void singleKey(String testConfigFile, String expectedConfigFile, String envName) {
    Config actualConfig = loadTestDeploymentConfig(testConfigFile, envName);
    Config expectedConfig = loadTestDeploymentConfig(expectedConfigFile, envName);
    Set<String> actualConfigPaths = getAllPaths(actualConfig);

    buildCandidateConfForDeployment(expectedConfigFile, actualConfig, expectedConfig,
      actualConfigPaths);
    Set<String> allPaths = getAllPaths(expectedConfig);
    allPaths.addAll(actualConfigPaths);
    for (String path : allPaths) {
      assertSame(path, expectedConfig.hasPath(path), actualConfig.hasPath(path));
      assertEquals(
        "\nExpected: " + getKVStr(expectedConfig, path) +
          "\nActual: " + getKVStr(actualConfig, path),
        expectedConfig.getValue(path),
        actualConfig.getValue(path));
    }
  }

  private void buildCandidateConfForDeployment(String expectedConfigFile,
                                               Config actualConfig,
                                               Config expectedConfig,
                                               Set<String> actualConfigPaths) {
    try {
      this.expectedConfigFilename = expectedConfigFile;

      Set<String> currentPathsFromExpectedConfigFile =
        getAllPaths(ConfigFactory.parseResources(expectedConfigFile));

      for (String path : actualConfigPaths) {
        if (currentPathsFromExpectedConfigFile.contains(path) ||
          isBatchTestPath(path, ALWAYS_GENERATE_PATH_PREFIXES) ||
          !expectedConfig.hasPathOrNull(path) ||
            !expectedConfig.getValue(path).equals(actualConfig.getValue(path))) {
            candidateGeneratedFromActualConfig.add(getKVStr(actualConfig, path));
          }
      }
    } catch (Exception exception) {
      exception.printStackTrace();
    }
  }

  private static boolean isBatchTestPath(String path, Set<String> batchTestPaths) {
    return batchTestPaths.stream().anyMatch(path::startsWith);
  }

  private static Set<String> getAllPaths(Config config) {
    return config
      .entrySet()
      .stream()
      .map(Map.Entry::getKey)
      .collect(Collectors.toSet());
  }

  /**
   * From a given config lookup key value pair and return string "key=value"
   */
  private static String getKVStr(Config config, String key) {
    return key + " = " +
      config.getValue(key)
        .render(CONFIG_RENDER_OPTIONS);
  }

  private static Config loadTestDeploymentConfig(String testConfigFile, String envName) {
    ClassLoader classLoader = Thread.currentThread().getContextClassLoader();
    Environment env = new Environment(new File("."), classLoader, Mode.TEST);
    // play uses this property to locate resource instead of taking an argument to load method.
    System.setProperty("config.resource", testConfigFile);
    Configuration load = Configuration.load(env.asScala(),
      Scala.asScala(getEnvForName(envName)));
    return load.underlying();
  }

  private static Map<String, Object> getEnvForName(String envName) {
    switch (envName) {
      case "envHelm":
        return ImmutableMap.of(
          "APP_SECRET", "TOP_SECRET_HELM",
          "POSTGRES_DB", "yugaware",
          "POSTGRES_USER", "helm.pg.user",
          "POSTGRES_PASSWORD", "helm#pg$passwd");
      case "envYugabyted":
        return ImmutableMap.of("USE_NATIVE_METRICS", true);
      case "envYugabundle":
        return ImmutableMap.of(
          "PLATFORM_DB_USER", "ybundle_user",
          "PLATFORM_DB_PASSWORD", "ybundle_pass",
          "PLATFORM_APP_SECRET", "PLATFORM_APP_SECRET",
          "CORS_ORIGIN", "http://cors.origin.com");
      default:
        return ImmutableMap.of();
    }
  }

}
