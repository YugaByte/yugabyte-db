/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.models;

import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.models.KmsHistoryId.TargetType;
import org.junit.Before;
import org.junit.Test;
import play.libs.Json;

import java.util.List;
import java.util.UUID;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

public class KmsHistoryTest extends FakeDBApplication {

  private KmsConfig testKMSConfig;

  @Before
  public void setup() {
    Customer testCustomer = ModelFactory.testCustomer();
    testKMSConfig = KmsConfig.createKMSConfig(
      testCustomer.uuid,
      KeyProvider.AWS,
      Json.newObject().put("test_key", "test_val"),
      "some config name"
    );
  }

    @Test
    public void testCreateUniverseHistory() {
        UUID configUUID = testKMSConfig.configUUID;
        UUID universeUUID = UUID.randomUUID();
        KmsHistory keyRef = KmsHistory
                .createKmsHistory(configUUID, universeUUID, TargetType.UNIVERSE_KEY, "a");
        assertNotNull(keyRef);
    }

    @Test
    public void testGetCurrentKeyRef() {
        UUID configUUID = testKMSConfig.configUUID;
        UUID universeUUID = UUID.randomUUID();
        KmsHistory.createKmsHistory(configUUID, universeUUID, TargetType.UNIVERSE_KEY, "a");
        KmsHistory.createKmsHistory(configUUID, universeUUID, TargetType.UNIVERSE_KEY, "b");

        // Nothing should be returned because the key ref has not been set to active yet
        KmsHistory keyRef = KmsHistory.getActiveHistory(universeUUID, TargetType.UNIVERSE_KEY);
        assertEquals(null, keyRef);

        // Activate key ref
        KmsHistory.activateKeyRef(universeUUID, configUUID, TargetType.UNIVERSE_KEY, "a");

        // Ensure that now a result (the active history row) is returned
        keyRef = KmsHistory.getActiveHistory(universeUUID, TargetType.UNIVERSE_KEY);
        assertEquals("a", keyRef.uuid.keyRef);

        // Activate key ref
        KmsHistory.activateKeyRef(universeUUID, configUUID, TargetType.UNIVERSE_KEY, "b");

        // Ensure that now the appropriate result (the active history row) is returned
        keyRef = KmsHistory.getActiveHistory(universeUUID, TargetType.UNIVERSE_KEY);
        assertEquals("b", keyRef.uuid.keyRef);
    }

    @Test
    public void testGetAllTargetKeyRefs() {
        UUID configUUID = testKMSConfig.configUUID;
        UUID universeUUID = UUID.randomUUID();
        KmsHistory.createKmsHistory(configUUID, universeUUID, TargetType.UNIVERSE_KEY, "a");
        List<KmsHistory> targetHistory = KmsHistory
                .getAllConfigTargetKeyRefs(configUUID, universeUUID, TargetType.UNIVERSE_KEY);
        assertEquals(targetHistory.size(), 1);
    }

    @Test
    public void testDeleteAllTargetKeyRefs() {
        UUID configUUID = testKMSConfig.configUUID;
        UUID universeUUID = UUID.randomUUID();
        KmsHistory.createKmsHistory(configUUID, universeUUID, TargetType.UNIVERSE_KEY, "a");
        KmsHistory.deleteAllConfigTargetKeyRefs(configUUID, universeUUID, TargetType.UNIVERSE_KEY);
        List<KmsHistory> targetHistory = KmsHistory
                .getAllConfigTargetKeyRefs(configUUID, universeUUID, TargetType.UNIVERSE_KEY);
        assertEquals(targetHistory.size(), 0);
    }
}
