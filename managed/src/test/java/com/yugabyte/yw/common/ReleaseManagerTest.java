// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.common;


import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import org.apache.commons.io.FileUtils;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.Mockito;
import org.mockito.runners.MockitoJUnitRunner;
import play.Configuration;
import play.libs.Json;

import java.io.File;
import java.io.IOException;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.ConfigHelper.ConfigType.SoftwareReleases;
import static com.yugabyte.yw.common.TestHelper.createTempFile;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.junit.Assert.*;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.when;
import static org.hamcrest.CoreMatchers.*;

@RunWith(MockitoJUnitRunner.class)
public class ReleaseManagerTest {
  static String TMP_STORAGE_PATH = "/tmp/yugaware_tests/releases";
  static String TMP_DOCKER_STORAGE_PATH = "/tmp/yugaware_tests/docker/releases";

  @InjectMocks
  ReleaseManager releaseManager;

  @Mock
  Configuration appConfig;

  @Mock
  ConfigHelper configHelper;

  @Before
  public void beforeTest() throws IOException {
    new File(TMP_STORAGE_PATH).mkdirs();
    new File(TMP_DOCKER_STORAGE_PATH).mkdirs();
  }

  @After
  public void tearDown() throws IOException {
    FileUtils.deleteDirectory(new File(TMP_STORAGE_PATH));
    FileUtils.deleteDirectory(new File(TMP_DOCKER_STORAGE_PATH));
  }

  private void createDummyReleases(List<String> versions, boolean multipleRepos, boolean inDockerPath) {
    createDummyReleases(versions, multipleRepos, inDockerPath, true);
  }

  private void createDummyReleases(
      List<String> versions, boolean multipleRepos, boolean inDockerPath,
      boolean hasEnterpriseStr) {
    versions.forEach((version) -> {
      String versionPath = String.format("%s/%s", TMP_STORAGE_PATH, version);
      new File(versionPath).mkdirs();
      if (inDockerPath) {
        versionPath = TMP_DOCKER_STORAGE_PATH;
      }
      String eeStr = hasEnterpriseStr ? "ee-" : "";
      createTempFile(
        versionPath, "yugabyte-" + eeStr + version + "-centos-x86_64.tar.gz", "Sample data");
      if (multipleRepos) {
        createTempFile(
          versionPath, "devops.xyz." + version + "-centos-x86_64.tar.gz", "Sample data");
      }
    });
  }

  @Test
  public void testGetLocalReleasesWithValidPath() {
    List<String> versions = ImmutableList.of("0.0.1", "0.0.2", "0.0.3");
    createDummyReleases(versions, false, false);
    Map<String, String> releases = releaseManager.getLocalReleases(TMP_STORAGE_PATH);
    assertEquals(3, releases.size());

    releases.keySet().forEach((version) -> {
      assertTrue(versions.contains(version));
    });
  }

  @Test
  public void testGetLocalReleasesWithMultipleFilesValidPath() {
    List<String> versions = ImmutableList.of("0.0.1", "0.0.2", "0.0.3");
    createDummyReleases(versions, true, false);
    Map<String, String> releases = releaseManager.getLocalReleases(TMP_STORAGE_PATH);
    assertEquals(3, releases.size());

    releases.keySet().forEach((version) -> {
      assertTrue(versions.contains(version));
    });
  }

  @Test
  public void testGetLocalReleasesWithInvalidPath() {
    Map<String, String> releases = releaseManager.getLocalReleases("/foo/bar");
    assertTrue(releases.isEmpty());
  }

  @Test
  public void testLoadReleasesWithoutReleasePath() {
    releaseManager.importLocalReleases();
    Mockito.verify(configHelper, times(0)).loadConfigToDB(any(), anyMap());
  }

