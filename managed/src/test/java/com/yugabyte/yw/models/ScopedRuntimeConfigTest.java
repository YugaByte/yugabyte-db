package com.yugabyte.yw.models;

import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import org.junit.Before;
import org.junit.Test;

import java.util.UUID;

import static com.yugabyte.yw.models.ScopedRuntimeConfig.GLOBAL_SCOPE_UUID;
import static org.junit.Assert.*;

public class ScopedRuntimeConfigTest extends FakeDBApplication {
  private Customer defaultCustomer;
  private Universe defaultUniverse;
  private Provider defaultProvider;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultUniverse = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
  }

  @Test
  public void testEnsureGetCascadeDelete_globalConfig() {
    ScopedRuntimeConfig.ensureGlobal();
    UUID uuid = GLOBAL_SCOPE_UUID;
    ScopedRuntimeConfig config = ScopedRuntimeConfig.get(uuid);
    assertEquals(uuid, config.uuid);
    assertNull(config.customerUUID);
    assertNull(config.universeUUID);
    assertNull(config.providerUUID);
  }

  @Test
  public void testEnsureGetCascadeDelete_customerConfig() {
    ScopedRuntimeConfig.ensure(defaultCustomer);
    UUID uuid = defaultCustomer.uuid;
    ScopedRuntimeConfig config = ScopedRuntimeConfig.get(uuid);
    assertEquals(uuid, config.uuid);
    assertEquals(uuid, config.customerUUID);
    assertNull(config.universeUUID);
    assertNull(config.providerUUID);

    assertTrue(defaultCustomer.deletePermanent());
    assertNull(ScopedRuntimeConfig.get(uuid));
  }

  @Test
  public void testEnsureGetCascadeDelete_universeConfig() {
    ScopedRuntimeConfig.ensure(defaultUniverse);
    UUID uuid = defaultUniverse.universeUUID;
    ScopedRuntimeConfig config = ScopedRuntimeConfig.get(uuid);
    assertEquals(uuid, config.uuid);
    assertEquals(uuid, config.universeUUID);
    assertNull(config.customerUUID);
    assertNull(config.providerUUID);

    assertTrue(defaultUniverse.deletePermanent());
    assertNull(ScopedRuntimeConfig.get(uuid));
  }

  @Test
  public void testEnsureGetCascadeDelete_providerConfig() {
    ScopedRuntimeConfig.ensure(defaultProvider);
    UUID uuid = defaultProvider.uuid;
    ScopedRuntimeConfig config = ScopedRuntimeConfig.get(uuid);
    assertEquals(uuid, config.uuid);
    assertEquals(uuid, config.providerUUID);
    assertNull(config.customerUUID);
    assertNull(config.universeUUID);

    assertTrue(defaultProvider.deletePermanent());
    assertNull(ScopedRuntimeConfig.get(uuid));
  }
}
