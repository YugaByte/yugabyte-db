/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks.cloud;

import com.yugabyte.yw.commissioner.tasks.CloudTaskBase;
import com.yugabyte.yw.commissioner.tasks.CloudBootstrap;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Region;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.api.Play;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public class CloudAccessKeySetup extends CloudTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(CloudAccessKeySetup.class);

  public static class Params extends CloudBootstrap.Params {
    public String regionCode;
  }

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public void run() {
    String regionCode = taskParams().regionCode;
    Region region = Region.getByCode(getProvider(), regionCode);
    if (region == null) {
      throw new RuntimeException("Region " +  regionCode + " not setup.");
    }
    AccessManager accessManager = Play.current().injector().instanceOf(AccessManager.class);
    boolean airGapInstall = taskParams().airGapInstall;
    Integer sshPort = taskParams().sshPort;
    // TODO(bogdan): validation at higher level?
    // If no custom keypair / ssh data specified, then create new.
    if (taskParams().keyPairName == null || taskParams().keyPairName.isEmpty() ||
        taskParams().sshPrivateKeyContent == null || taskParams().sshPrivateKeyContent.isEmpty() ||
        taskParams().sshUser == null || taskParams().sshUser.isEmpty()) {
      String sanitizedProviderName = getProvider().name.replaceAll("\\s+", "-").toLowerCase();
      String accessKeyCode = String.format(
          "yb-%s-%s-key", Customer.get(getProvider().customerUUID).code, sanitizedProviderName);
      accessManager.addKey(
          region.uuid, accessKeyCode, null, taskParams().sshUser, sshPort, airGapInstall);
    } else {
      // Create temp file and fill with content.
      AccessManager.KeyType keyType = AccessManager.KeyType.PRIVATE;
      try {
        Path tempFile = Files.createTempFile(taskParams().keyPairName, keyType.getExtension());
        Files.write(tempFile, taskParams().sshPrivateKeyContent.getBytes());
        accessManager.addKey(
            region.uuid, taskParams().keyPairName, tempFile.toFile(), taskParams().sshUser,
            taskParams().sshPort, airGapInstall);
      } catch (IOException ioe) {
        ioe.printStackTrace();
        throw new RuntimeException("Could not create AccessKey", ioe);
      }
    }
  }
}
