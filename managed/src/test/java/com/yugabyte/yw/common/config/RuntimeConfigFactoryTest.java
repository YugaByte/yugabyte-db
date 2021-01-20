package com.yugabyte.yw.common.config;

import com.google.common.collect.ImmutableMap;
import com.google.common.collect.ImmutableSet;
import com.typesafe.config.Config;
import com.typesafe.config.ConfigFactory;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;
import io.ebean.Model;
import org.junit.Before;
import org.junit.Test;

import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;

public class RuntimeConfigFactoryTest extends FakeDBApplication {

  // Key overridden in each scope
  public static final String YB_OVERRIDDEN_KEY = "yb.overridden.key";

  // Keys defined only in specific scope and never overridden
  public static final String YB_STATIC_ONLY_KEY = "yb.static.key";
  public static final String YB_GLOBAL_RUNTIME_ONLY_KEY = "yb.runtime.global";
  public static final String YB_CUSTOMER_RUNTIME_ONLY_KEY = "yb.runtime.customer";
  public static final String YB_PROVIDER_RUNTIME_ONLY_KEY = "yb.runtime.provider";
  public static final String YB_UNIVERSE_RUNTIME_ONLY_KEY = "yb.runtime.universe";

  // Key not defined in any scope
  public static final String YB_NOT_PRESENT_KEY = "yb.not.present";
  public static final String TASK_GC_FREQUENCY = "taskGC.frequency";

  enum Scope {
    STATIC,
    GLOBAL,
    CUSTOMER,
    PROVIDER,
    UNIVERSE
  }

  // app config
  private static final Map<String, String> staticConfigMap =
    ImmutableMap.of(
      YB_STATIC_ONLY_KEY, Scope.STATIC.toString(),
      YB_OVERRIDDEN_KEY, Scope.STATIC.toString());

  // overrides in global scope:
  private static final Set<String> globalConfigSet =
    ImmutableSet.of(
      YB_GLOBAL_RUNTIME_ONLY_KEY,
      YB_OVERRIDDEN_KEY);

  // overrides in customer scope:
  private static final Set<String> customerConfigSet =
    ImmutableSet.of(
      YB_CUSTOMER_RUNTIME_ONLY_KEY,
      YB_OVERRIDDEN_KEY);

  // overrides in provider scope:
  private static final Set<String> providerConfigSet =
    ImmutableSet.of(
      YB_PROVIDER_RUNTIME_ONLY_KEY,
      YB_OVERRIDDEN_KEY);

  // overrides in provider scope:
  private static final Set<String> universeConfigSet =
    ImmutableSet.of(
      YB_UNIVERSE_RUNTIME_ONLY_KEY,
      YB_OVERRIDDEN_KEY);

  private Customer defaultCustomer;
  private Universe defaultUniverse;
  private Provider defaultProvider;

