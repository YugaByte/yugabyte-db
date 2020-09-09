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

import com.google.common.net.HostAndPort;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUtil;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.models.KmsHistory;
import java.util.Map;
import java.util.UUID;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.YBClient;
import play.api.Play;

public class WaitForEncryptionKeyInMemory extends NodeTaskBase {
    public static final Logger LOG = LoggerFactory.getLogger(WaitForEncryptionKeyInMemory.class);

    public YBClientService ybService = null;

    public EncryptionAtRestManager keyManager = null;

    public static final int KEY_IN_MEMORY_TIMEOUT = 5000;

    public static class Params extends NodeTaskParams {
        public HostAndPort nodeAddress;
    }

    @Override
    public void initialize(ITaskParams params) {
        super.initialize(params);
        ybService = Play.current().injector().instanceOf(YBClientService.class);
        keyManager = Play.current().injector().instanceOf(EncryptionAtRestManager.class);
    }

    @Override
    protected Params taskParams() {
        return (Params)taskParams;
    }

    @Override
    public void run() {
        Universe universe = Universe.get(taskParams().universeUUID);
        if (universe != null &&
                EncryptionAtRestUtil.getNumKeyRotations(universe.universeUUID) > 0) {
            YBClient client = null;
            String hostPorts = universe.getMasterAddresses();
            String certificate = universe.getCertificate();
            try {
                client = ybService.getClient(hostPorts, certificate);
                KmsHistory activeKey = EncryptionAtRestUtil.getActiveKey(universe.universeUUID);
                if (!client.waitForMasterHasUniverseKeyInMemory(
                        KEY_IN_MEMORY_TIMEOUT,
                        activeKey.uuid.keyRef,
                        taskParams().nodeAddress
                )) {
                    throw new RuntimeException(
                            "Timeout occurred waiting for universe encryption key to be set in memory"
                    );
                }
            } catch (Exception e) {
                LOG.error("{} hit error : {}", getName(), e.getMessage());
            } finally {
                ybService.closeClient(client, hostPorts);
            }
        }
    }
}
