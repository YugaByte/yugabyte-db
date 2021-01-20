// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.CertificateHelper;
import com.yugabyte.yw.common.KubernetesManager;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.RegexMatcher;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.CertificateInfo;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.DeviceInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import org.apache.commons.io.FileUtils;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.mockito.ArgumentCaptor;
import org.yaml.snakeyaml.Yaml;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.test.Helpers;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;

import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;

import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertTrue;
import static org.mockito.Matchers.any;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static play.inject.Bindings.bind;

public class KubernetesCommandExecutorTest extends SubTaskBaseTest {
  KubernetesManager kubernetesManager;
  Provider defaultProvider;
  Universe defaultUniverse;
  Region defaultRegion;
  AvailabilityZone defaultAZ;
  CertificateInfo defaultCert;
  // TODO: when trying to fetch the cluster UUID directly, we get:
  // javax.persistence.EntityNotFoundException: Bean not found during lazy load or refresh
  //
  // This is mostly because we cache the defaultUniverse, but actually update the universe itself
  // through calls to saveDetails, which OBVIOUSLY will not be reflected into the defaultUniverse
  // field in the test...
  UUID hackPlacementUUID;
  UniverseDefinitionTaskParams.UserIntent defaultUserIntent;
  InstanceType instanceType;
  String ybSoftwareVersion = "1.0.0";
  int numNodes = 3;
  Map<String, String> config= new HashMap<String, String>();

  protected CallbackController mockCallbackController;
  protected PlayCacheSessionStore mockSessionStore;

  @Override
  protected Application provideApplication() {
    kubernetesManager = mock(KubernetesManager.class);
    mockCallbackController = mock(CallbackController.class);
    mockSessionStore = mock(PlayCacheSessionStore.class);
    return new GuiceApplicationBuilder()
        .configure((Map) Helpers.inMemoryDatabase())
        .overrides(bind(KubernetesManager.class).toInstance(kubernetesManager))
        .overrides(bind(CallbackController.class).toInstance(mockCallbackController))
        .overrides(bind(PlaySessionStore.class).toInstance(mockSessionStore))
        .build();
  }

