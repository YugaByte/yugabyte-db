/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.models.KmsConfig;

public class CreateKMSConfig extends KMSConfigTaskBase {
  @Override
  public void run() {
    LOG.info("Creating KMS Configuration for customer: " +
      taskParams().customerUUID.toString());
    KmsConfig createResult = kmsManager
      .getServiceInstance(taskParams().kmsProvider.name())
      .createAuthConfig(
        taskParams().customerUUID,
        taskParams().kmsConfigName,
        taskParams().providerConfig
      );

    if (createResult == null) {
      throw new RuntimeException(String.format(
        "Error creating KMS Configuration for customer %s and kms provider %s",
        taskParams().customerUUID.toString(),
        taskParams().kmsProvider
      ));
    }
  }
}
