// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.models.YugawareProperty;
import play.Application;
import play.libs.Json;

import javax.inject.Singleton;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import org.yaml.snakeyaml.Yaml;
import org.yaml.snakeyaml.constructor.CustomClassLoaderConstructor;

@Singleton
public class ConfigHelper {

  public enum ConfigType {
    AWSRegionMetadata,
    AWSInstanceTypeMetadata,
    GCPRegionMetadata,
    GCPInstanceTypeMetadata,
    AZURegionMetadata,
    AZUInstanceTypeMetadata,
    DockerRegionMetadata,
    DockerInstanceTypeMetadata,
    SoftwareReleases,
    SoftwareVersion,
    YugawareMetadata,
    Security;

    public String getConfigFile() {
      switch (this) {
        case AWSRegionMetadata:
          return "configs/aws-region-metadata.yml";
        case AWSInstanceTypeMetadata:
          // TODO: pull this directly from AWS api via ybcloud.
          return "configs/aws-instance-type-metadata.yml";
        case GCPRegionMetadata:
          return "configs/gcp-region-metadata.yml";
        case GCPInstanceTypeMetadata:
          return "configs/gcp-instance-type-metadata.yml";
        case AZURegionMetadata:
          return "configs/azu-region-metadata.yml";
        case DockerRegionMetadata:
          return "configs/docker-region-metadata.yml";
        case DockerInstanceTypeMetadata:
          return "configs/docker-instance-type-metadata.yml";
        default:
          return null;
      }
    }

    public String getDescription() {
      switch (this) {
        case AWSInstanceTypeMetadata:
          return "AWS Instance Type Metadata";
        case AWSRegionMetadata:
          return "AWS Region Metadata";
        case GCPRegionMetadata:
          return "GCP Region Metadata";
        case AZURegionMetadata:
          return "Azure Region Metadata";
        case DockerRegionMetadata:
          return "Docker Region Metadata";
        case SoftwareReleases:
          return "Software Releases";
        case SoftwareVersion:
          return "Software Version";
        case YugawareMetadata:
          return "Yugaware Metadata";
        case Security:
          return "Security Level";
        default:
          return null;
      }
    }
  }

  public Map<String, Object> getConfig(ConfigType type) {
    YugawareProperty p = YugawareProperty.get(type.toString());
    if (p == null) return Collections.emptyMap();
    JsonNode node = p.getValue();
    if (node == null) return Collections.emptyMap();
    return Json.fromJson(node, Map.class);
  }

  public Map<String, Object> getRegionMetadata(Common.CloudType type) {
    switch (type) {
      case aws:
        return getConfig(ConfigType.AWSRegionMetadata);
      case gcp:
        return getConfig(ConfigType.GCPRegionMetadata);
      case docker:
        return getConfig(ConfigType.DockerRegionMetadata);
      case azu:
        return getConfig(ConfigType.AZURegionMetadata);
      default:
        return Collections.emptyMap();
    }
  }

  public void loadConfigsToDB(Application app) {
    for (ConfigType type: ConfigType.values()) {
      if (type.getConfigFile() == null) {
        continue;
      }
      Yaml yaml = new Yaml(new CustomClassLoaderConstructor(app.classloader()));
      Map<String, Object> config = (HashMap<String, Object>) yaml.load(
        app.resourceAsStream(type.getConfigFile())
      );
      loadConfigToDB(type, config);
    }
  }

  public void loadConfigToDB(ConfigType type, Map<String, Object> config) {
    YugawareProperty.addConfigProperty(type.toString(), Json.toJson(config), type.getDescription());
  }
}
