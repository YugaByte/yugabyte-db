package com.yugabyte.yw.models.helpers;

import java.lang.reflect.Field;
import java.util.Arrays;
import java.util.List;
import java.util.stream.Collectors;


/**
 * These are the various types of user tasks and internal tasks.
 */
public enum TaskType {

  // Tasks that are CustomerTasks
  CloudBootstrap("CloudBootstrap"),

  CloudCleanup("CloudCleanup"),

  CreateCassandraTable("CreateCassandraTable"),

  CreateUniverse("CreateUniverse"),

  ReadOnlyClusterCreate("ReadOnlyClusterCreate"),

  ReadOnlyClusterDelete("ReadOnlyClusterDelete"),

  CreateKubernetesUniverse("CreateKubernetesUniverse"),

  DestroyUniverse("DestroyUniverse"),

  DestroyKubernetesUniverse("DestroyKubernetesUniverse"),

  DeleteTable("DeleteTable"),

  BackupUniverse("BackupUniverse"),

  DeleteBackup("DeleteBackup"),

  MultiTableBackup("MultiTableBackup"),

  EditUniverse("EditUniverse"),

  EditKubernetesUniverse("EditKubernetesUniverse"),

  @Deprecated
  KubernetesProvision("KubernetesProvision"),

  ImportIntoTable("ImportIntoTable"),

  UpgradeUniverse("UpgradeUniverse"),

  UpgradeKubernetesUniverse("UpgradeKubernetesUniverse"),

  DeleteNodeFromUniverse("DeleteNodeFromUniverse"),

  StopNodeInUniverse("StopNodeInUniverse"),

  StartNodeInUniverse("StartNodeInUniverse"),

  AddNodeToUniverse("AddNodeToUniverse"),

  RemoveNodeFromUniverse("RemoveNodeFromUniverse"),

  ReleaseInstanceFromUniverse("ReleaseInstanceFromUniverse"),

  SetUniverseKey("SetUniverseKey"),

  @Deprecated
  SetKubernetesUniverseKey("SetKubernetesUniverseKey"),

  CreateKMSConfig("CreateKMSConfig"),

  DeleteKMSConfig("DeleteKMSConfig"),

  UpdateDiskSize("UpdateDiskSize"),

  StartMasterOnNode("StartMasterOnNode"),

  // Tasks belonging to subtasks classpath
  AnsibleClusterServerCtl("subtasks.AnsibleClusterServerCtl"),

  AnsibleConfigureServers("subtasks.AnsibleConfigureServers"),

  AnsibleDestroyServer("subtasks.AnsibleDestroyServer"),

  AnsibleSetupServer("subtasks.AnsibleSetupServer"),

  PrecheckNode("subtasks.PrecheckNode"),

  AnsibleUpdateNodeInfo("subtasks.AnsibleUpdateNodeInfo"),

  BulkImport("subtasks.BulkImport"),

  ChangeMasterConfig("subtasks.ChangeMasterConfig"),

  CreateTable("subtasks.CreateTable"),

  DeleteNode("subtasks.DeleteNode"),

  UpdateNodeProcess("subtasks.nodes.UpdateNodeProcess"),

  DeleteTableFromUniverse("subtasks.DeleteTableFromUniverse"),

  LoadBalancerStateChange("subtasks.LoadBalancerStateChange"),

  ModifyBlackList("subtasks.ModifyBlackList"),

  ManipulateDnsRecordTask("subtasks.ManipulateDnsRecordTask"),

  RemoveUniverseEntry("subtasks.RemoveUniverseEntry"),

  SetFlagInMemory("subtasks.SetFlagInMemory"),

  SetNodeState("subtasks.SetNodeState"),

  SwamperTargetsFileUpdate("subtasks.SwamperTargetsFileUpdate"),

  UniverseUpdateSucceeded("subtasks.UniverseUpdateSucceeded"),

  UpdateAndPersistGFlags("subtasks.UpdateAndPersistGFlags"),

  UpdatePlacementInfo("subtasks.UpdatePlacementInfo"),

  UpdateSoftwareVersion("subtasks.UpdateSoftwareVersion"),

  WaitForDataMove("subtasks.WaitForDataMove"),

  WaitForLoadBalance("subtasks.WaitForLoadBalance"),

  WaitForMasterLeader("subtasks.WaitForMasterLeader"),

  WaitForServer("subtasks.WaitForServer"),

  WaitForTServerHeartBeats("subtasks.WaitForTServerHeartBeats"),

  DeleteClusterFromUniverse("subtasks.DeleteClusterFromUniverse"),

  InstanceActions("subtasks.InstanceActions"),

  WaitForServerReady("subtasks.WaitForServerReady"),

  // Tasks belonging to subtasks.cloud classpath
  CloudAccessKeyCleanup("subtasks.cloud.CloudAccessKeyCleanup"),

  CloudAccessKeySetup("subtasks.cloud.CloudAccessKeySetup"),

  CloudInitializer("subtasks.cloud.CloudInitializer"),

  CloudProviderCleanup("subtasks.cloud.CloudProviderCleanup"),

  CloudRegionCleanup("subtasks.cloud.CloudRegionCleanup"),

  CloudRegionSetup("subtasks.cloud.CloudRegionSetup"),

  CloudSetup("subtasks.cloud.CloudSetup"),

  BackupTable("subtasks.BackupTable"),

  BackupUniverseKeys("subtasks.BackupUniverseKeys"),

  RestoreUniverseKeys("subtasks.RestoreUniverseKeys"),

  WaitForLeadersOnPreferredOnly("subtasks.WaitForLeadersOnPreferredOnly"),

  EnableEncryptionAtRest("subtasks.EnableEncryptionAtRest"),

  DisableEncryptionAtRest("subtasks.DisableEncryptionAtRest"),

  DestroyEncryptionAtRest("subtasks.DestroyEncryptionAtRest"),

  KubernetesCommandExecutor("subtasks.KubernetesCommandExecutor"),

  KubernetesWaitForPod("subtasks.KubernetesWaitForPod"),

  KubernetesCheckNumPod("subtasks.KubernetesCheckNumPod"),

  @Deprecated
  CopyEncryptionKeyFile("subtasks.CopyEncryptionKeyFile"),

  WaitForEncryptionKeyInMemory("subtasks.WaitForEncryptionKeyInMemory"),

  UnivSetCertificate("subtasks.UnivSetCertificate");

  private String relativeClassPath;

  TaskType(String relativeClassPath) {
    this.relativeClassPath = relativeClassPath;
  }

  @Override
  public String toString() {
    return this.relativeClassPath;
  }

  public static List<TaskType> filteredValues() {
    return Arrays.stream(TaskType.values()).filter(value -> {
      try {
        Field field = TaskType.class.getField(value.name());
        return !field.isAnnotationPresent(Deprecated.class);
      } catch (Exception e) {
        return false;
      }
    }).collect(Collectors.toList());
  }
}