  RuntimeConfigFactory configFactory =
    new RuntimeConfigFactory(ConfigFactory.parseMap(staticConfigMap));

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultUniverse = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    defaultProvider = ModelFactory.kubernetesProvider(defaultCustomer);
  }

  @Test
  public void testStatic() {
    // Make sure factory returns injected app config
    validateStaticValues(configFactory.staticApplicationConf());
  }

  @Test
  public void testGlobal_empty() {
    // Nothing set into global scope so global values are same as static conf values.
    validateStaticValues(configFactory.globalRuntimeConf());
  }

  @Test
  public void testGlobal() {
    RuntimeConfig<Model> globalConfig = setupGlobalConfig();
    validateGlobalValues(globalConfig);
    // hit db again
    validateGlobalValues(configFactory.globalRuntimeConf());
  }

  @Test
  public void testForCustomer_empty() {
    setupGlobalConfig();
    RuntimeConfig<Customer> customerScopedRuntimeConfig =
      configFactory.forCustomer(defaultCustomer);
    // Nothing set into customer scope so customer values are same as global conf values.
    validateGlobalValues(customerScopedRuntimeConfig);
  }

  @Test
  public void testForCustomer() {
    RuntimeConfig<Customer> customerConfig = setupCustomerConfig();
    validateCustomerValues(customerConfig);
    // modifying customer level should not mod global config.
    validateGlobalValues(configFactory.globalRuntimeConf());
    // fetch from db and check again:
    validateCustomerValues(configFactory.forCustomer(defaultCustomer));
  }

  @Test
  public void testTwoCustomers() {
    Customer customer1 = ModelFactory.testCustomer();
    Customer customer2 = ModelFactory.testCustomer();
    configFactory.forCustomer(customer1)
      .setValue(TASK_GC_FREQUENCY, "1 day");
    configFactory.forCustomer(customer2)
      .setValue(TASK_GC_FREQUENCY, "2 days");

    assertEquals(1L, configFactory.forCustomer(customer1).getDuration(TASK_GC_FREQUENCY).toDays());
    assertEquals(2L, configFactory.forCustomer(customer2).getDuration(TASK_GC_FREQUENCY).toDays());
  }

  @Test
  public void testForProvider_empty() {
    setupCustomerConfig();
    RuntimeConfig<Provider> providerConfig = configFactory.forProvider(defaultProvider);
    // Nothing set into provider scope so provider values are same as customer conf values.
    validateCustomerValues(providerConfig);
  }

  @Test
  public void testForProvider() {
    RuntimeConfig<Provider> providerConfig = setupProviderConfig();
    validateProviderValues(providerConfig);

    // modifying provider level should not mod customer config.
    validateCustomerValues(configFactory.forCustomer(Customer.get(defaultProvider.customerUUID)));

    // modifying provider level should not mod global config.
    validateGlobalValues(configFactory.globalRuntimeConf());

    // fetch from db and check again:
    validateProviderValues(configFactory.forProvider(defaultProvider));
  }

  @Test
  public void testTwoProviders() {
    Provider awsProvider = ModelFactory.awsProvider(defaultCustomer);
    Provider gcpProvider = ModelFactory.gcpProvider(defaultCustomer);
    configFactory.forProvider(awsProvider)
      .setValue(TASK_GC_FREQUENCY, "1 day");
    configFactory.forProvider(gcpProvider)
      .setValue(TASK_GC_FREQUENCY, "2 days");

    assertEquals(1L,
      configFactory.forProvider(awsProvider).getDuration(TASK_GC_FREQUENCY).toDays());
    assertEquals(2L,
      configFactory.forProvider(gcpProvider).getDuration(TASK_GC_FREQUENCY).toDays());
  }

  @Test
  public void testForUniverse_empty() {
    setupCustomerConfig();
    RuntimeConfig<Universe> universeConfig = configFactory.forUniverse(defaultUniverse);
    // Nothing set into provider scope so provider values are same as customer conf values.
    validateCustomerValues(universeConfig);
  }

  @Test
  public void testForUniverse() {
    RuntimeConfig<Universe> universeConfig = setupUniverseConfig();
    validateUniverseValues(universeConfig);

    // modifying universe level should not mod customer config.
    validateCustomerValues(configFactory.forCustomer(Customer.get(defaultUniverse.customerId)));

    // modifying universe level should not mod global config.
    validateGlobalValues(configFactory.globalRuntimeConf());

    // fetch from db and check again:
    validateUniverseValues(configFactory.forUniverse(defaultUniverse));
  }

  @Test
  public void testTwoUniverses() {
    Universe universe1 = ModelFactory.createUniverse("USA", defaultCustomer.getCustomerId());
    Universe universe2 = ModelFactory.createUniverse("Asia", defaultCustomer.getCustomerId());
    configFactory.forUniverse(universe1)
      .setValue(TASK_GC_FREQUENCY, "1 day");
    configFactory.forUniverse(universe2)
      .setValue(TASK_GC_FREQUENCY, "2 days");

    assertEquals(1L,
      configFactory.forUniverse(universe1).getDuration(TASK_GC_FREQUENCY).toDays());
    assertEquals(2L,
      configFactory.forUniverse(universe2).getDuration(TASK_GC_FREQUENCY).toDays());
  }

  private RuntimeConfig<Model> setupGlobalConfig() {
    RuntimeConfig<Model> runtimeConfig = configFactory.globalRuntimeConf();
    globalConfigSet.forEach(s -> runtimeConfig.setValue(s, Scope.GLOBAL.name()));
    return runtimeConfig;
  }

  private RuntimeConfig<Customer> setupCustomerConfig() {
    setupGlobalConfig();
    RuntimeConfig<Customer> customerConfig = configFactory.forCustomer(defaultCustomer);
    customerConfigSet.forEach(s -> customerConfig.setValue(s, Scope.CUSTOMER.name()));
    return customerConfig;
  }

  private RuntimeConfig<Provider> setupProviderConfig() {
    setupCustomerConfig();
    RuntimeConfig<Provider> providerConfig = configFactory.forProvider(defaultProvider);
    providerConfigSet.forEach(s -> providerConfig.setValue(s, Scope.PROVIDER.name()));
    return providerConfig;
  }

  private RuntimeConfig<Universe> setupUniverseConfig() {
    setupCustomerConfig();
    RuntimeConfig<Universe> universeConfig = configFactory.forUniverse(defaultUniverse);
    universeConfigSet.forEach(s -> universeConfig.setValue(s, Scope.UNIVERSE.name()));
    return universeConfig;
  }

  private void validateStaticValues(Config runtimeConfig) {
    assertEquals(Scope.STATIC, runtimeConfig.getEnum(Scope.class, YB_OVERRIDDEN_KEY));

    assertEquals(Scope.STATIC, runtimeConfig.getEnum(Scope.class, YB_STATIC_ONLY_KEY));

    assertFalse(runtimeConfig.hasPath(YB_GLOBAL_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_CUSTOMER_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_PROVIDER_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_UNIVERSE_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_NOT_PRESENT_KEY));
  }

  private void validateGlobalValues(Config runtimeConfig) {
    assertEquals(Scope.GLOBAL, runtimeConfig.getEnum(Scope.class, YB_OVERRIDDEN_KEY));

    assertEquals(Scope.STATIC, runtimeConfig.getEnum(Scope.class, YB_STATIC_ONLY_KEY));
    assertEquals(Scope.GLOBAL, runtimeConfig.getEnum(Scope.class, YB_GLOBAL_RUNTIME_ONLY_KEY));

    assertFalse(runtimeConfig.hasPath(YB_CUSTOMER_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_PROVIDER_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_UNIVERSE_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_NOT_PRESENT_KEY));
  }

  private void validateCustomerValues(Config runtimeConfig) {
    assertEquals(Scope.CUSTOMER, runtimeConfig.getEnum(Scope.class, YB_OVERRIDDEN_KEY));

    assertEquals(Scope.STATIC, runtimeConfig.getEnum(Scope.class, YB_STATIC_ONLY_KEY));
    assertEquals(Scope.GLOBAL, runtimeConfig.getEnum(Scope.class, YB_GLOBAL_RUNTIME_ONLY_KEY));
    assertEquals(Scope.CUSTOMER, runtimeConfig.getEnum(Scope.class, YB_CUSTOMER_RUNTIME_ONLY_KEY));

    assertFalse(runtimeConfig.hasPath(YB_PROVIDER_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_UNIVERSE_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_NOT_PRESENT_KEY));
  }

  private void validateProviderValues(Config runtimeConfig) {
    assertEquals(Scope.PROVIDER, runtimeConfig.getEnum(Scope.class, YB_OVERRIDDEN_KEY));

    assertEquals(Scope.STATIC, runtimeConfig.getEnum(Scope.class, YB_STATIC_ONLY_KEY));
    assertEquals(Scope.GLOBAL, runtimeConfig.getEnum(Scope.class, YB_GLOBAL_RUNTIME_ONLY_KEY));
    assertEquals(Scope.CUSTOMER, runtimeConfig.getEnum(Scope.class, YB_CUSTOMER_RUNTIME_ONLY_KEY));
    assertEquals(Scope.PROVIDER, runtimeConfig.getEnum(Scope.class, YB_PROVIDER_RUNTIME_ONLY_KEY));

    assertFalse(runtimeConfig.hasPath(YB_UNIVERSE_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_NOT_PRESENT_KEY));
  }

  private void validateUniverseValues(Config runtimeConfig) {
    assertEquals(Scope.UNIVERSE, runtimeConfig.getEnum(Scope.class, YB_OVERRIDDEN_KEY));

    assertEquals(Scope.STATIC, runtimeConfig.getEnum(Scope.class, YB_STATIC_ONLY_KEY));
    assertEquals(Scope.GLOBAL, runtimeConfig.getEnum(Scope.class, YB_GLOBAL_RUNTIME_ONLY_KEY));
    assertEquals(Scope.CUSTOMER, runtimeConfig.getEnum(Scope.class, YB_CUSTOMER_RUNTIME_ONLY_KEY));
    assertEquals(Scope.UNIVERSE, runtimeConfig.getEnum(Scope.class, YB_UNIVERSE_RUNTIME_ONLY_KEY));

    assertFalse(runtimeConfig.hasPath(YB_PROVIDER_RUNTIME_ONLY_KEY));
    assertFalse(runtimeConfig.hasPath(YB_NOT_PRESENT_KEY));
  }
}
