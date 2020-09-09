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

public class DeleteKMSConfig extends KMSConfigTaskBase {
  @Override
  public void run() {
    LOG.info("Deleting KMS Configuration for customer: " +
      taskParams().customerUUID.toString());
    kmsManager
      .getServiceInstance(taskParams().kmsProvider.name())
      .deleteKMSConfig(taskParams().configUUID);
  }
}
