/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */
package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.models.Universe;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.YBClient;
import play.api.Play;

import com.yugabyte.yw.commissioner.AbstractTaskBase;

public class BackupUniverseKeys extends AbstractTaskBase {

    public static final Logger LOG = LoggerFactory.getLogger(BackupUniverseKeys.class);

    // The encryption key manager
    public EncryptionAtRestManager keyManager = null;

    @Override
    protected BackupTableParams taskParams() {
        return (BackupTableParams) taskParams;
    }

    @Override
    public void initialize(ITaskParams params) {
        super.initialize(params);
        keyManager = Play.current().injector().instanceOf(EncryptionAtRestManager.class);
    }

    @Override
    public void run() {
        Universe universe = Universe.get(taskParams().universeUUID);
        String hostPorts = universe.getMasterAddresses();
        try {
            LOG.info("Running {}: hostPorts={}.", getName(), hostPorts);
            keyManager.backupUniverseKeyHistory(
                taskParams().universeUUID,
                taskParams().storageLocation
            );
        } catch (Exception e) {
            LOG.error("{} hit error : {}", getName(), e.getMessage(), e);
            throw new RuntimeException(e);
        }
    }
}
