package com.yugabyte.yw.models;

import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import org.junit.Before;
import org.junit.Test;

import java.util.List;
import java.util.UUID;

import static org.junit.Assert.*;

public class RuntimeConfigEntryTest extends FakeDBApplication {

  public static final String YB_SB_START_YEAR_KEY = "yb.sb.startYear";
  public static final String YB_SB_TIMEZONE_KEY = "yb.sb.timezone";
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
  public void testCreateLookupGetAllValidScopeCascadeDelete_global() {
    UUID scopeUuid = ScopedRuntimeConfig.GLOBAL_SCOPE_UUID;
    RuntimeConfigEntry.upsertGlobal(YB_SB_START_YEAR_KEY, "1886");
    checkStartYear(scopeUuid, "1886");
    // Make sure that the config with same scope and path is unique and later update overwrites
    // the former.
    RuntimeConfigEntry.upsertGlobal(YB_SB_START_YEAR_KEY, "2020");
    checkStartYear(scopeUuid, "2020");

    RuntimeConfigEntry.upsertGlobal(YB_SB_TIMEZONE_KEY, "PST");
    checkAll(scopeUuid);

    ScopedRuntimeConfig scopedRuntimeConfig = ScopedRuntimeConfig.get(scopeUuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.uuid);
    assertNull(scopedRuntimeConfig.customerUUID);
    assertNull(scopedRuntimeConfig.providerUUID);
    assertNull(scopedRuntimeConfig.universeUUID);

    scopedRuntimeConfig.deletePermanent();
    checkCascadeDelete(scopeUuid);
  }

  @Test
  public void testCreateLookupGetAllValidScopeCascadeDelete_customer() {
    UUID scopeUuid = defaultCustomer.uuid;
    RuntimeConfigEntry.upsert(defaultCustomer, YB_SB_START_YEAR_KEY, "1857");
    checkStartYear(scopeUuid, "1857");
    // Make sure that the config with same scope and path is unique and later update overwrites
    // the former.
    RuntimeConfigEntry.upsert(defaultCustomer, YB_SB_START_YEAR_KEY, "2020");
    checkStartYear(scopeUuid, "2020");

    RuntimeConfigEntry.upsert(defaultCustomer, YB_SB_TIMEZONE_KEY, "PST");
    checkAll(scopeUuid);

    ScopedRuntimeConfig scopedRuntimeConfig = ScopedRuntimeConfig.get(scopeUuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.uuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.customerUUID);
    assertNull(scopedRuntimeConfig.providerUUID);
    assertNull(scopedRuntimeConfig.universeUUID);

    defaultCustomer.deletePermanent();
    checkCascadeDelete(scopeUuid);
  }

  @Test
  public void testCreateLookupGetAllValidScopeCascadeDelete_universe() {
    UUID scopeUuid = defaultUniverse.universeUUID;
    RuntimeConfigEntry.upsert(defaultUniverse, YB_SB_START_YEAR_KEY, "2021");
    checkStartYear(scopeUuid, "2021");
    // Make sure that the config with same scope and path is unique and later update overwrites
    // the former.
    RuntimeConfigEntry.upsert(defaultUniverse, YB_SB_START_YEAR_KEY, "2020");
    checkStartYear(scopeUuid, "2020");

    RuntimeConfigEntry.upsert(defaultUniverse, YB_SB_TIMEZONE_KEY, "PST");
    checkAll(scopeUuid);

    ScopedRuntimeConfig scopedRuntimeConfig = ScopedRuntimeConfig.get(scopeUuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.uuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.universeUUID);
    assertNull(scopedRuntimeConfig.providerUUID);
    assertNull(scopedRuntimeConfig.customerUUID);

    defaultUniverse.deletePermanent();
    checkCascadeDelete(scopeUuid);
  }

  @Test
  public void testCreateLookupGetAllValidScopeCascadeDelete_provider() {
    UUID scopeUuid = defaultProvider.uuid;
    RuntimeConfigEntry.upsert(defaultProvider, YB_SB_START_YEAR_KEY, "1984");
    checkStartYear(scopeUuid, "1984");
    // Make sure that the config with same scope and path is unique and later update overwrites
    // the former.
    RuntimeConfigEntry.upsert(defaultProvider, YB_SB_START_YEAR_KEY, "2020");
    checkStartYear(scopeUuid, "2020");

    RuntimeConfigEntry.upsert(defaultProvider, YB_SB_TIMEZONE_KEY, "PST");
    checkAll(scopeUuid);

    ScopedRuntimeConfig scopedRuntimeConfig = ScopedRuntimeConfig.get(scopeUuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.uuid);
    assertEquals(scopeUuid, scopedRuntimeConfig.providerUUID);
    assertNull(scopedRuntimeConfig.universeUUID);
    assertNull(scopedRuntimeConfig.customerUUID);

    defaultProvider.deletePermanent();
    checkCascadeDelete(scopeUuid);
  }

  private void checkCascadeDelete(UUID scopeUuid) {
    assertNull(ScopedRuntimeConfig.get(scopeUuid));
    assertNull(RuntimeConfigEntry.get(scopeUuid, YB_SB_START_YEAR_KEY));
    assertTrue(RuntimeConfigEntry.getAll(scopeUuid).isEmpty());
  }

  private void checkStartYear(UUID scopeUuid, String expected) {
    RuntimeConfigEntry entry = RuntimeConfigEntry.get(scopeUuid, YB_SB_START_YEAR_KEY);
    assertNotNull(entry);
    assertEquals(YB_SB_START_YEAR_KEY, entry.getPath());
    assertEquals(expected, entry.getValue());
  }

  private void checkAll(UUID scopeUuid) {
    List<RuntimeConfigEntry> entryList = RuntimeConfigEntry.getAll(scopeUuid);
    assertSame(2, entryList.size());
    entryList.forEach(runtimeConfigEntry -> {
      switch (runtimeConfigEntry.getPath()) {
        case YB_SB_START_YEAR_KEY:
          assertEquals("2020", runtimeConfigEntry.getValue());
          break;
        case YB_SB_TIMEZONE_KEY:
          assertEquals("PST", runtimeConfigEntry.getValue());
          break;
        default:
          fail("Unexpected path " + runtimeConfigEntry.getPath()
            + "for config entry: " + runtimeConfigEntry);
      }
    });
  }
}
