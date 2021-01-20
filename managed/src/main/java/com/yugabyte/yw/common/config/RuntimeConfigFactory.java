package com.yugabyte.yw.common.config;

import com.google.common.annotations.VisibleForTesting;
import com.typesafe.config.Config;
import com.typesafe.config.ConfigFactory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.RuntimeConfigEntry;
import com.yugabyte.yw.models.Universe;
import io.ebean.Model;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import javax.inject.Inject;
import javax.inject.Singleton;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static com.yugabyte.yw.models.ScopedRuntimeConfig.GLOBAL_SCOPE_UUID;
import static java.util.stream.Collectors.toMap;

/**
 * Factory to create RuntimeConfig for various scopes
 */
@Singleton
public class RuntimeConfigFactory {
  private static final Logger LOG = LoggerFactory.getLogger(RuntimeConfigFactory.class);

  private final Config appConfig;

  @Inject
  public RuntimeConfigFactory(Config appConfig) {
    this.appConfig = appConfig;
  }

  /**
   * @return A RuntimeConfig instance for a given scope
   */
  public RuntimeConfig<Customer> forCustomer(Customer customer) {
    Config config = getConfigForScope(
      customer.uuid, "Scoped Config (" + customer.toString() + ")")
      .withFallback(globalConfig());
    LOG.debug("forCustomer {}: {}", customer.uuid, config);
    return new RuntimeConfig<>(customer, config);
  }

  /**
   * @return A RuntimeConfig instance for a given scope
   */
  public RuntimeConfig<Universe> forUniverse(Universe universe) {
    Customer customer = Customer.get(universe.customerId);
    Config config = getConfigForScope(
      universe.universeUUID, "Scoped Config (" + universe.toString() + ")")
      .withFallback(getConfigForScope(customer.uuid,
        "Scoped Config (" + customer.toString() + ")"))
      .withFallback(globalConfig());
    LOG.debug("forUniverse {}: {}", universe.universeUUID, config);
    return new RuntimeConfig<>(universe, config);
  }

  /**
   * @return A RuntimeConfig instance for a given scope
   */
  public RuntimeConfig<Provider> forProvider(Provider provider) {
    Customer customer = Customer.get(provider.customerUUID);
    Config config = getConfigForScope(
      provider.uuid, "Scoped Config (" + provider.toString() + ")")
      .withFallback(getConfigForScope(customer.uuid,
        "Scoped Config (" + customer.toString() + ")"))
      .withFallback(globalConfig());
    LOG.debug("forProvider {}: {}", provider.uuid, config);
    return new RuntimeConfig<>(provider, config);
  }

  /**
   * @return A RuntimeConfig instance for a GLOBAL_SCOPE
   */
  public RuntimeConfig<Model> globalRuntimeConf() {
    return new RuntimeConfig<>(globalConfig());
  }

  public Config staticApplicationConf() {
    return appConfig;
  }

  private Config globalConfig() {
    Config config = getConfigForScope(GLOBAL_SCOPE_UUID,
      "Global Runtime Config (" + GLOBAL_SCOPE_UUID.toString() + ")")
      .withFallback(appConfig);
    LOG.debug("globalConfig : {}", config);
    return config;
  }

  @VisibleForTesting
  Config getConfigForScope(UUID scope, String description) {
    List<RuntimeConfigEntry> scopedValues =
      RuntimeConfigEntry.getAll(scope);
    Map<String, String> values = scopedValues
      .stream()
      .collect(toMap(RuntimeConfigEntry::getPath, RuntimeConfigEntry::getValue));
    Config config = ConfigFactory.parseMap(values, description);
    LOG.debug("Read from DB for {}: {}", description, config);
    return config;
  }
}
