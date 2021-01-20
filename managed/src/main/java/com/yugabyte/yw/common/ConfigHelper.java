// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.annotations.VisibleForTesting;
import com.google.common.collect.ImmutableMap;
import com.google.common.collect.Maps;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.models.YugawareProperty;
import play.Application;
import play.libs.Json;

import javax.inject.Singleton;
import java.util.Collections;
import java.util.EnumMap;
import java.util.Map;

import org.yaml.snakeyaml.Yaml;
import org.yaml.snakeyaml.constructor.CustomClassLoaderConstructor;

import static com.yugabyte.yw.commissioner.Common.CloudType.aws;

@Singleton
public class ConfigHelper {

  public enum ConfigType {
    // TODO: investigate why many of these are null description. Is it intended or a bug?
    // Hopefully this less error prone way will prevent any bugs in future.
    AWSRegionMetadata("AWS Region Metadata", "configs/aws-region-metadata.yml"),
    AWSInstanceTypeMetadata("AWS Instance Type Metadata", "configs/aws-instance-type-metadata.yml"),
    GCPRegionMetadata("GCP Region Metadata", "configs/gcp-region-metadata.yml"),
    GCPInstanceTypeMetadata(null, "configs/gcp-instance-type-metadata.yml"),
    AZURegionMetadata("Azure Region Metadata", "configs/azu-region-metadata.yml"),
    AZUInstanceTypeMetadata(null),
    DockerRegionMetadata("Docker Region Metadata", "configs/docker-region-metadata.yml"),
    DockerInstanceTypeMetadata(null, "configs/docker-instance-type-metadata.yml"),
    SoftwareReleases("Software Releases"),
    SoftwareVersion("Software Version"),
    YugawareMetadata("Yugaware Metadata"),
    Security("Security Level");

    private final String description;
    private final String configFile;

    ConfigType(String description, String configFile) {
      this.description = description;
      this.configFile = configFile;
    }

    ConfigType(String description) {
      this(description, null);
    }

    @VisibleForTesting
    String getConfigFile() {
      return configFile;
    }

    public String getDescription() {
      return this.description;
    }
  }

  public Map<String, Object> getConfig(ConfigType type) {
    YugawareProperty p = YugawareProperty.get(type.toString());
    if (p == null)
      return Collections.emptyMap();
    JsonNode node = p.getValue();
    if (node == null)
      return Collections.emptyMap();
    return Json.fromJson(node, Map.class);
  }

  public Map<String, Object> getRegionMetadata(Common.CloudType type) {
    return type.getRegionMetadataConfigType()
      .map(this::getConfig)
      .orElse(Collections.emptyMap());
  }

  public void loadConfigsToDB(Application app) {
    for (ConfigType type : ConfigType.values()) {
      if (type.getConfigFile() == null) {
        continue;
      }
      Yaml yaml = new Yaml(new CustomClassLoaderConstructor(app.classloader()));
      Map<String, Object> config = yaml.load(
        app.resourceAsStream(type.getConfigFile())
      );
      loadConfigToDB(type, config);
    }
  }

  public void loadConfigToDB(ConfigType type, Map<String, Object> config) {
    YugawareProperty.addConfigProperty(type.toString(), Json.toJson(config), type.getDescription());
  }
}
