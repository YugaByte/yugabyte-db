// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.BulkImportParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.runners.MockitoJUnitRunner;

import java.util.LinkedList;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.HashMap;
import java.util.UUID;

import play.libs.Json;

import static com.yugabyte.yw.common.TableManager.CommandSubType.BACKUP;
import static com.yugabyte.yw.common.TableManager.CommandSubType.BULK_IMPORT;
import static com.yugabyte.yw.common.TableManager.PY_WRAPPER;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.junit.Assert.assertEquals;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
  public class TableManagerTest extends FakeDBApplication {

  private static final String K8S_CERT_PATH = "/opt/certs/yugabyte/";
  private static final String VM_CERT_PATH = "/home/yugabyte/yugabyte-tls-config/";

  @Mock
  play.Configuration mockAppConfig;

  @Mock
  ShellProcessHandler shellProcessHandler;

  @Mock
  ReleaseManager releaseManager;

  @InjectMocks
  TableManager tableManager;

  private Provider testProvider;
  private Customer testCustomer;
  private Universe testUniverse;
  private String keyCode = "demo-access";
  private String pkPath = "/path/to/private.key";

  private List<UUID> getMockRegionUUIDs(int numRegions) {
    List<UUID> regionUUIDs = new LinkedList<>();
    for (int i = 0; i < numRegions; ++i) {
      String regionCode = "region-" + Integer.toString(i);
      String regionName = "Foo Region " + Integer.toString(i);
      String azCode = "PlacementAZ-" + Integer.toString(i);
      String azName = "PlacementAZ " + Integer.toString(i);
      String subnetName = "Subnet - " + Integer.toString(i);
      Region r = Region.create(testProvider, regionCode, regionName, "default-image");
      AvailabilityZone.create(r, azCode, azName, subnetName);
      regionUUIDs.add(r.uuid);
    }
    return regionUUIDs;
  }

  private void setupUniverse(Provider p) {
    setupUniverse(p, "0.0.1");
  }

  private void setupUniverse(Provider p, String softwareVersion) {
    setupUniverse(p, softwareVersion, false);
  }

  private void setupUniverse(Provider p, String softwareVersion, boolean enableTLS) {
    testProvider = p;
    AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
    keyInfo.privateKey = pkPath;
    keyInfo.sshPort = 3333;
    if (AccessKey.get(testProvider.uuid, keyCode) == null) {
      AccessKey.create(testProvider.uuid, keyCode, keyInfo);
    }

    UniverseDefinitionTaskParams uniParams = new UniverseDefinitionTaskParams();
    uniParams.nodePrefix = "yb-1-" + testUniverse.name;
    UserIntent userIntent = new UniverseDefinitionTaskParams.UserIntent();
    userIntent.accessKeyCode = keyCode;
    userIntent.ybSoftwareVersion = softwareVersion;
    userIntent.numNodes = 3;
    userIntent.replicationFactor = 3;
    userIntent.regionList = getMockRegionUUIDs(3);
    if (enableTLS) {
      userIntent.enableNodeToNodeEncrypt = true;
    }
    uniParams.upsertPrimaryCluster(userIntent, null);
    testUniverse.setUniverseDetails(uniParams);
    testUniverse = Universe.saveDetails(testUniverse.universeUUID,
        ApiUtils.mockUniverseUpdater(userIntent, uniParams.nodePrefix));
  }

  private BulkImportParams getBulkImportParams() {
    BulkImportParams bulkImportParams = new BulkImportParams();
    bulkImportParams.tableName = "mock_table";
    bulkImportParams.keyspace = "mock_ks";
    bulkImportParams.s3Bucket = "s3://foo.bar.com/bulkload";
    bulkImportParams.universeUUID = testUniverse.universeUUID;
    return bulkImportParams;
  }

  private BackupTableParams getBackupTableParams(BackupTableParams.ActionType actionType) {
    BackupTableParams backupTableParams = new BackupTableParams();
    if (actionType.equals(BackupTableParams.ActionType.CREATE)) {
      backupTableParams.tableUUID = UUID.randomUUID();
    }
    backupTableParams.tableName = "mock_table";
    backupTableParams.keyspace = "mock_ks";
    backupTableParams.actionType = actionType;
    backupTableParams.universeUUID = testUniverse.universeUUID;
    return backupTableParams;
  }

  private BackupTableParams getBackupUniverseParams(BackupTableParams.ActionType actionType, UUID storageUUID) {
    BackupTableParams backupTableParams = new BackupTableParams();
    if (actionType.equals(BackupTableParams.ActionType.CREATE)) {
      backupTableParams.tableUUID = UUID.randomUUID();
    }
    backupTableParams.actionType = actionType;
    backupTableParams.storageConfigUUID = storageUUID;
    backupTableParams.universeUUID = testUniverse.universeUUID;
    List<BackupTableParams> backupList = new ArrayList<>();
    BackupTableParams b1Params = new BackupTableParams();
    b1Params.tableName = "mock_table";
    b1Params.keyspace = "mock_ks";
    b1Params.actionType = actionType;
    b1Params.universeUUID = testUniverse.universeUUID;
    b1Params.storageConfigUUID = storageUUID;
    backupList.add(b1Params);
    BackupTableParams b2Params = new BackupTableParams();
    b2Params.keyspace = "mock_ysql";
    b2Params.actionType = actionType;
    b2Params.universeUUID = testUniverse.universeUUID;
    b2Params.storageConfigUUID = storageUUID;
    backupList.add(b2Params);
    backupTableParams.backupList = backupList;
    return backupTableParams;
  }

  private List<String> getExpectedBulkImportCommmand(BulkImportParams bulkImportParams) {
    List<String> cmd = new LinkedList<>();
    // bin/py_wrapper bin/yb_bulk_load.py, --key_path, /path/to/private.key, --instance_count, 24,
    // --universe, Universe-1, --release, /yb/release.tar.gz,
    // --masters, host-n1:7100,host-n2:7100,host-n3:7100, --table, mock_table, --keyspace, mock_ks,
    // --s3bucket, s3://foo.bar.com/bulkload
    cmd.add(PY_WRAPPER);
    cmd.add(BULK_IMPORT.getScript());
    cmd.add("--masters");
    cmd.add(testUniverse.getMasterAddresses());
    cmd.add("--table");
    cmd.add(bulkImportParams.tableName);
    cmd.add("--keyspace");
    cmd.add(bulkImportParams.keyspace);
    cmd.add("--key_path");
    cmd.add(pkPath);
    cmd.add("--instance_count");
    if (bulkImportParams.instanceCount == 0) {
      cmd.add(Integer.toString(testUniverse.getUniverseDetails().getPrimaryCluster().userIntent.numNodes * 8));
    } else {
      cmd.add(Integer.toString(bulkImportParams.instanceCount));
    }
    cmd.add("--universe");
    cmd.add("yb-1-" + testUniverse.name);
    cmd.add("--release");
    cmd.add("/yb/release.tar.gz");
    cmd.add("--s3bucket");
    cmd.add(bulkImportParams.s3Bucket);
    return cmd;
  }

  private List<String> getExpectedBackupTableCommand(
      BackupTableParams backupTableParams, String storageType) {
    AccessKey accessKey = AccessKey.get(testProvider.uuid, keyCode);
    Map<String, String> namespaceToConfig = new HashMap<>();
    UserIntent userIntent = testUniverse.getUniverseDetails().getPrimaryCluster().userIntent;

    if (testProvider.code.equals("kubernetes")) {
      PlacementInfo pi = testUniverse.getUniverseDetails().getPrimaryCluster().placementInfo;
      namespaceToConfig = PlacementInfoUtil.getConfigPerNamespace(pi,
          testUniverse.getUniverseDetails().nodePrefix, testProvider);
    }

    List<String> cmd = new LinkedList<>();
    cmd.add(PY_WRAPPER);
    cmd.add(BACKUP.getScript());
    cmd.add("--masters");
    // TODO(bogdan): we do not have nodes to test this?

    cmd.add(testUniverse.getMasterAddresses());

    cmd.add("--parallelism");
    cmd.add("8");

    if (backupTableParams.tableNameList != null) {
      for (String tableName : backupTableParams.tableNameList) {
        cmd.add("--table");
        cmd.add(tableName);
        cmd.add("--keyspace");
        cmd.add(backupTableParams.keyspace);
      }
    } else {
      if (backupTableParams.tableName != null) {
        cmd.add("--table");
        cmd.add(backupTableParams.tableName);
      }
      cmd.add("--keyspace");
      cmd.add(backupTableParams.keyspace);
    }
    if (testProvider.code.equals("kubernetes")) {
      cmd.add("--k8s_config");
      cmd.add(Json.stringify(Json.toJson(namespaceToConfig)));
    } else {
      if (accessKey.getKeyInfo().sshUser != null) {
        cmd.add("--ssh_user");
        cmd.add(accessKey.getKeyInfo().sshUser);
      }
      cmd.add("--ssh_port");
      cmd.add(accessKey.getKeyInfo().sshPort.toString());
      cmd.add("--ssh_key_path");
      cmd.add(pkPath);
    }
    cmd.add("--backup_location");
    cmd.add(backupTableParams.storageLocation);
    cmd.add("--storage_type");
    cmd.add(storageType);
    if (backupTableParams.actionType.equals(BackupTableParams.ActionType.CREATE) &&
        backupTableParams.tableUUID != null) {
      cmd.add("--table_uuid");
      cmd.add(backupTableParams.tableUUID.toString().replace("-", ""));
    }
    cmd.add("--no_auto_name");
    if (userIntent.enableNodeToNodeEncrypt) {
      cmd.add("--certs_dir");
      cmd.add(testProvider.code.equals("kubernetes") ? K8S_CERT_PATH : VM_CERT_PATH);
    }
    cmd.add(backupTableParams.actionType.name().toLowerCase());
    if (backupTableParams.enableVerboseLogs) {
      cmd.add("--verbose");
    }
    if (backupTableParams.sse) {
      cmd.add("--sse");
    }
    return cmd;
  }

  @Before
  public void setUp() {
    testCustomer = ModelFactory.testCustomer();
    testUniverse = createUniverse("Universe-1", testCustomer.getCustomerId());
    testCustomer.addUniverseUUID(testUniverse.universeUUID);
    testCustomer.save();
    ReleaseManager.ReleaseMetadata metadata = new ReleaseManager.ReleaseMetadata();
    metadata.filePath = "/yb/release.tar.gz";
    when(releaseManager.getReleaseByVersion("0.0.1")).thenReturn(metadata);
  }

  @Test
  public void testBulkImportWithDefaultInstanceCount() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    BulkImportParams bulkImportParams = getBulkImportParams();
    UserIntent userIntent = testUniverse.getUniverseDetails().getPrimaryCluster().userIntent;
    List<String> expectedCommand = getExpectedBulkImportCommmand(bulkImportParams);
    Map<String, String> expectedEnvVars = testProvider.getConfig();
    expectedEnvVars.put("AWS_DEFAULT_REGION", Region.get(userIntent.regionList.get(0)).code);

    tableManager.bulkImport(bulkImportParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testBulkImportWithSpecificInstanceCount() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    BulkImportParams bulkImportParams = getBulkImportParams();
    bulkImportParams.instanceCount = 5;
    UserIntent userIntent = testUniverse.getUniverseDetails().getPrimaryCluster().userIntent;
    List<String> expectedCommand = getExpectedBulkImportCommmand(bulkImportParams);
    Map<String, String> expectedEnvVars = testProvider.getConfig();
    expectedEnvVars.put("AWS_DEFAULT_REGION", Region.get(userIntent.regionList.get(0)).code);

    tableManager.bulkImport(bulkImportParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  private void testCreateS3BackupHelper(boolean enableVerbose, boolean sse) {
    CustomerConfig storageConfig = ModelFactory.createS3StorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupTableParams(BackupTableParams.ActionType.CREATE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;
    backupTableParams.sse = sse;
    if (enableVerbose) {
      backupTableParams.enableVerboseLogs = true;
    }
    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "s3");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  private void testCreateBackupKubernetesHelper() {
    Map<String, String> config = new HashMap<>();
    config.put("KUBECONFIG", "foo");
    testProvider.setConfig(config);
    CustomerConfig storageConfig = ModelFactory.createS3StorageConfig(testCustomer);
    BackupTableParams backupTableParams = getBackupTableParams(BackupTableParams.ActionType.CREATE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;

    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "s3");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    expectedEnvVars.put("KUBECONFIG", "foo");
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testCreateS3Backup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    testCreateS3BackupHelper(false, false);
  }

  @Test
  public void testCreateS3BackupWithSSE() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    testCreateS3BackupHelper(false, true);
  }

  @Test
  public void testCreateS3BackupVerbose() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    testCreateS3BackupHelper(true, false);
  }

  @Test
  public void testCreateNfsBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createNfsStorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupTableParams(BackupTableParams.ActionType.CREATE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;
    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "nfs");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testCreateGcsBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createGcsStorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupTableParams(BackupTableParams.ActionType.CREATE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;
    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "gcs");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testCreateUniverseBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createNfsStorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupUniverseParams(BackupTableParams.ActionType.CREATE,
                                                                  storageConfig.configUUID);
    Backup.create(testCustomer.uuid, backupTableParams);
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    for (BackupTableParams params : backupTableParams.backupList) {
      tableManager.createBackup(params);
      List<String> expectedCommand = getExpectedBackupTableCommand(params, "nfs");
      verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
    }
  }

  @Test
  public void testRestoreS3Backup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createS3StorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupTableParams(BackupTableParams.ActionType.RESTORE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;
    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "s3");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testRestoreNfsBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createNfsStorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupTableParams(
      BackupTableParams.ActionType.RESTORE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;
    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "nfs");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testRestoreGcsBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createGcsStorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupTableParams(
      BackupTableParams.ActionType.RESTORE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;
    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "gcs");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testRestoreUniverseBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    CustomerConfig storageConfig = ModelFactory.createNfsStorageConfig(testCustomer);;
    BackupTableParams backupTableParams = getBackupUniverseParams(BackupTableParams.ActionType.RESTORE,
      storageConfig.configUUID);
    Backup.create(testCustomer.uuid, backupTableParams);
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    for (BackupTableParams params : backupTableParams.backupList) {
      tableManager.createBackup(params);
      List<String> expectedCommand = getExpectedBackupTableCommand(params, "nfs");
      verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
    }
  }

  @Test
  public void testCreateBackupWithSSHUser() {
    setupUniverse(ModelFactory.awsProvider(testCustomer));
    AccessKey accessKey = AccessKey.get(testProvider.uuid, keyCode);
    AccessKey.KeyInfo keyInfo = accessKey.getKeyInfo();
    keyInfo.sshUser = "foo";
    accessKey.setKeyInfo(keyInfo);
    accessKey.save();

    CustomerConfig storageConfig = ModelFactory.createS3StorageConfig(testCustomer);
    BackupTableParams backupTableParams = getBackupTableParams(BackupTableParams.ActionType.CREATE);
    backupTableParams.storageConfigUUID = storageConfig.configUUID;

    Backup.create(testCustomer.uuid, backupTableParams);
    List<String> expectedCommand = getExpectedBackupTableCommand(backupTableParams, "s3");
    Map<String, String> expectedEnvVars = storageConfig.dataAsMap();
    tableManager.createBackup(backupTableParams);
    verify(shellProcessHandler, times(1)).run(expectedCommand, expectedEnvVars);
  }

  @Test
  public void testBulkImportWithIncorrectYBVersion() {
    setupUniverse(ModelFactory.awsProvider(testCustomer), "0.0.2");
    BulkImportParams bulkImportParams = getBulkImportParams();
    try {
      tableManager.bulkImport(bulkImportParams);
    } catch (RuntimeException re) {
      assertEquals("Unable to fetch yugabyte release for version: 0.0.2", re.getMessage());
    }
  }

  @Test
  public void testCreateTLSBackup() {
    setupUniverse(ModelFactory.awsProvider(testCustomer), "0.0.1", true);
    testCreateS3BackupHelper(false, false);
  }

  @Test
  public void testCreateBackupKubernetes() {
    setupUniverse(ModelFactory.kubernetesProvider(testCustomer));
    testCreateBackupKubernetesHelper();
  }

  @Test
  public void testCreateBackupKubernetesWithTLS() {
    setupUniverse(ModelFactory.kubernetesProvider(testCustomer), "0.0.1", true);
    testCreateBackupKubernetesHelper();
  }
}