  private void assertReleases(Map expectedMap, HashMap releases) {
    assertEquals(expectedMap.size(), releases.size());
    for (Object version : releases.keySet()) {
      assertTrue(expectedMap.containsKey(version));
      Object expectedFile = expectedMap.get(version);
      JsonNode releaseJson = Json.toJson(releases.get(version));
      assertValue(releaseJson, "filePath", expectedFile.toString());
      assertValue(releaseJson, "imageTag", version.toString());
      assertValue(releaseJson, "state", "ACTIVE");
    }
  }

  @Test
  public void testLoadReleasesWithReleasePath() {
    when(appConfig.getString("yb.releases.path")).thenReturn(TMP_STORAGE_PATH);
    List<String> versions = ImmutableList.of("0.0.1");
    createDummyReleases(versions, false, false);
    releaseManager.importLocalReleases();

    ArgumentCaptor<ConfigHelper.ConfigType> configType;
    ArgumentCaptor<HashMap> releaseMap;
    configType = ArgumentCaptor.forClass(ConfigHelper.ConfigType.class);
    releaseMap = ArgumentCaptor.forClass(HashMap.class);
    Mockito.verify(configHelper, times(1)).loadConfigToDB(configType.capture(), releaseMap.capture());
    Map expectedMap = ImmutableMap.of(
        "0.0.1", TMP_STORAGE_PATH + "/0.0.1/yugabyte-ee-0.0.1-centos-x86_64.tar.gz");
    assertReleases(expectedMap, releaseMap.getValue());
  }

  @Test
  public void testLoadReleasesWithReleaseAndDockerPath() {
    when(appConfig.getString("yb.releases.path")).thenReturn(TMP_STORAGE_PATH);
    when(appConfig.getString("yb.docker.release")).thenReturn(TMP_DOCKER_STORAGE_PATH);
    List<String> versions = ImmutableList.of("0.0.1");
    createDummyReleases(versions, false, false);
    List<String> dockerVersions = ImmutableList.of("0.0.2-b2");
    createDummyReleases(dockerVersions, false, true);
    List<String> dockerVersionsWithoutEe = ImmutableList.of("0.0.3-b3");
    createDummyReleases(dockerVersionsWithoutEe, false, true, false);
    releaseManager.importLocalReleases();
    ArgumentCaptor<ConfigHelper.ConfigType> configType;
    ArgumentCaptor<HashMap> releaseMap;
    configType = ArgumentCaptor.forClass(ConfigHelper.ConfigType.class);
    releaseMap = ArgumentCaptor.forClass(HashMap.class);
    Mockito.verify(configHelper, times(1)).loadConfigToDB(configType.capture(), releaseMap.capture());
    Map expectedMap = ImmutableMap.of(
        "0.0.1", TMP_STORAGE_PATH + "/0.0.1/yugabyte-ee-0.0.1-centos-x86_64.tar.gz",
        "0.0.2-b2", TMP_STORAGE_PATH + "/0.0.2-b2/yugabyte-ee-0.0.2-b2-centos-x86_64.tar.gz",
        "0.0.3-b3", TMP_STORAGE_PATH + "/0.0.3-b3/yugabyte-0.0.3-b3-centos-x86_64.tar.gz"
    );

    assertEquals(SoftwareReleases, configType.getValue());
    assertReleases(expectedMap, releaseMap.getValue());
    File dockerStoragePath = new File(TMP_DOCKER_STORAGE_PATH);
    File[] files = dockerStoragePath.listFiles();
    assertEquals(0, files.length);
  }

