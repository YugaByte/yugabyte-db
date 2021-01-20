// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import com.yugabyte.yw.common.ConfigHelper;

import java.util.Optional;

public class Common {
  // The various cloud types supported.
  public enum CloudType {
    unknown("unknown"),
    aws("aws", ConfigHelper.ConfigType.AWSRegionMetadata),
    gcp("gcp", ConfigHelper.ConfigType.GCPRegionMetadata),
    azu("azu", ConfigHelper.ConfigType.AZURegionMetadata),
    docker("docker", ConfigHelper.ConfigType.DockerRegionMetadata),
    onprem("onprem"),
    kubernetes("kubernetes"),
    local("cloud-1"),
    other("other");

    private final String value;
    private final Optional<ConfigHelper.ConfigType> regionMetadataConfigType;

    CloudType(String value, ConfigHelper.ConfigType regionMetadataConfigType) {
      this.value = value;
      this.regionMetadataConfigType = Optional.ofNullable(regionMetadataConfigType);
    }

    CloudType(String value) {
      this(value, null);
    }

    public Optional<ConfigHelper.ConfigType> getRegionMetadataConfigType() {
      return this.regionMetadataConfigType;
    }

    public String toString() {
      return this.value;
    }
  }
}