  @Before
  public void setUp() {
    super.setUp();
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
    config.put("KUBECONFIG", "test");
    defaultProvider.setConfig(config);
    defaultProvider.save();
    defaultRegion = Region.create(defaultProvider, "region-1", "PlacementRegion 1", "default-image");
    defaultAZ = AvailabilityZone.create(defaultRegion, "az-1", "PlacementAZ 1", "subnet-1");
    defaultUniverse = ModelFactory.createUniverse(defaultCustomer.getCustomerId());
    defaultUniverse = updateUniverseDetails("small");
    defaultCert = CertificateInfo.get(CertificateHelper.createRootCA(
        defaultUniverse.getUniverseDetails().nodePrefix,
        defaultProvider.customerUUID, "/tmp/certs"));
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V3.toString()));
  }

  @After
  public void tearDown() throws IOException {
    FileUtils.deleteDirectory(new File("/tmp/certs"));
  }

  private Universe updateUniverseDetails(String instanceTypeCode) {
    instanceType = InstanceType.upsert(defaultProvider.code, instanceTypeCode,
        10, 5.5, new InstanceType.InstanceTypeDetails());
    defaultUserIntent = getTestUserIntent(
        defaultRegion, defaultProvider, instanceType, numNodes);
    defaultUserIntent.replicationFactor = 3;
    defaultUserIntent.masterGFlags = new HashMap<>();
    defaultUserIntent.tserverGFlags = new HashMap<>();
    defaultUserIntent.universeName = "demo-universe";
    defaultUserIntent.ybSoftwareVersion = ybSoftwareVersion;
    defaultUserIntent.enableYSQL = true;
    Universe u = Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(defaultUserIntent, "host", true));
    hackPlacementUUID = u.getUniverseDetails().getPrimaryCluster().uuid;
    return u;
  }

  private KubernetesCommandExecutor createExecutor(KubernetesCommandExecutor.CommandType commandType) {
    KubernetesCommandExecutor kubernetesCommandExecutor = new KubernetesCommandExecutor();
    KubernetesCommandExecutor.Params params = new KubernetesCommandExecutor.Params();
    params.providerUUID = defaultProvider.uuid;
    params.commandType = commandType;
    params.config = config;
    params.nodePrefix = defaultUniverse.getUniverseDetails().nodePrefix;
    params.universeUUID = defaultUniverse.universeUUID;
    kubernetesCommandExecutor.initialize(params);
    return kubernetesCommandExecutor;
  }

  private KubernetesCommandExecutor createExecutor(KubernetesCommandExecutor.CommandType commandType,
                                                   PlacementInfo placementInfo) {
    KubernetesCommandExecutor kubernetesCommandExecutor = new KubernetesCommandExecutor();
    KubernetesCommandExecutor.Params params = new KubernetesCommandExecutor.Params();
    params.providerUUID = defaultProvider.uuid;
    params.commandType = commandType;
    params.nodePrefix = defaultUniverse.getUniverseDetails().nodePrefix;
    params.universeUUID = defaultUniverse.universeUUID;
    params.config = config;
    params.placementInfo = placementInfo;
    kubernetesCommandExecutor.initialize(params);
    return kubernetesCommandExecutor;
  }

  private Map<String, Object> getExpectedOverrides(boolean exposeAll) {
    Yaml yaml = new Yaml();
    Map<String, Object> expectedOverrides = new HashMap<>();
    if (exposeAll) {
      expectedOverrides = (HashMap<String, Object>) yaml.load(
          provideApplication().resourceAsStream("k8s-expose-all.yml")
      );
    }
    double burstVal = 1.2;
    Map<String, String> config = defaultProvider.getConfig();

    Map<String, Object> storageOverrides = (HashMap) expectedOverrides.getOrDefault("storage", new HashMap<>());
    if (defaultUserIntent.deviceInfo != null) {
      Map<String, Object> tserverDiskSpecs = (HashMap) storageOverrides.getOrDefault("tserver", new HashMap<>());
      Map<String, Object> masterDiskSpecs = (HashMap) storageOverrides.getOrDefault("master", new HashMap<>());

      if (defaultUserIntent.deviceInfo.numVolumes != null) {
        tserverDiskSpecs.put("count", defaultUserIntent.deviceInfo.numVolumes);
      }
      if (defaultUserIntent.deviceInfo.volumeSize != null) {
        tserverDiskSpecs.put("size", String.format("%dGi", defaultUserIntent.deviceInfo.volumeSize));
      }
      if (defaultUserIntent.deviceInfo.storageClass != null) {
        tserverDiskSpecs.put("storageClass", defaultUserIntent.deviceInfo.storageClass);
        masterDiskSpecs.put("storageClass", defaultUserIntent.deviceInfo.storageClass);
      }
      storageOverrides.put("tserver", tserverDiskSpecs);
      storageOverrides.put("master", masterDiskSpecs);
    }

    expectedOverrides.put("replicas", ImmutableMap.of("tserver", numNodes,
        "master", defaultUserIntent.replicationFactor));

    Map<String, Object> resourceOverrides = new HashMap();

    Map<String, Object> tserverResource = new HashMap<>();
    Map<String, Object> tserverLimit = new HashMap<>();
    tserverResource.put("cpu", instanceType.numCores);
    tserverResource.put("memory", String.format("%.2fGi", instanceType.memSizeGB));
    tserverLimit.put("cpu", instanceType.numCores * burstVal);
    tserverLimit.put("memory", String.format("%.2fGi", instanceType.memSizeGB));
    resourceOverrides.put("tserver", ImmutableMap.of("requests", tserverResource, "limits", tserverLimit));

    Map<String, Object> masterResource = new HashMap<>();
    Map<String, Object> masterLimit = new HashMap<>();

    if (!instanceType.getInstanceTypeCode().equals("xsmall") &&
        !instanceType.getInstanceTypeCode().equals("dev")) {
      masterResource.put("cpu", 2);
      masterResource.put("memory", "4Gi");
      masterLimit.put("cpu", 2 * burstVal);
      masterLimit.put("memory", "4Gi");
      resourceOverrides.put("master", ImmutableMap.of("requests", masterResource, "limits", masterLimit));
    }

    if (instanceType.getInstanceTypeCode().equals("dev")) {
      masterResource.put("cpu", 0.5);
      masterResource.put("memory", "0.5Gi");
      masterLimit.put("cpu", 0.5);
      masterLimit.put("memory", "0.5Gi");
      resourceOverrides.put("master", ImmutableMap.of("requests", masterResource, "limits", masterLimit));
    }

    expectedOverrides.put("resource", resourceOverrides);

    expectedOverrides.put("Image", ImmutableMap.of("tag", ybSoftwareVersion));
    if (defaultUserIntent.enableNodeToNodeEncrypt || defaultUserIntent.enableClientToNodeEncrypt) {
      Map<String, Object> tlsInfo = new HashMap<>();
      tlsInfo.put("enabled", true);
      tlsInfo.put("insecure", true);
      Map<String, Object> rootCA = new HashMap<>();
      rootCA.put("cert", CertificateHelper.getCertPEM(defaultCert));
      rootCA.put("key", CertificateHelper.getKeyPEM(defaultCert));
      tlsInfo.put("rootCA", rootCA);
      expectedOverrides.put("tls", tlsInfo);
    }
    if (defaultUserIntent.enableIPV6) {
      expectedOverrides.put("ip_version_support", "v6_only");
    }

    Map<String, Object> partition = new HashMap<>();
    partition.put("tserver", 0);
    partition.put("master", 0);
    expectedOverrides.put("partition", partition);

    // All flags as overrides.
    Map<String, Object> gflagOverrides = new HashMap<>();
    // Master flags.
    Map<String, Object> masterOverrides = new HashMap<String, Object>(defaultUserIntent.masterGFlags);
    masterOverrides.put("placement_cloud", defaultProvider.code);
    masterOverrides.put("placement_region", defaultRegion.code);
    masterOverrides.put("placement_zone", defaultAZ.code);
    // masterOverrides.put("placement_uuid", defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    masterOverrides.put("placement_uuid", hackPlacementUUID.toString());
    gflagOverrides.put("master", masterOverrides);

    // Tserver flags.
    Map<String, Object> tserverOverrides = new HashMap<String, Object>(defaultUserIntent.tserverGFlags);
    tserverOverrides.put("placement_cloud", defaultProvider.code);
    tserverOverrides.put("placement_region", defaultRegion.code);
    tserverOverrides.put("placement_zone", defaultAZ.code);
    // tserverOverrides.put("placement_uuid", defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    tserverOverrides.put("placement_uuid", hackPlacementUUID.toString());

    gflagOverrides.put("tserver", tserverOverrides);
    // Put all the flags together.
    expectedOverrides.put("gflags", gflagOverrides);

    Map<String, String> azConfig = defaultAZ.getConfig();
    Map<String, String> regionConfig = defaultRegion.getConfig();


    Map<String, Object> annotations = new HashMap<String, Object>();
    String overridesYAML = null;
    if (!azConfig.containsKey("OVERRIDES")) {
      if (!regionConfig.containsKey("OVERRIDES")) {
        if (config.containsKey("OVERRIDES")) {
          overridesYAML = config.get("OVERRIDES");
        }
      } else {
        overridesYAML = regionConfig.get("OVERRIDES");
      }
    } else {
      overridesYAML = azConfig.get("OVERRIDES");
    }

    if (overridesYAML != null) {
      annotations =(HashMap<String, Object>) yaml.load(overridesYAML);
      if (annotations != null ) {
        expectedOverrides.putAll(annotations);
      }
    }
    expectedOverrides.put("disableYsql", !defaultUserIntent.enableYSQL);
    Map<String, String> universeConfig = defaultUniverse.getConfig();
    boolean helmLegacy = Universe.HelmLegacy.valueOf(universeConfig.get(Universe.HELM2_LEGACY))
        == Universe.HelmLegacy.V2TO3;
    if (helmLegacy) {
      expectedOverrides.put("helm2Legacy", helmLegacy);
    }

    return expectedOverrides;
  }

  @Test
  public void testHelmInstall() throws IOException {
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    assertEquals(getExpectedOverrides(true), overrides);
  }

    @Test
  public void testHelmInstallIPV6() throws IOException {
    defaultUserIntent.enableIPV6 = true;
    Universe u = Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(defaultUserIntent, "host", true));
    hackPlacementUUID = u.getUniverseDetails().getPrimaryCluster().uuid;

    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(),
                     expectedNodePrefix.capture(), expectedOverrideFile.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmInstallWithGflags() throws IOException {
    defaultUserIntent.masterGFlags = ImmutableMap.of("yb-master-flag", "demo-flag");
    defaultUserIntent.tserverGFlags = ImmutableMap.of("yb-tserver-flag", "demo-flag");
    defaultUserIntent.ybSoftwareVersion = ybSoftwareVersion;
    Universe u = Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(defaultUserIntent, "host", true));
    hackPlacementUUID = u.getUniverseDetails().getPrimaryCluster().uuid;

    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmInstallWithTLS() throws IOException {
    defaultUserIntent.masterGFlags = ImmutableMap.of("yb-master-flag", "demo-flag");
    defaultUserIntent.tserverGFlags = ImmutableMap.of("yb-tserver-flag", "demo-flag");
    defaultUserIntent.ybSoftwareVersion = ybSoftwareVersion;
    defaultUserIntent.enableNodeToNodeEncrypt = true;
    defaultUserIntent.enableClientToNodeEncrypt = true;
    Universe u = Universe.saveDetails(defaultUniverse.universeUUID,
      ApiUtils.mockUniverseUpdater(defaultUserIntent, "host", true));
    hackPlacementUUID = u.getUniverseDetails().getPrimaryCluster().uuid;
    KubernetesCommandExecutor kubernetesCommandExecutor =
      createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
      kubernetesCommandExecutor.taskParams().rootCA = defaultCert.uuid;
      kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }

  private void testHelmInstallForInstanceType(String instanceType) throws IOException {
    defaultUniverse = updateUniverseDetails(instanceType);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);
    Map<String, Object> resourceOverrides = (Map<String, Object>) overrides.get("resource");
    if (instanceType.equals("dev")) {
      assertTrue(resourceOverrides.containsKey("master"));
    } else {
      assertFalse(resourceOverrides.containsKey("master"));
    }

    assertTrue(resourceOverrides.containsKey("tserver"));
    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmInstallForXSmallInstance() throws IOException {
    testHelmInstallForInstanceType("xsmall");
  }

  @Test
  public void testHelmInstallForDevInstance() throws IOException {
    testHelmInstallForInstanceType("dev");
  }

  @Test
  public void testHelmInstallForAnnotations() throws IOException {
    Map<String, String> defaultAnnotations = new HashMap<String, String>();
    defaultAnnotations.put("OVERRIDES", "serviceEndpoints:\n  - name: yb-master-service\n    type: LoadBalancer\n    app: yb-master\n    annotations:\n      annotation-1: foo\n    ports:\n      ui: 7000\n\n  - name: yb-tserver-service\n    type: LoadBalancer\n    app: yb-tserver\n    ports:\n      ycql-port: 9042\n      yedis-port: 6379");
    defaultProvider.setConfig(defaultAnnotations);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmInstallForAnnotationsRegion() throws IOException {
    Map<String, String> defaultAnnotations = new HashMap<String, String>();
    defaultAnnotations.put("OVERRIDES", "serviceEndpoints:\n  - name: yb-master-service\n    type: LoadBalancer\n    app: yb-master\n    annotations:\n      annotation-1: bar\n    ports:\n      ui: 7000\n\n  - name: yb-tserver-service\n    type: LoadBalancer\n    app: yb-tserver\n    ports:\n      ycql-port: 9042\n      yedis-port: 6379");
    defaultRegion.setConfig(defaultAnnotations);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmInstallForAnnotationsAZ() throws IOException {
    Map<String, String> defaultAnnotations = new HashMap<String, String>();
    defaultAnnotations.put("OVERRIDES", "serviceEndpoints:\n  - name: yb-master-service\n    type: LoadBalancer\n    app: yb-master\n    annotations:\n      annotation-1: bar\n    ports:\n      ui: 7000\n\n  - name: yb-tserver-service\n    type: LoadBalancer\n    app: yb-tserver\n    ports:\n      ycql-port: 9042\n      yedis-port: 6379");
    defaultAZ.setConfig(defaultAnnotations);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmInstallForAnnotationsPrecendence() throws IOException {
    Map<String, String> defaultAnnotations = new HashMap<String, String>();
    defaultAnnotations.put("OVERRIDES", "foo: bar");
    defaultProvider.setConfig(defaultAnnotations);
    defaultAnnotations.put("OVERRIDES", "bar: foo");
    defaultAZ.setConfig(defaultAnnotations);

    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);
    assertEquals(overrides.get("bar"), "foo");
    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }


  @Test
  public void testHelmInstallWithStorageClass() throws IOException {
    defaultUserIntent.deviceInfo = new DeviceInfo();
    defaultUserIntent.deviceInfo.storageClass = "foo";
    Universe u = Universe.saveDetails(defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(defaultUserIntent, "host", true));
    hackPlacementUUID = u.getUniverseDetails().getPrimaryCluster().uuid;
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    kubernetesCommandExecutor.run();

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1))
        .helmInstall(expectedConfig.capture(), expectedProviderUUID.capture(), expectedNodePrefix.capture(),
            expectedOverrideFile.capture());
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(getExpectedOverrides(true), overrides);
  }

  @Test
  public void testHelmDelete() {
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_DELETE);
    kubernetesCommandExecutor.run();
    verify(kubernetesManager, times(1))
        .helmDelete(config, defaultUniverse.getUniverseDetails().nodePrefix);
  }

  @Test
  public void testVolumeDelete() {
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.VOLUME_DELETE);
    kubernetesCommandExecutor.run();
    verify(kubernetesManager, times(1))
        .deleteStorage(config, defaultUniverse.getUniverseDetails().nodePrefix);
  }

  @Test
  public void testNamespaceDelete() {
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.NAMESPACE_DELETE);
    kubernetesCommandExecutor.run();
    verify(kubernetesManager, times(1))
        .deleteNamespace(config, defaultUniverse.getUniverseDetails().nodePrefix);
  }

  @Test
  public void testPodInfo() {
    ShellResponse shellResponse = new ShellResponse();
    shellResponse.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\"," +
            " \"podIP\": \"123.456.78.90\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
        "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"123.456.78.91\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}," +
        "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"123.456.78.92\"}, \"spec\": {\"hostname\": \"yb-master-1\"}}," +
        "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\"," +
            " \"podIP\": \"123.456.78.93\"}, \"spec\": {\"hostname\": \"yb-tserver-1\"}}," +
        "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"123.456.78.94\"}, \"spec\": {\"hostname\": \"yb-master-2\"}}," +
        "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"123.456.78.95\"}, \"spec\": {\"hostname\": \"yb-tserver-2\"}}]}";
    when(kubernetesManager.getPodInfos(any(), any())).thenReturn(shellResponse);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.POD_INFO,
        defaultUniverse.getUniverseDetails().getPrimaryCluster().placementInfo);
    assertEquals(3, defaultUniverse.getNodes().size());
    kubernetesCommandExecutor.run();
    verify(kubernetesManager, times(1))
        .getPodInfos(config, defaultUniverse.getUniverseDetails().nodePrefix);
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);
    ImmutableList<String> pods = ImmutableList.of(
        "yb-master-0",
        "yb-master-1",
        "yb-master-2",
        "yb-tserver-0",
        "yb-tserver-1",
        "yb-tserver-2"
    );
    for (String podName : pods) {
      NodeDetails node = defaultUniverse.getNode(podName);
      assertNotNull(node);
      String serviceName = podName.contains("master") ? "yb-masters" : "yb-tservers";
      assertTrue(podName.contains("master") ? node.isMaster: node.isTserver);
      assertEquals(node.cloudInfo.private_ip, String.format("%s.%s.%s.%s", podName,
          serviceName, defaultUniverse.getUniverseDetails().nodePrefix, "svc.cluster.local"));
    }
  }

  @Test
  public void testPodInfoMultiAZ() {

    ShellResponse shellResponse = new ShellResponse();
    shellResponse.message =
        "{\"items\": [{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\"," +
            " \"podIP\": \"123.456.78.90\"}, \"spec\": {\"hostname\": \"yb-master-0\"}}," +
        "{\"status\": {\"startTime\": \"1234\", \"phase\": \"Running\", " +
            "\"podIP\": \"123.456.78.91\"}, \"spec\": {\"hostname\": \"yb-tserver-0\"}}]}";
    when(kubernetesManager.getPodInfos(any(), any())).thenReturn(shellResponse);

    Region r1 = Region.create(defaultProvider, "region-1", "region-1", "yb-image-1");
    Region r2 = Region.create(defaultProvider, "region-2", "region-2", "yb-image-1");
    AvailabilityZone az1 = AvailabilityZone.create(r1, "az-" + 1, "az-" + 1, "subnet-" + 1);
    AvailabilityZone az2 = AvailabilityZone.create(r1, "az-" + 2, "az-" + 2, "subnet-" + 2);
    AvailabilityZone az3 = AvailabilityZone.create(r2, "az-" + 3, "az-" + 3, "subnet-" + 3);
    PlacementInfo pi = new PlacementInfo();
    PlacementInfoUtil.addPlacementZone(az1.uuid, pi);
    PlacementInfoUtil.addPlacementZone(az2.uuid, pi);
    PlacementInfoUtil.addPlacementZone(az3.uuid, pi);

    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.POD_INFO, pi);
    kubernetesCommandExecutor.run();

    verify(kubernetesManager, times(1)).getPodInfos(config,
        String.format("%s-%s", defaultUniverse.getUniverseDetails().nodePrefix, "az-1"));
    verify(kubernetesManager, times(1)).getPodInfos(config,
        String.format("%s-%s", defaultUniverse.getUniverseDetails().nodePrefix, "az-2"));
    verify(kubernetesManager, times(1)).getPodInfos(config,
        String.format("%s-%s", defaultUniverse.getUniverseDetails().nodePrefix, "az-3"));
    defaultUniverse = Universe.get(defaultUniverse.universeUUID);
    ImmutableList<String> pods = ImmutableList.of(
        "yb-master-0_az-1",
        "yb-master-0_az-2",
        "yb-master-0_az-3",
        "yb-tserver-0_az-1",
        "yb-tserver-0_az-2",
        "yb-tserver-0_az-3"
    );
    Map<String, String> azToRegion = new HashMap();
    azToRegion.put("az-1", "region-1");
    azToRegion.put("az-2", "region-1");
    azToRegion.put("az-3", "region-2");
    for (String podName : pods) {
      NodeDetails node = defaultUniverse.getNode(podName);
      assertNotNull(node);
      String serviceName = podName.contains("master") ? "yb-masters" : "yb-tservers";

      assertTrue(podName.contains("master") ? node.isMaster: node.isTserver);

      String az = podName.split("_")[1];
      String podK8sName = podName.split("_")[0];
      assertEquals(node.cloudInfo.private_ip, String.format("%s.%s.%s.%s", podK8sName, serviceName,
          String.format("%s-%s", defaultUniverse.getUniverseDetails().nodePrefix, az),
          "svc.cluster.local"));
      assertEquals(node.cloudInfo.az, az);
      assertEquals(node.cloudInfo.region, azToRegion.get(az));
    }
  }

  @Test
  public void testHelmInstallLegacy() throws IOException {
    ShellResponse shellResponse = new ShellResponse();
    shellResponse.message =
        "{\"items\": [{\"metadata\": {\"name\": \"test\"}, \"spec\": {\"clusterIP\": \"None\"," +
        "\"type\":\"clusterIP\"}}]}";
    when(kubernetesManager.getServices(any(), any())).thenReturn(shellResponse);
    defaultUniverse.setConfig(ImmutableMap.of(Universe.HELM2_LEGACY,
                                              Universe.HelmLegacy.V2TO3.toString()));
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    KubernetesCommandExecutor kubernetesCommandExecutor =
        createExecutor(KubernetesCommandExecutor.CommandType.HELM_INSTALL,
                       defaultUniverse.getUniverseDetails().getPrimaryCluster().placementInfo);
    kubernetesCommandExecutor.run();
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);

    ArgumentCaptor<UUID> expectedProviderUUID = ArgumentCaptor.forClass(UUID.class);
    ArgumentCaptor<String> expectedNodePrefix = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<String> expectedOverrideFile = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<HashMap> expectedConfig = ArgumentCaptor.forClass(HashMap.class);
    verify(kubernetesManager, times(1)).helmInstall(expectedConfig.capture(),
                                                    expectedProviderUUID.capture(),
                                                    expectedNodePrefix.capture(),
                                                    expectedOverrideFile.capture());
    verify(kubernetesManager, times(1)).getServices(expectedConfig.capture(),
                                                    expectedNodePrefix.capture());
    assertEquals(config, expectedConfig.getValue());
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    assertEquals(defaultProvider.uuid, expectedProviderUUID.getValue());
    assertEquals(defaultUniverse.getUniverseDetails().nodePrefix, expectedNodePrefix.getValue());
    String overrideFileRegex = "(.*)" + defaultUniverse.universeUUID + "(.*).yml";
    assertThat(expectedOverrideFile.getValue(), RegexMatcher.matchesRegex(overrideFileRegex));
    Yaml yaml = new Yaml();
    InputStream is = new FileInputStream(new File(expectedOverrideFile.getValue()));
    Map<String, Object> overrides = yaml.loadAs(is, Map.class);

    // TODO implement exposeAll false case
    assertEquals(hackPlacementUUID, defaultUniverse.getUniverseDetails().getPrimaryCluster().uuid);
    assertEquals(getExpectedOverrides(true), overrides);
  }
}
