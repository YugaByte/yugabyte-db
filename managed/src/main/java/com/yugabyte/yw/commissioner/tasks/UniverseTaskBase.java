// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.fasterxml.jackson.databind.JsonNode;

import java.time.Duration;
import java.util.*;
import java.util.Map.Entry;

import com.yugabyte.yw.commissioner.tasks.subtasks.*;
import com.yugabyte.yw.common.*;
import com.yugabyte.yw.forms.*;
import com.yugabyte.yw.models.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.typesafe.config.Config;
import org.apache.commons.lang3.StringUtils;
import org.yb.Common;
import org.yb.client.YBClient;
import org.yb.client.ModifyClusterConfigIncrementVersion;
import com.google.common.net.HostAndPort;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.AbstractTaskBase;
import com.yugabyte.yw.commissioner.SubTaskGroup;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.commissioner.tasks.subtasks.nodes.UpdateNodeProcess;

import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.forms.UniverseTaskParams.EncryptionAtRestConfig.OpType;
import com.yugabyte.yw.models.Universe.UniverseUpdater;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.TableDetails;

import play.api.Play;
import play.libs.Json;

public abstract class UniverseTaskBase extends AbstractTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(UniverseTaskBase.class);

  // Flag to indicate if we have locked the universe.
  private boolean universeLocked = false;

  protected Config config;

  // The task params.
  @Override
  protected UniverseTaskParams taskParams() {
    return (UniverseTaskParams)taskParams;
  }

  private UniverseUpdater getLockingUniverseUpdater(int expectedUniverseVersion,
                                                    boolean checkSuccess) {
    return getLockingUniverseUpdater(expectedUniverseVersion, checkSuccess, false);
  }

  private UniverseUpdater getLockingUniverseUpdater(int expectedUniverseVersion,
                                                    boolean checkSuccess,
                                                    boolean isForceUpdate) {
    return new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        verifyUniverseVersion(expectedUniverseVersion, universe);
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        // If this universe is already being edited, fail the request.
        if (!isForceUpdate && universeDetails.updateInProgress) {
          String msg = "UserUniverse " + taskParams().universeUUID + " is already being updated.";
          LOG.error(msg);
          throw new RuntimeException(msg);
        }
        markUniverseUpdateInProgress(universe, checkSuccess);
      }
    };
  }

  public void markUniverseUpdateInProgress(Universe universe, boolean checkSuccess) {
    UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
    // Persist the updated information about the universe. Mark it as being edited.
    universeDetails.updateInProgress = true;
    if (checkSuccess) {
      universeDetails.updateSucceeded = false;
    }
    universe.setUniverseDetails(universeDetails);
  }

  public void verifyUniverseVersion(int expectedUniverseVersion, Universe universe) {
    if (expectedUniverseVersion != -1 && expectedUniverseVersion != universe.version) {
      String msg = "Universe " + taskParams().universeUUID + " version " + universe.version +
              ", is different from the expected version of " + expectedUniverseVersion + ". User " +
              "would have to sumbit the operation from a refreshed top-level universe page.";
      LOG.error(msg);
      throw new IllegalStateException(msg);
    }
  }

  private Universe lockUniverseForUpdate(int expectedUniverseVersion, UniverseUpdater updater) {
    // Perform the update. If unsuccessful, this will throw a runtime exception which we do not
    // catch as we want to fail.
    Universe universe = saveUniverseDetails(updater);
    universeLocked = true;
    LOG.trace("Locked universe {} at version {}.", taskParams().universeUUID,
      expectedUniverseVersion);
    // Return the universe object that we have already updated.
    return universe;
  }

  public SubTaskGroup createManageEncryptionAtRestTask() {
    SubTaskGroup subTaskGroup = null;
    AbstractTaskBase task = null;
    UniverseDefinitionTaskParams params = null;
    switch (taskParams().encryptionAtRestConfig.opType) {
      case ENABLE:
        subTaskGroup = new SubTaskGroup("EnableEncryptionAtRest", executor);
        task = new EnableEncryptionAtRest();
        EnableEncryptionAtRest.Params enableParams = new EnableEncryptionAtRest.Params();
        enableParams.universeUUID = taskParams().universeUUID;
        enableParams.encryptionAtRestConfig = taskParams().encryptionAtRestConfig;
        task.initialize(enableParams);
        subTaskGroup.addTask(task);
        subTaskGroupQueue.add(subTaskGroup);
        break;
      case DISABLE:
        subTaskGroup = new SubTaskGroup("DisableEncryptionAtRest", executor);
        task = new DisableEncryptionAtRest();
        DisableEncryptionAtRest.Params disableParams = new DisableEncryptionAtRest.Params();
        disableParams.universeUUID = taskParams().universeUUID;
        task.initialize(disableParams);
        subTaskGroup.addTask(task);
        subTaskGroupQueue.add(subTaskGroup);
        break;
      default:
      case UNDEFINED:
        break;
    }

    UniverseUpdater updater = new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        LOG.info(String.format(
                "Setting encryption at rest status to %s for universe %s",
                taskParams().encryptionAtRestConfig.opType.name(),
                universe.universeUUID.toString()
        ));
        // Persist the updated information about the universe.
        // It should have been marked as being edited in lockUniverseForUpdate().
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        if (!universeDetails.updateInProgress) {
          String msg = "Universe " + taskParams().universeUUID +
                  " has not been marked as being updated.";
          LOG.error(msg);
          throw new RuntimeException(msg);
        }

        universeDetails.encryptionAtRestConfig = taskParams().encryptionAtRestConfig;

        universeDetails.encryptionAtRestConfig.encryptionAtRestEnabled =
                taskParams().encryptionAtRestConfig.opType.equals(OpType.ENABLE);
        universe.setUniverseDetails(universeDetails);
      }
    };
    // Perform the update. If unsuccessful, this will throw a runtime exception which we do not
    // catch as we want to fail.
    saveUniverseDetails(updater);
    LOG.trace("Wrote user intent for universe {}.", taskParams().universeUUID);

    return subTaskGroup;
  }

  public SubTaskGroup createDestroyEncryptionAtRestTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("DestroyEncryptionAtRest", executor);
    DestroyEncryptionAtRest task = new DestroyEncryptionAtRest();
    DestroyEncryptionAtRest.Params params = new DestroyEncryptionAtRest.Params();
    params.universeUUID = taskParams().universeUUID;
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  @Override
  public void initialize(ITaskParams params) {
    super.initialize(params);
    // Create the threadpool for the subtasks to use.
    createThreadpool();
    this.config = Play.current().injector().instanceOf(Config.class);
  }

  @Override
  public String getName() {
    return super.getName() + "(" + taskParams().universeUUID + ")";
  }

  /**
   * Locks the universe for updates by setting the 'updateInProgress' flag. If the universe is
   * already being modified, then throws an exception.
   *
   * @param expectedUniverseVersion Lock only if the current version of the universe is at this
   *                                version. -1 implies always lock the universe.
   */
  public Universe lockUniverseForUpdate(int expectedUniverseVersion) {
    UniverseUpdater updater = getLockingUniverseUpdater(expectedUniverseVersion, true);
    return lockUniverseForUpdate(expectedUniverseVersion, updater);
  }

  public Universe forceLockUniverseForUpdate(int expectedUniverseVersion) {
    LOG.info("Force lock universe {} at version {}.", taskParams().universeUUID,
             expectedUniverseVersion);
    UniverseUpdater updater = getLockingUniverseUpdater(expectedUniverseVersion, true, true);
    return lockUniverseForUpdate(expectedUniverseVersion, updater);
  }

  /**
   * Locks the universe by setting the 'updateInProgress' flag. If the universe is already being
   * modified, then throws an exception. Any tasks involving tables should use this method, not any
   * other.
   *
   * @param expectedUniverseVersion Lock only if the current version of the unvierse is at this
   *                                version. -1 implies always lock the universe.
   */
  public Universe lockUniverse(int expectedUniverseVersion) {
    UniverseUpdater updater = getLockingUniverseUpdater(expectedUniverseVersion, false);
    return lockUniverseForUpdate(expectedUniverseVersion, updater);
  }

  public void unlockUniverseForUpdate() {
    unlockUniverseForUpdate(null);
  }

  public void unlockUniverseForUpdate(String error) {
    if (!universeLocked) {
      LOG.warn("Unlock universe called when it was not locked.");
      return;
    }
    final String err = error;
    // Create the update lambda.
    UniverseUpdater updater = new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        // If this universe is not being edited, fail the request.
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        if (!universeDetails.updateInProgress) {
          String msg = "UserUniverse " + taskParams().universeUUID + " is not being edited.";
          LOG.error(msg);
          throw new RuntimeException(msg);
        }
        // Persist the updated information about the universe. Mark it as being edited.
        universeDetails.updateInProgress = false;
        universeDetails.errorString = err;
        universe.setUniverseDetails(universeDetails);
      }
    };
    // Perform the update. If unsuccessful, this will throw a runtime exception which we do not
    // catch as we want to fail.
    saveUniverseDetails(updater);
    LOG.trace("Unlocked universe {} for updates.", taskParams().universeUUID);
  }

  /**
   * Create a task to mark the change on a universe as success.
   */
  public SubTaskGroup createMarkUniverseUpdateSuccessTasks() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("FinalizeUniverseUpdate", executor);
    UniverseUpdateSucceeded.Params params = new UniverseUpdateSucceeded.Params();
    params.universeUUID = taskParams().universeUUID;
    UniverseUpdateSucceeded task = new UniverseUpdateSucceeded();
    task.initialize(params);
    task.setUserTaskUUID(userTaskUUID);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create a task to mark the final software version on a universe.
   */
  public SubTaskGroup createUpdateSoftwareVersionTask(String softwareVersion) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("FinalizeUniverseUpdate", executor);
    UpdateSoftwareVersion.Params params = new UpdateSoftwareVersion.Params();
    params.universeUUID = taskParams().universeUUID;
    params.softwareVersion = softwareVersion;
    UpdateSoftwareVersion task = new UpdateSoftwareVersion();
    task.initialize(params);
    task.setUserTaskUUID(userTaskUUID);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create a task to mark the updated cert on a universe.
   */
  public SubTaskGroup createUnivSetCertTask(UUID certUUID) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("FinalizeUniverseUpdate", executor);
    UnivSetCertificate.Params params = new UnivSetCertificate.Params();
    params.universeUUID = taskParams().universeUUID;
    params.certUUID = certUUID;
    UnivSetCertificate task = new UnivSetCertificate();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task list to destroy nodes and adds it to the task queue.
   *
   * @param nodes : a collection of nodes that need to be removed
   * @param isForceDelete if this is true, ignore ansible errors
   * @param deleteNode if true, the node info is deleted from the universe db.
   */
  public SubTaskGroup createDestroyServerTasks(Collection<NodeDetails> nodes,
                                               boolean isForceDelete,
                                               boolean deleteNode) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("AnsibleDestroyServers", executor);
    for (NodeDetails node : nodes) {
      // Check if the private ip for the node is set. If not, that means we don't have
      // a clean state to delete the node. Log it and skip the node.
      if (node.cloudInfo.private_ip == null) {
        LOG.warn(String.format("Node %s doesn't have a private IP. Skipping node delete.",
                               node.nodeName));
        continue;
      }
      AnsibleDestroyServer.Params params = new AnsibleDestroyServer.Params();
      // Set the device information (numVolumes, volumeSize, etc.)
      params.deviceInfo = taskParams().deviceInfo;
      // Set the region name to the proper provider code so we can use it in the cloud API calls.
      params.azUuid = node.azUuid;
      // Add the node name.
      params.nodeName = node.nodeName;
      // Add the universe uuid.
      params.universeUUID = taskParams().universeUUID;
      // Flag to be set where errors during Ansible Destroy Server will be ignored.
      params.isForceDelete = isForceDelete;
      // Flag to track if node info should be deleted from universe db.
      params.deleteNode = deleteNode;
      // Add the instance type
      params.instanceType = node.cloudInfo.instance_type;
      // Assign the node IP to ensure deletion of the correct node.
      params.nodeIP = node.cloudInfo.private_ip;
      // Create the Ansible task to destroy the server.
      AnsibleDestroyServer task = new AnsibleDestroyServer();
      task.initialize(params);
      task.setUserTaskUUID(userTaskUUID);
      // Add it to the task list.
      subTaskGroup.addTask(task);
    }
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create tasks to update the state of the nodes.
   *
   * @param nodes set of nodes to be updated.
   * @param nodeState State into which these nodes will be transitioned.
   * @return
   */
  public SubTaskGroup createSetNodeStateTasks(Collection<NodeDetails> nodes,
                                              NodeDetails.NodeState nodeState) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("SetNodeState", executor);
    for (NodeDetails node : nodes) {
      SetNodeState.Params params = new SetNodeState.Params();
      params.universeUUID = taskParams().universeUUID;
      params.azUuid = node.azUuid;
      params.nodeName = node.nodeName;
      params.state = nodeState;
      SetNodeState task = new SetNodeState();
      task.initialize(params);
      task.setUserTaskUUID(userTaskUUID);
      subTaskGroup.addTask(task);
    }
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createWaitForKeyInMemoryTask(NodeDetails node) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForEncryptionKeyInMemory", executor);
    WaitForEncryptionKeyInMemory.Params params = new WaitForEncryptionKeyInMemory.Params();
    params.universeUUID = taskParams().universeUUID;
    params.nodeAddress = HostAndPort.fromParts(node.cloudInfo.private_ip, node.masterRpcPort);
    params.nodeName = node.nodeName;
    WaitForEncryptionKeyInMemory task = new WaitForEncryptionKeyInMemory();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create task to execute a Cluster CTL command against specific process
   *
   * @param node node for which the CTL command needs to be executed
   * @param processType, Master/TServer process type
   * @param command, actual command (start, stop, create)
   * @return SubTaskGroup
   */
  public SubTaskGroup createServerControlTask(NodeDetails node,
                                              UniverseDefinitionTaskBase.ServerType processType,
                                              String command) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("AnsibleClusterServerCtl", executor);
    subTaskGroup.addTask(getServerControlTask(node, processType, command, 0));
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create task to check if a specific process is ready to serve requests on a given node.
   *
   * @param node node for which the check needs to be executed.
   * @param serverType server process type on the node to the check.
   * @param sleepTimeMs default sleep time if server does not support check for readiness.
   * @return SubTaskGroup
   */
  public SubTaskGroup createWaitForServerReady(NodeDetails node, ServerType serverType,
                                               int sleepTimeMs) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForServerReady", executor);
    WaitForServerReady.Params params = new WaitForServerReady.Params();
    params.universeUUID = taskParams().universeUUID;
    params.nodeName = node.nodeName;
    params.serverType = serverType;
    params.waitTimeMs = sleepTimeMs;
    WaitForServerReady task = new WaitForServerReady();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create tasks to execute Cluster CTL command against specific process in parallel
   *
   * @param nodes set of nodes to issue control command in parallel.
   * @param processType, Master/TServer process type
   * @param command, actual command (start, stop, create)
   * @return SubTaskGroup
   */
  public SubTaskGroup createServerControlTasks(List<NodeDetails> nodes,
                                               UniverseDefinitionTaskBase.ServerType processType,
                                               String command) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("AnsibleClusterServerCtl", executor);
    for (NodeDetails node : nodes) {
      subTaskGroup.addTask(getServerControlTask(node, processType, command, 0));
    }
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  private AnsibleClusterServerCtl getServerControlTask(NodeDetails node,
                                                       UniverseDefinitionTaskBase.ServerType processType,
                                                       String command,
                                                       int sleepAfterCmdMillis) {
    AnsibleClusterServerCtl.Params params = new AnsibleClusterServerCtl.Params();
    // Add the node name.
    params.nodeName = node.nodeName;
    // Add the universe uuid.
    params.universeUUID = taskParams().universeUUID;
    // Add the az uuid.
    params.azUuid = node.azUuid;
    // The service and the command we want to run.
    params.process = processType.toString().toLowerCase();
    params.command = command;
    params.sleepAfterCmdMills = sleepAfterCmdMillis;
    // Set the InstanceType
    params.instanceType = node.cloudInfo.instance_type;
    // Create the Ansible task to get the server info.
    AnsibleClusterServerCtl task = new AnsibleClusterServerCtl();
    task.initialize(params);
    return task;
  }

  /**
   * Create task to update the state of single node.
   *
   * @param node node for which we need to update the state
   * @param nodeState State into which these nodes will be transitioned.
   */
  public SubTaskGroup createSetNodeStateTask(NodeDetails node, NodeDetails.NodeState nodeState) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("SetNodeState", executor);
    SetNodeState.Params params = new SetNodeState.Params();
    params.azUuid = node.azUuid;
    params.universeUUID = taskParams().universeUUID;
    params.nodeName = node.nodeName;
    params.state = nodeState;
    SetNodeState task = new SetNodeState();
    task.initialize(params);
    task.setUserTaskUUID(userTaskUUID);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create a task to update the swamper target file
   *
   * @param removeFile, flag to state if we want to remove the swamper or not
   */
  public void createSwamperTargetUpdateTask(boolean removeFile) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("SwamperTargetFileUpdate", executor);
    SwamperTargetsFileUpdate.Params params = new SwamperTargetsFileUpdate.Params();
    SwamperTargetsFileUpdate task = new SwamperTargetsFileUpdate();
    params.universeUUID = taskParams().universeUUID;
    params.removeFile = removeFile;
    task.initialize(params);
    subTaskGroup.setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
  }

  /**
   * Create a task to create a table.
   *
   * @param tableName name of the table.
   * @param tableDetails table options and related  details.
   */
  public SubTaskGroup createTableTask(Common.TableType tableType, String tableName,
                                      TableDetails tableDetails) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("CreateTable", executor);
    CreateTable task = new CreateTable();
    CreateTable.Params params = new CreateTable.Params();
    params.universeUUID = taskParams().universeUUID;
    params.tableType = tableType;
    params.tableName = tableName;
    params.tableDetails = tableDetails;
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Create a task to delete a table.
   *
   * @param params The necessary parameters for dropping a table.
   */
  public SubTaskGroup createDeleteTableFromUniverseTask(DeleteTableFromUniverse.Params params) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("DeleteTableFromUniverse", executor);
    DeleteTableFromUniverse task = new DeleteTableFromUniverse();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createWaitForServersTasks(Collection<NodeDetails> nodes, ServerType type) {
    return createWaitForServersTasks(
      nodes,
      type,
      config.getDuration("yb.wait_for_server_timeout") /* default timeout */
    );
  }

  /**
   * Create a task list to ping all servers until they are up.
   *
   * @param nodes : a collection of nodes that need to be pinged.
   * @param type  : Master or tserver type server running on these nodes.
   * @param timeout : time to wait for each rpc call to the server.
   */
  public SubTaskGroup createWaitForServersTasks(
    Collection<NodeDetails> nodes,
    ServerType type,
    Duration timeout
  ) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForServer", executor);
    for (NodeDetails node : nodes) {
      WaitForServer.Params params = new WaitForServer.Params();
      params.universeUUID = taskParams().universeUUID;
      params.nodeName = node.nodeName;
      params.serverType = type;
      params.serverWaitTimeoutMs = timeout.toMillis();
      WaitForServer task = new WaitForServer();
      task.initialize(params);
      subTaskGroup.addTask(task);
    }
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task to persist customized gflags to be used by server processes.
   */
  public SubTaskGroup updateGFlagsPersistTasks(Map<String, String> masterGFlags, Map<String, String> tserverGFlags) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("UpdateAndPersistGFlags", executor);
    UpdateAndPersistGFlags.Params params = new UpdateAndPersistGFlags.Params();
    params.universeUUID = taskParams().universeUUID;
    params.masterGFlags = masterGFlags;
    params.tserverGFlags = tserverGFlags;
    UpdateAndPersistGFlags task = new UpdateAndPersistGFlags();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task to bulk import data from an s3 bucket into a given table.
   *
   * @param taskParams Info about the table and universe of the table to import into.
   */
  public SubTaskGroup createBulkImportTask(BulkImportParams taskParams) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("BulkImport", executor);
    BulkImport task = new BulkImport();
    task.initialize(taskParams);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task delete the given node name from the univers.
   *
   * @param nodeName name of a node in the taskparams' uuid universe.
   */
  public SubTaskGroup deleteNodeFromUniverseTask(String nodeName) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("DeleteNode", executor);
    NodeTaskParams params = new NodeTaskParams();
    params.nodeName = nodeName;
    params.universeUUID = taskParams().universeUUID;
    DeleteNode task = new DeleteNode();
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Add or Remove Master process on the node
   * @param node the node to add/remove master process on
   * @param isAdd whether Master is being added or removed.
   * @param subTask subtask type
   * @param useHostPort indicate to server to use host/port instead of uuid.
   */
  public void createChangeConfigTask(NodeDetails node,
                                     boolean isAdd,
                                     UserTaskDetails.SubTaskGroupType subTask) {
    createChangeConfigTask(node, isAdd, subTask, false);
  }

  public void createChangeConfigTask(NodeDetails node,
                                     boolean isAdd,
                                     UserTaskDetails.SubTaskGroupType subTask,
                                     boolean useHostPort) {
    // Create a new task list for the change config so that it happens one by one.
    String subtaskGroupName = "ChangeMasterConfig(" + node.nodeName + ", " +
      (isAdd? "add" : "remove") + ")";
    SubTaskGroup subTaskGroup = new SubTaskGroup(subtaskGroupName, executor);
    // Create the task params.
    ChangeMasterConfig.Params params = new ChangeMasterConfig.Params();
    // Set the azUUID
    params.azUuid = node.azUuid;
    // Add the node name.
    params.nodeName = node.nodeName;
    // Add the universe uuid.
    params.universeUUID = taskParams().universeUUID;
    // This is an add master.
    params.opType = isAdd ? ChangeMasterConfig.OpType.AddMaster :
      ChangeMasterConfig.OpType.RemoveMaster;
    params.useHostPort = useHostPort;
    // Create the task.
    ChangeMasterConfig changeConfig = new ChangeMasterConfig();
    changeConfig.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(changeConfig);
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    // Configure the user facing subtask for this task list.
    subTaskGroup.setSubTaskGroupType(subTask);
  }

  /**
   * Start T-Server process on the given node
   * @param currentNode the node to operate upon
   * @param taskType Command start/stop
   * @return Subtask group
   */
  public SubTaskGroup createTServerTaskForNode(NodeDetails currentNode, String taskType) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("AnsibleClusterServerCtl", executor);
    AnsibleClusterServerCtl.Params params = new AnsibleClusterServerCtl.Params();
    // Add the node name.
    params.nodeName = currentNode.nodeName;
    // Add the universe uuid.
    params.universeUUID = taskParams().universeUUID;
    // Add the az uuid.
    params.azUuid = currentNode.azUuid;
    // The service and the command we want to run.
    params.process = "tserver";
    params.command = taskType;
    // Set the InstanceType
    params.instanceType = currentNode.cloudInfo.instance_type;
    // Create the Ansible task to get the server info.
    AnsibleClusterServerCtl task = new AnsibleClusterServerCtl();
    task.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Wait for Master Leader Election
   * @return subtask group
   */
  public SubTaskGroup createWaitForMasterLeaderTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForMasterLeader", executor);
    WaitForMasterLeader task = new WaitForMasterLeader();
    WaitForMasterLeader.Params params = new WaitForMasterLeader.Params();
    params.universeUUID = taskParams().universeUUID;
    task.initialize(params);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  // Helper function to create a process update object.
  private UpdateNodeProcess getUpdateTaskProcess(String nodeName, ServerType processType,
      Boolean isAdd) {
    // Create the task params.
    UpdateNodeProcess.Params params = new UpdateNodeProcess.Params();
    params.processType = processType;
    params.isAdd = isAdd;
    params.universeUUID = taskParams().universeUUID;
    params.nodeName = nodeName;
    UpdateNodeProcess updateNodeProcess = new UpdateNodeProcess();
    updateNodeProcess.initialize(params);
    return updateNodeProcess;
  }

  /**
   * Update the process state across all the given servers in Yugaware DB.
   * @param servers     : Set of nodes whose process state is to be updated.
   * @param processType : process type: master or tserver.
   * @param isAdd       : true if the process is being added, false otherwise.
   */
  public void createUpdateNodeProcessTasks(Set<NodeDetails> servers,
                                           ServerType processType,
                                           Boolean isAdd) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("UpdateNodeProcess", executor);
    for (NodeDetails server : servers) {
      UpdateNodeProcess updateNodeProcess = getUpdateTaskProcess(
          server.nodeName, processType, isAdd);
      // Add it to the task list.
      subTaskGroup.addTask(updateNodeProcess);
    }
    subTaskGroup.setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);
    subTaskGroupQueue.add(subTaskGroup);
  }

  /**
   * Update the given node's process state in Yugaware DB,
   * @param nodeName    : name of the node where the process state is to be updated.
   * @param processType : process type: master or tserver.
   * @param isAdd       : boolean signifying if the process is being added or removed.
   * @return The subtask group.
   */
  public SubTaskGroup createUpdateNodeProcessTask(String nodeName, ServerType processType,
                                                  Boolean isAdd) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("UpdateNodeProcess", executor);
    UpdateNodeProcess updateNodeProcess = getUpdateTaskProcess(nodeName, processType, isAdd);
    // Add it to the task list.
    subTaskGroup.addTask(updateNodeProcess);
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task list to start the masters and adds it to the task queue.
   * @param nodes   : a collection of nodes that need masters to be spawned.
   * @return The subtask group.
   */
  public SubTaskGroup createStartMasterTasks(Collection<NodeDetails> nodes) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("AnsibleClusterServerCtl", executor);
    for (NodeDetails node : nodes) {
      AnsibleClusterServerCtl.Params params = new AnsibleClusterServerCtl.Params();
      // Add the node name.
      params.nodeName = node.nodeName;
      // Add the universe uuid.
      params.universeUUID = taskParams().universeUUID;
      // Add the az uuid.
      params.azUuid = node.azUuid;
      // The service and the command we want to run.
      params.process = "master";
      params.command = "start";
      params.placementUuid = node.placementUuid;
      // Set the InstanceType
      params.instanceType = node.cloudInfo.instance_type;
      // Create the Ansible task to get the server info.
      AnsibleClusterServerCtl task = new AnsibleClusterServerCtl();
      task.initialize(params);
      // Add it to the task list.
      subTaskGroup.addTask(task);
    }
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task list to stop the masters of the cluster and adds it to the task queue.
   * @param nodes set of nodes to be stopped as master
   * @return
   */
  public SubTaskGroup createStopMasterTasks(Collection<NodeDetails> nodes) {
    return createStopServerTasks(nodes, "master", false);
  }

  /**
   * Creates a task list to stop the tservers of the cluster and adds it to the task queue.
   * @param nodes set of nodes to be stopped as master
   * @return
   */
  public SubTaskGroup createStopServerTasks(Collection<NodeDetails> nodes,
                                            String serverType,
                                            boolean isForceDelete) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("AnsibleClusterServerCtl", executor);
    for (NodeDetails node : nodes) {
      AnsibleClusterServerCtl.Params params = new AnsibleClusterServerCtl.Params();
      // Add the node name.
      params.nodeName = node.nodeName;
      // Add the universe uuid.
      params.universeUUID = taskParams().universeUUID;
      // Add the az uuid.
      params.azUuid = node.azUuid;
      // The service and the command we want to run.
      params.process = serverType;
      params.command = "stop";
      // Set the InstanceType
      params.instanceType = node.cloudInfo.instance_type;
      params.isForceDelete = isForceDelete;
      // Create the Ansible task to get the server info.
      AnsibleClusterServerCtl task = new AnsibleClusterServerCtl();
      task.initialize(params);
      // Add it to the task list.
      subTaskGroup.addTask(task);
    }
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createTableBackupTask(BackupTableParams taskParams, Backup backup) {
    SubTaskGroup subTaskGroup;
    if (backup == null) {
      subTaskGroup = new SubTaskGroup("BackupTable", executor);
    } else {
      subTaskGroup = new SubTaskGroup("BackupTable", executor, true);
    }

    BackupTable task = new BackupTable(backup);
    task.initialize(taskParams);
    task.setUserTaskUUID(userTaskUUID);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createEncryptedUniverseKeyBackupTask() {
    return createEncryptedUniverseKeyBackupTask((BackupTableParams) taskParams());
  }

  public SubTaskGroup createEncryptedUniverseKeyBackupTask(BackupTableParams params) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("BackupUniverseKeys", executor);
    BackupUniverseKeys task = new BackupUniverseKeys();
    task.initialize(params);
    task.setUserTaskUUID(userTaskUUID);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public SubTaskGroup createEncryptedUniverseKeyRestoreTask(BackupTableParams params) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("RestoreUniverseKeys", executor);
    RestoreUniverseKeys task = new RestoreUniverseKeys();
    task.initialize(params);
    task.setUserTaskUUID(userTaskUUID);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task list to manipulate the DNS record available for this universe.
   * @param eventType   the type of manipulation to do on the DNS records.
   * @param isForceDelete if this is a delete operation, set this to true to ignore errors
   * @param providerType provider type, to check that we allow only on AWS.
   * @param provider the provider uuid (stored as a string).
   * @param universeName the universe name used for domain info.
   * @return subtask group
   */
  public SubTaskGroup createDnsManipulationTask(
      DnsManager.DnsCommandType eventType, boolean isForceDelete, CloudType providerType,
      String provider, String universeName) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("UpdateDnsEntry", executor);
    if (!providerType.equals(CloudType.aws)) {
      return subTaskGroup;
    }
    Provider p = Provider.get(UUID.fromString(provider));
    // TODO: shared constant with javascript land?
    String hostedZoneId = p.getConfig().get("AWS_HOSTED_ZONE_ID");
    if (hostedZoneId == null || hostedZoneId.isEmpty()) {
      return subTaskGroup;
    }
    ManipulateDnsRecordTask.Params params = new ManipulateDnsRecordTask.Params();
    params.universeUUID = taskParams().universeUUID;
    params.type = eventType;
    params.providerUUID = UUID.fromString(provider);
    params.hostedZoneId = hostedZoneId;
    params.domainNamePrefix =
        String.format("%s.%s", universeName, Customer.get(p.customerUUID).code);
    params.isForceDelete = isForceDelete;
    // Create the task to update DNS entries.
    ManipulateDnsRecordTask task = new ManipulateDnsRecordTask();
    task.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task list to update the placement information by making a call to the master leader
   * and adds it to the task queue.
   *
   * @param blacklistNodes    list of nodes which are being removed.
   */
  public SubTaskGroup createPlacementInfoTask(Collection<NodeDetails> blacklistNodes) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("UpdatePlacementInfo", executor);
    UpdatePlacementInfo.Params params = new UpdatePlacementInfo.Params();
    // Add the universe uuid.
    params.universeUUID = taskParams().universeUUID;
    // Set the blacklist nodes if any are passed in.
    if (blacklistNodes != null && !blacklistNodes.isEmpty()) {
      Set<String> blacklistNodeNames = new HashSet<String>();
      for (NodeDetails node : blacklistNodes) {
        blacklistNodeNames.add(node.nodeName);
      }
      params.blacklistNodes = blacklistNodeNames;
    }
    // Create the task to update placement info.
    UpdatePlacementInfo task = new UpdatePlacementInfo();
    task.initialize(params);
    task.setUserTaskUUID(userTaskUUID);
    // Add it to the task list.
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task to move the data out of blacklisted servers.
   * @return the created task group.
   */
  public SubTaskGroup createWaitForDataMoveTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForDataMove", executor);
    WaitForDataMove.Params params = new WaitForDataMove.Params();
    params.universeUUID = taskParams().universeUUID;
    // Create the task.
    WaitForDataMove waitForMove = new WaitForDataMove();
    waitForMove.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(waitForMove);
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task to wait for leaders to be on preferred regions only.
   */
  public void createWaitForLeadersOnPreferredOnlyTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForLeadersOnPreferredOnly", executor);
    WaitForLeadersOnPreferredOnly.Params params = new WaitForLeadersOnPreferredOnly.Params();
    params.universeUUID = taskParams().universeUUID;
    // Create the task.
    WaitForLeadersOnPreferredOnly waitForLeadersOnPreferredOnly = new WaitForLeadersOnPreferredOnly();
    waitForLeadersOnPreferredOnly.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(waitForLeadersOnPreferredOnly);
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    // Set the subgroup task type.
    subTaskGroup.setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.WaitForDataMigration);
  }

  /**
   * Creates a task to move the data onto any lesser loaded servers.
   * @return the created task group.
   */
  public SubTaskGroup createWaitForLoadBalanceTask() {
    SubTaskGroup subTaskGroup = new SubTaskGroup("WaitForLoadBalance", executor);
    WaitForLoadBalance.Params params = new WaitForLoadBalance.Params();
    params.universeUUID = taskParams().universeUUID;
    // Create the task.
    WaitForLoadBalance waitForLoadBalance = new WaitForLoadBalance();
    waitForLoadBalance.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(waitForLoadBalance);
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  /**
   * Creates a task to remove a node from blacklist on server.
   * @param nodes The nodes that have to be removed from the blacklist.
   * @param isAdd true if the node are added to server blacklist, else removed.
   * @return the created task group.
   */
  public SubTaskGroup createModifyBlackListTask(List<NodeDetails> nodes, boolean isAdd) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("ModifyBlackList", executor);
    ModifyBlackList.Params params = new ModifyBlackList.Params();
    params.universeUUID = taskParams().universeUUID;
    params.isAdd = isAdd;
    params.nodes = new HashSet<NodeDetails>(nodes);
    // Create the task.
    ModifyBlackList modifyBlackList = new ModifyBlackList();
    modifyBlackList.initialize(params);
    // Add it to the task list.
    subTaskGroup.addTask(modifyBlackList);
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  // Subtask to update gflags in memory.
  public void createSetFlagInMemoryTasks(Collection<NodeDetails> nodes,
                                         ServerType serverType,
                                         boolean force,
                                         Map<String, String> gflags,
                                         boolean updateMasterAddrs) {
    SubTaskGroup subTaskGroup = new SubTaskGroup("InMemoryGFlagUpdate", executor);
    for (NodeDetails node : nodes) {
      // Create the task params.
      SetFlagInMemory.Params params = new SetFlagInMemory.Params();
      // Add the node name.
      params.nodeName = node.nodeName;
      // Add the universe uuid.
      params.universeUUID = taskParams().universeUUID;
      // The server type for the flag.
      params.serverType = serverType;
      // If the flags need to be force updated.
      params.force = force;
      // The flags to update.
      params.gflags = gflags;
      // If only master addresses need to be updated.
      params.updateMasterAddrs = updateMasterAddrs;

      // Create the task.
      SetFlagInMemory setFlag = new SetFlagInMemory();
      setFlag.initialize(params);
      // Add it to the task list.
      subTaskGroup.addTask(setFlag);
    }
    // Add the task list to the task queue.
    subTaskGroupQueue.add(subTaskGroup);
    // Configure the user facing subtask for this task list.
    subTaskGroup.setSubTaskGroupType(SubTaskGroupType.UpdatingGFlags);
  }

  // Check if the node present in taskParams has a backing instance alive on the IaaS.
  public boolean instanceExists(NodeTaskParams taskParams) {
    // Create the process to fetch information about the node from the cloud provider.
    NodeManager nodeManager = Play.current().injector().instanceOf(NodeManager.class);
    ShellResponse response = nodeManager.nodeCommand(
        NodeManager.NodeCommandType.List, taskParams);
    processShellResponse(response);
    boolean exists = true;
    if (response != null && response.message != null && !StringUtils.isEmpty(response.message)) {
      JsonNode jsonNodeTmp = Json.parse(response.message);
      if (jsonNodeTmp.isArray()) {
        jsonNodeTmp = jsonNodeTmp.get(0);
      }
      final JsonNode jsonNode = jsonNodeTmp;
      Iterator<Entry<String, JsonNode>> iter = jsonNode.fields();
      while (iter.hasNext()) {
        Entry<String, JsonNode> entry = iter.next();
        LOG.info("key {} to value {}.", entry.getKey(), entry.getValue());
        if (entry.getKey().equals("host_found") && entry.getValue().asText().equals("false")) {
          exists = false;
          break;
        }
      }
    } else {
      exists = false;
    }
    return exists;
  }

  // Perform preflight checks on the given node.
  public String performPreflightCheck(NodeDetails node, NodeTaskParams taskParams) {
    // Create the process to fetch information about the node from the cloud provider.
    NodeManager nodeManager = Play.current().injector().instanceOf(NodeManager.class);
    LOG.info("Running preflight checks for node {}.", taskParams.nodeName);
    ShellResponse response = nodeManager.nodeCommand(
        NodeManager.NodeCommandType.Precheck, taskParams);
    if (response.code == 0) {
      JsonNode responseJson = Json.parse(response.message);
      for (JsonNode nodeContent: responseJson) {
        if (!nodeContent.isBoolean() || !nodeContent.asBoolean()) {
          String errString = "Failed preflight checks for node "
            + taskParams.nodeName + ":\n" + response.message;
          LOG.error(errString);
          return response.message;
        }
      }
    }
    return null;
  }

  private boolean isServerAlive(NodeDetails node, ServerType server, String masterAddrs) {
    YBClientService ybService = Play.current().injector().instanceOf(YBClientService.class);

    Universe universe = Universe.get(taskParams().universeUUID);
    String certificate = universe.getCertificate();
    YBClient client = ybService.getClient(masterAddrs, certificate);

    HostAndPort hp = HostAndPort.fromParts(node.cloudInfo.private_ip,
        server == ServerType.MASTER ? node.masterRpcPort : node.tserverRpcPort);
    return client.waitForServer(hp, 5000);
  }

  public boolean isMasterAliveOnNode(NodeDetails node, String masterAddrs) {
    if (!node.isMaster) {
      return false;
    }
    return isServerAlive(node, ServerType.MASTER, masterAddrs);
  }

  public boolean isTserverAliveOnNode(NodeDetails node, String masterAddrs) {
    return isServerAlive(node, ServerType.TSERVER, masterAddrs);
  }

  // Helper API to update the db for the node with the given state.
  public void setNodeState(String nodeName, NodeDetails.NodeState state) {
    // Persist the desired node information into the DB.
    UniverseUpdater updater = nodeStateUpdater(taskParams().universeUUID, nodeName, state);
    saveUniverseDetails(updater);
  }

  // Return list of nodeNames from the given set of node details.
  public String nodeNames(Collection<NodeDetails> nodes) {
    String nodeNames = "";
    for (NodeDetails node : nodes) {
      nodeNames += node.nodeName + ",";
    }
    return nodeNames.substring(0, nodeNames.length() -1);
  }

  /**
  * Disable the loadbalancer to not move data. Used during rolling upgrades.
  */
  public SubTaskGroup createLoadBalancerStateChangeTask(boolean enable) {
    LoadBalancerStateChange.Params params = new LoadBalancerStateChange.Params();
    // Add the universe uuid.
    params.universeUUID = taskParams().universeUUID;
    params.enable = enable;
    LoadBalancerStateChange task = new LoadBalancerStateChange();
    task.initialize(params);

    SubTaskGroup subTaskGroup = new SubTaskGroup("LoadBalancerStateChange", executor);
    subTaskGroup.addTask(task);
    subTaskGroupQueue.add(subTaskGroup);
    return subTaskGroup;
  }

  public void updateBackupState(boolean state) {
    UniverseUpdater updater = new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        universeDetails.backupInProgress = state;
        universe.setUniverseDetails(universeDetails);
      }
    };
    saveUniverseDetails(updater);
  }

  /**
   * Whether to increment the universe/cluster config version. Skip incrementing version if the
   * task updating the universe metadata is create/destroy universe
   *
   * @return true if we should increment the version, false otherwise
   */
  protected boolean shouldIncrementVersion() {
    if (userTaskUUID == null) {
      return false;
    }

    final CustomerTask task = CustomerTask.findByTaskUUID(userTaskUUID);
    if (task == null) {
      return false;
    }

    return !(task.getTarget().equals(CustomerTask.TargetType.Universe) &&
      (task.getType().equals(CustomerTask.TaskType.Create) ||
        task.getType().equals(CustomerTask.TaskType.Delete)));
  }

  private synchronized static int getClusterConfigVersion(UUID universeUUID) {
    final Universe universe = Universe.get(universeUUID);
    final YBClientService ybService = Play.current().injector().instanceOf(YBClientService.class);
    final String hostPorts = universe.getMasterAddresses();
    final String certificate = universe.getCertificate();
    YBClient client = null;
    int version;
    try {
      client = ybService.getClient(hostPorts, certificate);
      version = client.getMasterClusterConfig().getConfig().getVersion();
      ybService.closeClient(client, hostPorts);
    } catch (Exception e) {
      LOG.error("Error occurred retrieving cluster config version", e);
      throw new RuntimeException("Error incrementing cluster config version", e);
    } finally {
      ybService.closeClient(client, hostPorts);
    }

    return version;
  }

  private static synchronized boolean versionsMatch(UUID universeUUID) {
    Universe universe = Universe.get(universeUUID);
    final int clusterConfigVersion = UniverseTaskBase.getClusterConfigVersion(universeUUID);

    // For backwards compatibility (see V56__Alter_Universe_Version.sql)
    if (universe.version == -1) {
      universe.version = clusterConfigVersion;
      universe.save();
    }

    return universe.version == clusterConfigVersion;
  }

  private static void checkUniverseVersion(UUID universeUUID) {
    if (!versionsMatch(universeUUID)) {
      throw new RuntimeException("Universe version does not match cluster config version");
    }
  }

  protected void checkUniverseVersion() {
    UniverseTaskBase.checkUniverseVersion(taskParams().universeUUID);
  }

  /**
   * Increment the cluster config version
   */
  static synchronized private void incrementClusterConfigVersion(UUID universeUUID) {
    Universe universe = Universe.get(universeUUID);
    YBClientService ybService = Play.current().injector().instanceOf(YBClientService.class);
    final String hostPorts = universe.getMasterAddresses();
    String certificate = universe.getCertificate();
    YBClient client = null;
    try {
      client = ybService.getClient(hostPorts, certificate);
      int version = universe.version;
      ModifyClusterConfigIncrementVersion modifyConfig =
        new ModifyClusterConfigIncrementVersion(client, version);
      int newVersion = modifyConfig.incrementVersion();
      ybService.closeClient(client, hostPorts);
      LOG.debug("Updated cluster config version from {} to {}", version, newVersion);
    } catch (Exception e) {
      LOG.error("Error occurred incrementing cluster config version", e);
      throw new RuntimeException("Error incrementing cluster config version", e);
    } finally {
      ybService.closeClient(client, hostPorts);
    }
  }

  /**
   * Run universe updater and increment the cluster config version
   *
   * @param updater the universe updater to run
   * @return the updated universe
   */
  static synchronized protected Universe saveUniverseDetails(
    UUID universeUUID,
    boolean shouldIncrementVersion,
    UniverseUpdater updater
  ) {
    if (shouldIncrementVersion) {
      incrementClusterConfigVersion(universeUUID);
    }

    return Universe.saveDetails(universeUUID, updater, shouldIncrementVersion);
  }

  protected Universe saveUniverseDetails(UniverseUpdater updater) {
    return UniverseTaskBase.saveUniverseDetails(
      taskParams().universeUUID,
      shouldIncrementVersion(),
      updater
    );
  }
}
