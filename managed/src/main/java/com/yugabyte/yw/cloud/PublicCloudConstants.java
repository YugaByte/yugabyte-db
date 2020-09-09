/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.cloud;
import com.yugabyte.yw.commissioner.Common;

public class PublicCloudConstants {
  public static final String IO1_SIZE = "io1.size";
  public static final String IO1_PIOPS = "io1.piops";
  public static final String GP2_SIZE = "gp2.size";
  public enum Tenancy {
    Shared,
    Dedicated,
    Host
  }

  /**
   * Tracks the supported storage options for each cloud provider.
   * Options in the UI will be ordered alphabetically
   * e.g. Persistent will be the default value for GCP, not Scratch
   */
  public enum StorageType {
    IO1(Common.CloudType.aws),
    GP2(Common.CloudType.aws),
    Scratch(Common.CloudType.gcp),
    Persistent(Common.CloudType.gcp),
    StandardSSD_LRS(Common.CloudType.azu),
    Premium_LRS(Common.CloudType.azu),
    UltraSSD_LRS(Common.CloudType.azu);

    private Common.CloudType cloudType;

    StorageType(Common.CloudType cloudType) {
      this.cloudType = cloudType;
    }

    public Common.CloudType getCloudType() {
      return cloudType;
    }
  }

}