  @Test
  public void testLoadReleasesWithReleaseAndDockerPathDuplicate() {
    when(appConfig.getString("yb.releases.path")).thenReturn(TMP_STORAGE_PATH);
    when(appConfig.getString("yb.docker.release")).thenReturn(TMP_DOCKER_STORAGE_PATH);
    List<String> versions = ImmutableList.of("0.0.2-b2");
    createDummyReleases(versions, false, false);
    List<String> dockerVersions = ImmutableList.of("0.0.2-b2");
    createDummyReleases(dockerVersions, false, true);
    releaseManager.importLocalReleases();
    ArgumentCaptor<ConfigHelper.ConfigType> configType;
    ArgumentCaptor<HashMap> releaseMap;
    configType = ArgumentCaptor.forClass(ConfigHelper.ConfigType.class);
    releaseMap = ArgumentCaptor.forClass(HashMap.class);
    Mockito.verify(configHelper, times(1)).loadConfigToDB(configType.capture(), releaseMap.capture());
    Map expectedMap = ImmutableMap.of(
        "0.0.2-b2", TMP_STORAGE_PATH + "/0.0.2-b2/yugabyte-ee-0.0.2-b2-centos-x86_64.tar.gz");
    assertReleases(expectedMap, releaseMap.getValue());
    assertEquals(SoftwareReleases, configType.getValue());

    File dockerStoragePath = new File(TMP_DOCKER_STORAGE_PATH);
    File[] files = dockerStoragePath.listFiles();
    assertEquals(0, files.length);
  }

  @Test
  public void testLoadReleasesWithInvalidDockerPath() {
    try {
      releaseManager.importLocalReleases();
    } catch (RuntimeException re) {
      assertEquals("Unable to look up release files in foo", re.getMessage());
    }
  }

  @Test
  public void testGetReleaseByVersionWithConfig() {
    ReleaseManager.ReleaseMetadata metadata =
        ReleaseManager.ReleaseMetadata.fromLegacy("0.0.1", "/path/to/yugabyte-0.0.1.tar.gz");
    when(configHelper.getConfig(SoftwareReleases))
        .thenReturn(ImmutableMap.of("0.0.1", metadata));
    ReleaseManager.ReleaseMetadata release = releaseManager.getReleaseByVersion("0.0.1");
    assertThat(release.filePath, allOf(notNullValue(),
        equalTo("/path/to/yugabyte-0.0.1.tar.gz")));
  }

  @Test
  public void testGetReleaseByVersionWithoutConfig() {
    when(configHelper.getConfig(SoftwareReleases)).thenReturn(Collections.emptyMap());
    ReleaseManager.ReleaseMetadata release = releaseManager.getReleaseByVersion("0.0.1");
    assertNull(release);
  }

  @Test
  public void testGetReleaseByVersionWithLegacyConfig() {
    HashMap releases = new HashMap();
    releases.put("0.0.1", "/path/to/yugabyte-0.0.1.tar.gz");
    when(configHelper.getConfig(SoftwareReleases)).thenReturn(releases);
    ReleaseManager.ReleaseMetadata release = releaseManager.getReleaseByVersion("0.0.1");
    assertThat(release.filePath, allOf(notNullValue(),
        equalTo("/path/to/yugabyte-0.0.1.tar.gz")));
  }


  @Test
  public void testAddRelease() {
    releaseManager.addRelease("0.0.1");
    ArgumentCaptor<ConfigHelper.ConfigType> configType;
    ArgumentCaptor<HashMap> releaseMap;
    configType = ArgumentCaptor.forClass(ConfigHelper.ConfigType.class);
    releaseMap = ArgumentCaptor.forClass(HashMap.class);
    Mockito.verify(configHelper, times(1)).loadConfigToDB(configType.capture(), releaseMap.capture());
    Map releaseInfo = releaseMap.getValue();
    assertTrue(releaseInfo.containsKey("0.0.1"));
    JsonNode releaseMetadata = Json.toJson(releaseInfo.get("0.0.1"));
    assertValue(releaseMetadata, "imageTag", "0.0.1");
  }

  @Test
  public void testAddExistingRelease() {
    ReleaseManager.ReleaseMetadata metadata =
        ReleaseManager.ReleaseMetadata.fromLegacy("0.0.1", "/path/to/yugabyte-0.0.1.tar.gz");
    when(configHelper.getConfig(SoftwareReleases))
        .thenReturn(ImmutableMap.of("0.0.1", metadata));
    try {
      releaseManager.addRelease("0.0.1");
    } catch (RuntimeException re) {
      assertEquals("Release already exists: 0.0.1", re.getMessage());
    }
  }
}
