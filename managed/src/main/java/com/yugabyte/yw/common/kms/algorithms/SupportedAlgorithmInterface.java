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

import java.util.List;

/**
 * Should be implemented for each EncryptionAtRestService impl as an enum of supported
 * encryption algorithms
 */
public interface SupportedAlgorithmInterface {
  List<Integer> getKeySizes();
  String name();
}
