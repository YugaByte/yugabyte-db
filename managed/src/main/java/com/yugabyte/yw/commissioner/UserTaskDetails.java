package com.yugabyte.yw.commissioner;

import com.yugabyte.yw.models.TaskInfo;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.ArrayList;
import java.util.List;

/**
 * Class that encapsulates the user task details.
 */
public class UserTaskDetails {
  public static final Logger LOG = LoggerFactory.getLogger(UserTaskDetails.class);

  // The various groupings of user facing subtasks.
  public enum SubTaskGroupType {
    // Ignore this subtask and do not display it to the user.
    Invalid,

    // Perform preflight checks to determine if the node is ready to be configured or provisioned.
    PreflightChecks,

    // Deploying machines in the desired cloud, fetching information (ip address, etc) of these
    // newly deployed machines, etc.
    Provisioning,

    // Running software upgrade on YugaByte clusters.
    UpgradingSoftware,

    // Download YB software locally but not install it.
    DownloadingSoftware,

    // Configure the mount points and the directories, install the desired version of YB software,
    // add the control scripts to start and stop daemons, setup monitoring, etc.
    InstallingSoftware,

    // Start the masters to create a new universe configuration, wait for leader elections, set
    // placement info, wait for the tservers to start up, etc.
    ConfigureUniverse,

    // Migrating data from one set of nodes to another.
    WaitForDataMigration,

    // Remove old, unused servers.
    RemovingUnusedServers,

    // Updating GFlags
    UpdatingGFlags,

    // Bootstrap Cloud
    BootstrappingCloud,

    // Bootstrapping Region
    BootstrappingRegion,

    // Creating Access Key
    CreateAccessKey,

    // Initializing Cloud Metadata
    InitializeCloudMetadata,

    // Cleanup Cloud
    CleanupCloud,

    // Creating Table
    CreatingTable,

    // Importing Data
    ImportingData,

    // Delete Node
    DeletingNode,

    // Stopping Node
    StoppingNode,

    // Starting Node
    StartingNode,

    // Start master and tserver processes on a node
    StartingNodeProcesses,

    // Stop master and tserver processes on a node
    StoppingNodeProcesses,

    // Adding a node.
    AddingNode,

    // Removing a node.
    RemovingNode,

    // Releasing the backing IaaS instance.
    ReleasingInstance,

    // Deleting Table
    DeletingTable,

    // Creating Table Backup
    CreatingTableBackup,

    // Restoring Table Backup
    RestoringTableBackup,

    // Create Namespace for Kubectl.
    CreateNamespace,

    // Apply Pull Secret.
    ApplySecret,

    // Initialize Kubernetes cluster
    HelmInit,

    // Install YugaByte helm chart.
    HelmInstall,

    // Update number of nodes k8s
    UpdateNumNodes,

    // Delete YugaByte helm chart.
    HelmDelete,

    // Delete Kubernetes volumes created by helm chart.
    KubernetesVolumeDelete,

    // Delete kubernetes namespace
    KubernetesNamespaceDelete,

    // Fetch the Kubernetes pod information.
    KubernetesPodInfo,

    // Wait for Kubernetes pod deployment
    KubernetesWaitForPod,

    // Wait for Kubernetes to have correct number of pods.
    KubernetesCheckNumPod,

    // Upgrade existing helm chart.
    HelmUpgrade,

    // Upgrade pod in Kubernetes.
    KubernetesUpgradePod,

    // Run the initdb script in a tserver pod. (Deprecated)
    KubernetesInitYSQL,

    // Start master process on a node
    StartingMasterProcess,

    // Rotate Node Certs.
    RotatingCert;
  }

  public List<SubTaskDetails> taskDetails;

