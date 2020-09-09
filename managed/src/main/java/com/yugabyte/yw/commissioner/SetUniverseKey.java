/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner;

import akka.actor.ActorSystem;
import com.google.common.annotations.VisibleForTesting;
import com.google.common.collect.ImmutableMap;
import com.google.common.net.HostAndPort;
import com.google.inject.Inject;
import com.google.inject.Singleton;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUtil;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.KmsHistory;
import com.yugabyte.yw.models.Universe;
import java.util.Arrays;
import java.util.Base64;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.YBClient;
import scala.concurrent.ExecutionContext;
import scala.concurrent.duration.Duration;

@Singleton
public class SetUniverseKey {
    public static final Logger LOG = LoggerFactory.getLogger(SetUniverseKey.class);

    private AtomicBoolean running = new AtomicBoolean(false);

    private final ActorSystem actorSystem;

    private final ExecutionContext executionContext;

    private final EncryptionAtRestManager keyManager;

    private final YBClientService ybService;

    private final int YB_SET_UNIVERSE_KEY_INTERVAL = 2;

    @Inject
    public SetUniverseKey(
            EncryptionAtRestManager keyManager,
            ExecutionContext executionContext,
            ActorSystem actorSystem,
            YBClientService ybService
    ) {
        this.keyManager = keyManager;
        this.actorSystem = actorSystem;
        this.executionContext = executionContext;
        this.ybService = ybService;
        this.initialize();
    }

    public void setRunningState(AtomicBoolean state) {
      this.running = state;
    }

    private void initialize() {
        this.actorSystem.scheduler().schedule(
                Duration.create(0, TimeUnit.MINUTES),
                Duration.create(YB_SET_UNIVERSE_KEY_INTERVAL, TimeUnit.MINUTES),
                () -> scheduleRunner(),
                this.executionContext
        );
    }

    private void setKeyInMaster(Universe u, HostAndPort masterAddr, byte[] keyRef, byte[] keyVal) {
        YBClient client = null;
        String hostPorts = u.getMasterAddresses();
        String certificate = u.getCertificate();
        try {
            client = ybService.getClient(hostPorts, certificate);
            String encodedKeyRef = Base64.getEncoder().encodeToString(keyRef);
            if (!client.hasUniverseKeyInMemory(encodedKeyRef, masterAddr)) {
                client.addUniverseKeys(ImmutableMap.of(encodedKeyRef, keyVal), masterAddr);
            }
        } catch (Exception e) {
            String errMsg = String.format(
                    "Error sending universe encryption key to node %s",
                    masterAddr.toString()
            );
            LOG.error(errMsg, e);
        } finally {
            ybService.closeClient(client, hostPorts);
        }
    }

    public void setUniverseKey(Universe u) {
        try {
            if (!u.universeIsLocked() &&
                    EncryptionAtRestUtil.getNumKeyRotations(u.universeUUID) > 0) {
                LOG.debug(String.format(
                        "Setting universe encryption key for universe %s",
                        u.universeUUID.toString()
                ));

                KmsHistory activeKey = EncryptionAtRestUtil.getActiveKey(u.universeUUID);
                if (activeKey == null ||
                        activeKey.uuid.keyRef == null || activeKey.uuid.keyRef.length() == 0) {
                    final String errMsg = String.format(
                            "No active key found for universe %s",
                            u.universeUUID.toString()
                    );
                    LOG.debug(errMsg);
                    return;
                }

                byte[] keyRef = Base64.getDecoder().decode(activeKey.uuid.keyRef);
                byte[] keyVal = keyManager.getUniverseKey(
                        u.universeUUID,
                        activeKey.configUuid,
                        keyRef
                );
                Arrays.stream(u.getMasterAddresses().split(","))
                        .map(addrString -> HostAndPort.fromString(addrString))
                        .forEach(addr -> setKeyInMaster(u, addr, keyRef, keyVal));
            }
        } catch (Exception e) {
            String errMsg = String.format(
                    "Error setting universe encryption key for universe %s",
                    u.universeUUID.toString()
            );
            LOG.error(errMsg, e);
        }
    }

    public void handleCustomerError(UUID cUUID, Exception e) {
      LOG.error(
        String.format("Error detected running universe key setter for customer %s", cUUID),
        e
      );
    }

    public void setCustomerUniverseKeys(Customer c) {
      c.getUniverses().forEach(this::setUniverseKey);
    }

    @VisibleForTesting
    void scheduleRunner() {
        if (!running.get()) {
            LOG.info("Running universe key setter");
            running.set(true);
            Customer.getAll().forEach(c -> {
              try {
                setCustomerUniverseKeys(c);
              } catch (Exception e) {
                handleCustomerError(c.uuid, e);
              }
            });
            running.set(false);
        }
    }
}
