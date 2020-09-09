/*
 * Copyright 2020 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common.kms.algorithms;

import java.util.Arrays;
import java.util.List;

public enum AwsAlgorithm implements SupportedAlgorithmInterface {
  AES(Arrays.asList(128, 256));

  private final List<Integer> keySizes;

  public List<Integer> getKeySizes() {
    return this.keySizes;
  }

  AwsAlgorithm(List<Integer> keySizes) {
    this.keySizes = keySizes;
  }
}