  // TODO: all the strings in this method should move into conf files eventually.
  public static SubTaskDetails createSubTask(SubTaskGroupType subTaskGroupType) {
    String title;
    String description;
    switch (subTaskGroupType) {
      case PreflightChecks:
        title = "Preflight Checks";
        description = "Perform preflight checks to determine if node is ready" +
          " to be provisioned/configured.";
          break;
      case Provisioning:
        title = "Provisioning";
        description = "Deploying machines of the required config into the desired cloud and" +
          " fetching information about them.";
        break;
      case UpgradingSoftware:
        title = "Upgrading software";
        description = "Upgrading YugaByte software on existing clusters.";
        break;
      case InstallingSoftware:
        title = "Installing software";
        description = "Configuring mount points, setting up the various directories and installing" +
          " the YugaByte software on the newly provisioned nodes.";
        break;
      case ConfigureUniverse:
        title = "Configuring the universe";
        description = "Creating and populating the universe config, waiting for the various" +
          " machines to discover one another.";
        break;
      case WaitForDataMigration:
        title = "Waiting for data migration";
        description = "Waiting for the data to get copied into the new set of machines to achieve" +
          " the desired configuration.";
        break;
      case RemovingUnusedServers:
        title = "Removing servers no longer used";
        description = "Removing servers that are no longer needed once the configuration change has" +
          " been successfully completed";
        break;
      case DownloadingSoftware:
        title = "Downloading software";
        description = "Downloading the YugaByte software on provisioned nodes.";
        break;
      case UpdatingGFlags:
        title = "Updating gflags";
        description = "Updating GFlags on provisioned nodes.";
        break;
      case BootstrappingCloud:
        title = "Bootstrapping Cloud";
        description = "Set up AccessKey, Region, and Provider for a given cloud Provider.";
        break;
      case BootstrappingRegion:
        title = "Bootstrapping Region";
        description = "Set up AccessKey, Region, and Provider for a given cloud Provider.";
        break;
      case CreateAccessKey:
        title = "Creating AccessKey";
        description = "Set up AccessKey in the given Provider Vault";
        break;
      case InitializeCloudMetadata:
        title = "Initializing Cloud Metadata";
        description = "Initialize Instance Pricing and Zone Metadata from Cloud Provider";
        break;
      case CleanupCloud:
        title = "Cleaning Up Cloud";
        description = "Remove AccessKey, Region, and Provider for a given cloud Provider.";
        break;
      case CreatingTable:
        title = "Creating Table";
        description = "Create a table.";
        break;
      case ImportingData:
        title = "Importing Data";
        description = "Import a large amount of data into a table";
        break;
      case DeletingNode:
        title = "Deleting Node";
        description = "Remove Node entry from Universe details";
        break;
      case StoppingNode:
        title = "Stopping Node";
        description = "Waiting for node to stop.";
        break;
      case StartingNode:
        title = "Starting Node";
        description = "Waiting for node to start.";
        break;
      case StartingNodeProcesses:
        title = "Starting Node processes";
        description = "Waiting for node to start either tserver or master process.";
        break;
     case AddingNode:
        title = "Adding a node";
        description = "Add a node to universe and start tserver (and master, if required).";
        break;
     case RemovingNode:
        title = "Removing a node";
        description = "Remove a node from universe.";
        break;
     case ReleasingInstance:
        title = "Releasing the instance back to the IaaS.";
        description = "Releasing the instance from universe.";
        break;
      case StoppingNodeProcesses:
        title = "Stopping Node Processes";
        description = "Stopping tserver and master processes on the node.";
        break;
      case DeletingTable:
        title = "Deleting Table";
        description = "Delete an existing table from a universe";
        break;
      case CreatingTableBackup:
        title = "Creating Table Backup";
        description = "Creating backup for a table.";
        break;
      case RestoringTableBackup:
        title = "Restoring Table Backup";
        description = "Restoring from a backup.";
        break;
      case HelmInit:
        title = "Kubernetes Helm Charts initialize";
        description = "Initialize Kubernetes Helm Charts";
        break;
      case HelmInstall:
        title = "Kubernetes Helm Charts Install";
        description = "Install Kubernetes Helm Charts";
        break;
      case HelmUpgrade:
        title = "Kubernetes Helm Charts Update";
        description = "Update Kubernetes Helm Charts";
        break;
      case KubernetesPodInfo:
        title = "Fetch Kubernetes Pods Information";
        description = "Fetch Kubernetes Pods Information";
        break;
      case HelmDelete:
        title = "Delete Kubernetes Helm Charts";
        description = "Delete Kubernetes Helm Charts";
        break;
      case KubernetesVolumeDelete:
        title = "Delete Kubernetes Volumes";
        description = "Delete Kubernetes Volumes";
        break;
      case KubernetesNamespaceDelete:
        title = "Delete Kubernetes Namespace";
        description = "Delete Kubernetes Namespace";
        break;
      case KubernetesWaitForPod:
        title = "Wait for Kubernetes pod to run";
        description = "Wait for Kubernetes pod to run";
        break;
      case KubernetesCheckNumPod:
        title = "Wait for Kubernetes to have correct number of pods.";
        description = "Wait for Kubernetes to have correct number of pods.";
        break;
      case KubernetesUpgradePod:
        title = "Upgrade Kubernetes Pod";
        description = "Upgrade Kubernetes Pod";
        break;
      case KubernetesInitYSQL:
        title = "Initialize YSQL in Kubernetes Universe";
        description = "Initialize YSQL in Kubernetes Universe";
        break;
      case StartingMasterProcess:
        title = "Starting Master Process";
        description = "Waiting for node to start the master process.";
        break;
      case RotatingCert:
        title = "Rotating Cert";
        description = "Changing certs.";
        break;
      case CreateNamespace:
        title = "Creating Namespace";
        description = "Create namespace for Kubectl.";
        break;
      case ApplySecret:
        title = "Applying Pull Secret";
        description = "Apply Pull Secret.";
        break;
      case UpdateNumNodes:
        title = "Updating number of nodes";
        description = "Update number of nodes.";
        break;
      default:
        LOG.warn("UserTaskDetails: Missing SubTaskDetails for : {}", subTaskGroupType);
        return null;
    }
    return new SubTaskDetails(title, description);
  }

  public UserTaskDetails() {
    taskDetails = new ArrayList<>();
  }

  public void add(SubTaskDetails subtaskDetails) {
    taskDetails.add(subtaskDetails);
  }

  public static class SubTaskDetails {
    // User facing title.
    private String title;

    // User facing short description.
    private String description;

    // The state of the task.
    private TaskInfo.State state;

    private SubTaskDetails(String title, String description) {
      this.title = title;
      this.description = description;
      this.state = TaskInfo.State.Unknown;
    }

    public void setState(TaskInfo.State state) {
      this.state = state;
    }

    public String getTitle() {
      return title;
    }

    public String getDescription() {
      return description;
    }

    public String getState() {
      return state.toString();
    }
  }
}
