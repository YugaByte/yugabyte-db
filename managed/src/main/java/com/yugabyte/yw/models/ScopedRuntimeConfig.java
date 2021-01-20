package com.yugabyte.yw.models;

import com.google.common.annotations.VisibleForTesting;
import io.ebean.Finder;
import io.ebean.Model;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import javax.persistence.Entity;
import javax.persistence.Id;
import java.util.UUID;

/**
 * This bean is a parent for ConfigEntries bean. Intent is to identify the
 * scope of the child ConfigEntries.
 * This bean just points to one of many parent tables through a foreign key.
 * At present we are only adding such a pointer to provider and customer tables,
 * so that we can have scoped configurations for provider or customer.
 */
@Entity
public class ScopedRuntimeConfig extends Model {
  // TODO: delete if not needed by the end of it.
//  public static final ScopedRuntimeConfig GLOBAL_SCOPE = new ScopedRuntimeConfig();
  public static final UUID GLOBAL_SCOPE_UUID = new UUID(0, 0);

  private static final Logger LOG = LoggerFactory.getLogger(ScopedRuntimeConfig.class);

  // This is not a foreign key but for ease of debugging
  // we  will reuse the uuid of parent entity. If there is
  // no uuid defined for parent entity we will
  // create a new uuid. At present all the use cases
  // global, provider, universe and customer have a uuid.
  @VisibleForTesting
  @Id
  final UUID uuid;

  //====================================================
  // Foreign keys to scoping entities.
  // At most one of these can be valid.
  @VisibleForTesting
  final UUID customerUUID;
  @VisibleForTesting
  final UUID universeUUID;
  @VisibleForTesting
  final UUID providerUUID;
  // End foreign key fields
  //====================================================

  private ScopedRuntimeConfig() {
    uuid = GLOBAL_SCOPE_UUID;
    customerUUID = null;
    universeUUID = null;
    providerUUID = null;
    LOG.info("Created Global ScopedRuntimeConfig");
  }

  private ScopedRuntimeConfig(Customer customer) {
    uuid = customer.uuid;
    customerUUID = customer.uuid;
    universeUUID = null;
    providerUUID = null;
    LOG.info("Created Customer({}) ScopedRuntimeConfig", customerUUID);
  }

  private ScopedRuntimeConfig(Universe universe) {
    uuid = universe.universeUUID;
    providerUUID = null;
    customerUUID = null;
    universeUUID = universe.universeUUID;
    LOG.info("Created Universe({}) ScopedRuntimeConfig", universeUUID);
  }

  private ScopedRuntimeConfig(Provider provider) {
    uuid = provider.uuid;
    customerUUID = null;
    universeUUID = null;
    providerUUID = provider.uuid;
    LOG.info("Created Provider({}) ScopedRuntimeConfig", providerUUID);
  }

  private static final Finder<UUID, ScopedRuntimeConfig> finder =
    new Finder<UUID, ScopedRuntimeConfig>(ScopedRuntimeConfig.class) {
    };

  @VisibleForTesting
  static ScopedRuntimeConfig get(UUID uuid) {
    return finder.byId(uuid);
  }

  // Below methods only used from RuntimeConfigEntry bean to ensure that scope is created
  // before creating the bean.
  static void ensureGlobal() {
    if (get(GLOBAL_SCOPE_UUID) == null) {
      new ScopedRuntimeConfig().save();
    }
  }

  static void ensure(Customer customer) {
    if (get(customer.uuid) == null) {
      new ScopedRuntimeConfig(customer).save();
    }
  }

  static void ensure(Universe universe) {
    if (get(universe.universeUUID) == null) {
      new ScopedRuntimeConfig(universe).save();
    }
  }

  static void ensure(Provider provider) {
    if (get(provider.uuid) == null) {
      new ScopedRuntimeConfig(provider).save();
    }
  }

  @Override
  public String toString() {
    if (uuid == GLOBAL_SCOPE_UUID) {
      return "ScopedRuntimeConfig(GLOBAL_SCOPE)";
    } else {
      return "ScopedRuntimeConfig{" +
        "uuid=" + uuid +
        ", customerUUID=" + customerUUID +
        ", universeUUID=" + universeUUID +
        ", providerUUID=" + providerUUID +
        '}';
    }
  }
}
