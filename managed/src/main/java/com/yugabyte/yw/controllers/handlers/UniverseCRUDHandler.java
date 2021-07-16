/*
 * Copyright 2021 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * http://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.controllers.handlers;

import com.google.common.annotations.VisibleForTesting;
import com.google.common.collect.ImmutableMap;
import com.google.inject.Inject;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.DestroyUniverse;
import com.yugabyte.yw.commissioner.tasks.ReadOnlyClusterDelete;
import com.yugabyte.yw.common.CertificateHelper;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.common.YWServiceException;
import com.yugabyte.yw.common.config.RuntimeConfigFactory;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.forms.*;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.TaskType;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.mvc.Http;

import java.util.*;

import static play.mvc.Http.Status.BAD_REQUEST;

public class UniverseCRUDHandler {

  private static final Logger LOG = LoggerFactory.getLogger(UniverseCRUDHandler.class);

  @Inject Commissioner commissioner;

  @Inject EncryptionAtRestManager keyManager;

  @Inject play.Configuration appConfig;

  @Inject RuntimeConfigFactory runtimeConfigFactory;

  /**
   * Function to Trim keys and values of the passed map.
   *
   * @param data key value pairs.
   * @return key value pairs with trim keys and values.
   */
  @VisibleForTesting
  public static Map<String, String> trimFlags(Map<String, String> data) {
    Map<String, String> trimData = new HashMap<>();
    for (Map.Entry<String, String> intent : data.entrySet()) {
      String key = intent.getKey();
      String value = intent.getValue();
      trimData.put(key.trim(), value.trim());
    }
    return trimData;
  }

  public void configure(Customer customer, UniverseConfigureTaskParams taskParams) {
    if (taskParams.currentClusterType == null) {
      throw new YWServiceException(BAD_REQUEST, "currentClusterType must be set");
    }
    if (taskParams.clusterOperation == null) {
      throw new YWServiceException(BAD_REQUEST, "clusterOperation must be set");
    }
    // TODO(Rahul): When we support multiple read only clusters, change clusterType to cluster
    //  uuid.
    Cluster c =
        taskParams.getCurrentClusterType().equals(UniverseDefinitionTaskParams.ClusterType.PRIMARY)
            ? taskParams.getPrimaryCluster()
            : taskParams.getReadOnlyClusters().get(0);
    UniverseDefinitionTaskParams.UserIntent primaryIntent = c.userIntent;
    primaryIntent.masterGFlags = trimFlags(primaryIntent.masterGFlags);
    primaryIntent.tserverGFlags = trimFlags(primaryIntent.tserverGFlags);
    if (PlacementInfoUtil.checkIfNodeParamsValid(taskParams, c)) {
      PlacementInfoUtil.updateUniverseDefinition(taskParams, customer.getCustomerId(), c.uuid);
    } else {
      throw new YWServiceException(
          BAD_REQUEST,
          "Invalid Node/AZ combination for given instance type " + c.userIntent.instanceType);
    }
  }

  public UniverseResp createUniverse(Customer customer, UniverseDefinitionTaskParams taskParams) {
    LOG.info("Create for {}.", customer.uuid);
    // Get the user submitted form data.

    if (taskParams.getPrimaryCluster() != null
        && !Util.isValidUniverseNameFormat(
            taskParams.getPrimaryCluster().userIntent.universeName)) {
      throw new YWServiceException(BAD_REQUEST, Util.UNIV_NAME_ERROR_MESG);
    }

    if (!taskParams.rootAndClientRootCASame
        && taskParams
            .getPrimaryCluster()
            .userIntent
            .providerType
            .equals(Common.CloudType.kubernetes)) {
      throw new YWServiceException(
          BAD_REQUEST, "root and clientRootCA cannot be different for Kubernetes env.");
    }

    for (Cluster c : taskParams.clusters) {
      Provider provider = Provider.getOrBadRequest(UUID.fromString(c.userIntent.provider));
      // Set the provider code.
      c.userIntent.providerType = Common.CloudType.valueOf(provider.code);
      c.validate();
      // Check if for a new create, no value is set, we explicitly set it to UNEXPOSED.
      if (c.userIntent.enableExposingService
          == UniverseDefinitionTaskParams.ExposingServiceState.NONE) {
        c.userIntent.enableExposingService =
            UniverseDefinitionTaskParams.ExposingServiceState.UNEXPOSED;
      }
      if (c.userIntent.providerType.equals(Common.CloudType.onprem)) {
        if (provider.getConfig().containsKey("USE_HOSTNAME")) {
          c.userIntent.useHostname = Boolean.parseBoolean(provider.getConfig().get("USE_HOSTNAME"));
        }
      }

      if (c.userIntent.providerType.equals(Common.CloudType.kubernetes)) {
        try {
          checkK8sProviderAvailability(provider, customer);
        } catch (IllegalArgumentException e) {
          throw new YWServiceException(BAD_REQUEST, e.getMessage());
        }
      }

      // Set the node exporter config based on the provider
      if (!c.userIntent.providerType.equals(Common.CloudType.kubernetes)) {
        AccessKey accessKey = AccessKey.get(provider.uuid, c.userIntent.accessKeyCode);
        AccessKey.KeyInfo keyInfo = accessKey.getKeyInfo();
        boolean installNodeExporter = keyInfo.installNodeExporter;
        int nodeExporterPort = keyInfo.nodeExporterPort;
        String nodeExporterUser = keyInfo.nodeExporterUser;
        taskParams.extraDependencies.installNodeExporter = installNodeExporter;
        taskParams.communicationPorts.nodeExporterPort = nodeExporterPort;

        for (NodeDetails node : taskParams.nodeDetailsSet) {
          node.nodeExporterPort = nodeExporterPort;
        }

        if (installNodeExporter) {
          taskParams.nodeExporterUser = nodeExporterUser;
        }
      }

      PlacementInfoUtil.updatePlacementInfo(taskParams.getNodesInCluster(c.uuid), c.placementInfo);
    }

    if (taskParams.getPrimaryCluster() != null) {
      UniverseDefinitionTaskParams.UserIntent userIntent =
          taskParams.getPrimaryCluster().userIntent;
      if (userIntent.providerType.isVM() && userIntent.enableYSQL) {
        taskParams.setTxnTableWaitCountFlag = true;
      }
    }

    // Create a new universe. This makes sure that a universe of this name does not already exist
    // for this customer id.
    Universe universe = Universe.create(taskParams, customer.getCustomerId());
    LOG.info("Created universe {} : {}.", universe.universeUUID, universe.name);

    // Add an entry for the universe into the customer table.
    customer.addUniverseUUID(universe.universeUUID);
    customer.save();

    LOG.info(
        "Added universe {} : {} for customer [{}].",
        universe.universeUUID,
        universe.name,
        customer.getCustomerId());

    TaskType taskType = TaskType.CreateUniverse;
    Cluster primaryCluster = taskParams.getPrimaryCluster();

    if (primaryCluster != null) {
      UniverseDefinitionTaskParams.UserIntent primaryIntent = primaryCluster.userIntent;
      primaryIntent.masterGFlags = trimFlags(primaryIntent.masterGFlags);
      primaryIntent.tserverGFlags = trimFlags(primaryIntent.tserverGFlags);
      if (primaryCluster.userIntent.providerType.equals(Common.CloudType.kubernetes)) {
        taskType = TaskType.CreateKubernetesUniverse;
        universe.updateConfig(
            ImmutableMap.of(Universe.HELM2_LEGACY, Universe.HelmLegacy.V3.toString()));
        // TODO(bhavin192): REVIEW: new naming style is enabled by
        // default for all new versions for now.

        // TODO(bhavin192): REVIEW: set the version to latest release
        // of 2.7.x.?  The change
        // https://github.com/yugabyte/charts/commit/5d164cb seems to
        // be available in b113.
        if (Util.compareYbVersions(primaryIntent.ybSoftwareVersion, "2.7.2.0-b113") >= 0) {
          universe.updateConfig(
              ImmutableMap.of(Universe.HELM_NAMING_STYLE, Universe.HelmNamingStyle.NEW.toString()));
        } else {
          universe.updateConfig(
              ImmutableMap.of(Universe.HELM_NAMING_STYLE, Universe.HelmNamingStyle.OLD.toString()));
        }
      } else {
        if (primaryCluster.userIntent.enableIPV6) {
          throw new YWServiceException(
              BAD_REQUEST, "IPV6 not supported for platform deployed VMs.");
        }
      }
      if (primaryCluster.userIntent.enableNodeToNodeEncrypt) {
        // create self signed rootCA in case it is not provided by the user.
        if (taskParams.rootCA == null) {
          taskParams.rootCA =
              CertificateHelper.createRootCA(
                  taskParams.nodePrefix, customer.uuid, appConfig.getString("yb.storage.path"));
        }
        CertificateInfo cert = CertificateInfo.get(taskParams.rootCA);
        if (cert.certType == CertificateInfo.Type.CustomServerCert) {
          throw new YWServiceException(
              BAD_REQUEST,
              "CustomServerCert are only supported for Client to Server Communication.");
        }
        if (cert.certType != CertificateInfo.Type.SelfSigned) {
          if (!taskParams
              .getPrimaryCluster()
              .userIntent
              .providerType
              .equals(Common.CloudType.onprem)) {
            throw new YWServiceException(
                BAD_REQUEST, "Custom certificates are only supported for onprem providers.");
          }
          checkValidRootCA(taskParams.rootCA);
        }
      }
      if (primaryCluster.userIntent.enableClientToNodeEncrypt) {
        if (taskParams.clientRootCA == null) {
          if (taskParams.rootCA != null && taskParams.rootAndClientRootCASame) {
            taskParams.clientRootCA = taskParams.rootCA;
          } else {
            // create self signed clientRootCA in case it is not provided by the user
            // and root and clientRoot CA needs to be different
            taskParams.clientRootCA =
                CertificateHelper.createClientRootCA(
                    taskParams.nodePrefix, customer.uuid, appConfig.getString("yb.storage.path"));
          }
        }

        // Setting rootCA to ClientRootCA in case node to node encryption is disabled.
        // This is necessary to set to ensure backward compatibity as existing parts of
        // codebase (kubernetes) uses rootCA for Client to Node Encryption
        if (taskParams.rootCA == null && taskParams.rootAndClientRootCASame) {
          taskParams.rootCA = taskParams.clientRootCA;
        }

        // If client encryption is enabled, generate the client cert file for each node.
        CertificateInfo cert = CertificateInfo.get(taskParams.clientRootCA);
        if (cert.certType == CertificateInfo.Type.SelfSigned) {
          CertificateHelper.createClientCertificate(
              taskParams.clientRootCA,
              String.format(
                  CertificateHelper.CERT_PATH,
                  appConfig.getString("yb.storage.path"),
                  customer.uuid.toString(),
                  taskParams.clientRootCA.toString()),
              CertificateHelper.DEFAULT_CLIENT,
              null,
              null);
        } else {
          if (cert.certType == CertificateInfo.Type.CustomCertHostPath
              && !taskParams
                  .getPrimaryCluster()
                  .userIntent
                  .providerType
                  .equals(Common.CloudType.onprem)) {
            throw new YWServiceException(
                BAD_REQUEST,
                "CustomCertHostPath certificates are only supported for onprem providers.");
          }
          checkValidRootCA(taskParams.clientRootCA);
          LOG.info(
              "Skipping client certificate creation for universe {} ({}) "
                  + "because cert {} (type {})is not a self-signed cert.",
              universe.name,
              universe.universeUUID,
              taskParams.clientRootCA,
              cert.certType);
        }
      }

      if (primaryCluster.userIntent.enableNodeToNodeEncrypt
          || primaryCluster.userIntent.enableClientToNodeEncrypt) {
        // Set the flag to mark the universe as using TLS enabled and therefore not allowing
        // insecure connections.
        taskParams.allowInsecure = false;
      }

      // TODO: (Daniel) - Move this out to an async task
      if (primaryCluster.userIntent.enableVolumeEncryption
          && primaryCluster.userIntent.providerType.equals(Common.CloudType.aws)) {
        byte[] cmkArnBytes =
            keyManager.generateUniverseKey(
                taskParams.encryptionAtRestConfig.kmsConfigUUID,
                universe.universeUUID,
                taskParams.encryptionAtRestConfig);
        if (cmkArnBytes == null || cmkArnBytes.length == 0) {
          primaryCluster.userIntent.enableVolumeEncryption = false;
        } else {
          // TODO: (Daniel) - Update this to be inside of encryptionAtRestConfig
          taskParams.cmkArn = new String(cmkArnBytes);
        }
      }
    }

    universe.updateConfig(ImmutableMap.of(Universe.TAKE_BACKUPS, "true"));

    // Submit the task to create the universe.
    UUID taskUUID = commissioner.submit(taskType, taskParams);
    LOG.info(
        "Submitted create universe for {}:{}, task uuid = {}.",
        universe.universeUUID,
        universe.name,
        taskUUID);

    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Universe,
        CustomerTask.TaskType.Create,
        universe.name);
    LOG.info(
        "Saved task uuid "
            + taskUUID
            + " in customer tasks table for universe "
            + universe.universeUUID
            + ":"
            + universe.name);

    return UniverseResp.create(universe, taskUUID, runtimeConfigFactory.globalRuntimeConf());
  }

  /**
   * Update Universe with given params. Updates only one cluster at a time (PRIMARY or Read Replica)
   *
   * @return task UUID of customer task that will actually do the update in the background
   */
  public UUID update(Customer customer, Universe u, UniverseDefinitionTaskParams taskParams) {
    checkCanEdit(customer, u);
    if (taskParams.getPrimaryCluster() == null) {
      // Update of a read only cluster.
      return updateCluster(customer, u, taskParams);
    } else {
      return updatePrimaryCluster(customer, u, taskParams);
    }
  }

  private UUID updatePrimaryCluster(
      Customer customer, Universe u, UniverseDefinitionTaskParams taskParams) {
    // Update Primary cluster
    Cluster primaryCluster = taskParams.getPrimaryCluster();
    TaskType taskType = TaskType.EditUniverse;
    if (primaryCluster.userIntent.providerType.equals(Common.CloudType.kubernetes)) {
      taskType = TaskType.EditKubernetesUniverse;
      notHelm2LegacyOrBadRequest(u);
    } else {
      mergeNodeExporterInfo(u, taskParams);
    }
    PlacementInfoUtil.updatePlacementInfo(
        taskParams.getNodesInCluster(primaryCluster.uuid), primaryCluster.placementInfo);
    return submitEditUniverse(customer, u, taskParams, taskType, CustomerTask.TargetType.Universe);
  }

  private UUID updateCluster(
      Customer customer, Universe u, UniverseDefinitionTaskParams taskParams) {
    Cluster cluster = getOnlyReadReplicaOrBadRequest(taskParams.getReadOnlyClusters());
    PlacementInfoUtil.updatePlacementInfo(
        taskParams.getNodesInCluster(cluster.uuid), cluster.placementInfo);
    return submitEditUniverse(
        customer, u, taskParams, TaskType.EditUniverse, CustomerTask.TargetType.Cluster);
  }

  /** Merge node exporter related information from current universe details to the task params */
  private void mergeNodeExporterInfo(Universe u, UniverseDefinitionTaskParams taskParams) {
    // Set the node exporter config based on the provider
    UniverseDefinitionTaskParams universeDetails = u.getUniverseDetails();
    boolean installNodeExporter = universeDetails.extraDependencies.installNodeExporter;
    int nodeExporterPort = universeDetails.communicationPorts.nodeExporterPort;
    String nodeExporterUser = universeDetails.nodeExporterUser;
    taskParams.extraDependencies.installNodeExporter = installNodeExporter;
    taskParams.communicationPorts.nodeExporterPort = nodeExporterPort;

    for (NodeDetails node : taskParams.nodeDetailsSet) {
      node.nodeExporterPort = nodeExporterPort;
    }

    if (installNodeExporter) {
      taskParams.nodeExporterUser = nodeExporterUser;
    }
  }

  private UUID submitEditUniverse(
      Customer customer,
      Universe u,
      UniverseDefinitionTaskParams taskParams,
      TaskType taskType,
      CustomerTask.TargetType targetType) {
    taskParams.rootCA = checkValidRootCA(u.getUniverseDetails().rootCA);
    LOG.info("Found universe {} : name={} at version={}.", u.universeUUID, u.name, u.version);
    UUID taskUUID = commissioner.submit(taskType, taskParams);
    LOG.info(
        "Submitted {} for {} : {}, task uuid = {}.", taskType, u.universeUUID, u.name, taskUUID);
    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer, u.universeUUID, taskUUID, targetType, CustomerTask.TaskType.Update, u.name);
    LOG.info(
        "Saved task uuid {} in customer tasks table for universe {} : {}.",
        taskUUID,
        u.universeUUID,
        u.name);
    return taskUUID;
  }

  private void notHelm2LegacyOrBadRequest(Universe u) {
    Map<String, String> universeConfig = u.getConfig();
    if (!universeConfig.containsKey(Universe.HELM2_LEGACY)) {
      throw new YWServiceException(
          BAD_REQUEST,
          "Cannot perform an edit operation on universe "
              + u.universeUUID
              + " as it is not helm 3 compatible. "
              + "Manually migrate the deployment to helm3 "
              + "and then mark the universe as helm 3 compatible.");
    }
  }

  private UUID checkValidRootCA(UUID rootCA) {
    if (!CertificateInfo.isCertificateValid(rootCA)) {
      String errMsg =
          String.format(
              "The certificate %s needs info. Update the cert and retry.",
              CertificateInfo.get(rootCA).label);
      LOG.error(errMsg);
      throw new YWServiceException(BAD_REQUEST, errMsg);
    }
    return rootCA;
  }

  private void checkCanEdit(Customer customer, Universe u) {
    LOG.info("Update universe {} [ {} ] customer {}.", u.name, u.universeUUID, customer.uuid);
    if (!u.getUniverseDetails().isUniverseEditable()) {
      String errMsg = "Universe UUID " + u.universeUUID + " cannot be edited.";
      LOG.error(errMsg);
      throw new YWServiceException(BAD_REQUEST, errMsg);
    }

    if (u.nodesInTransit()) {
      // TODO 503 - Service Unavailable
      throw new YWServiceException(
          BAD_REQUEST,
          "Cannot perform an edit operation on universe "
              + u.universeUUID
              + " as it has nodes in one of "
              + NodeDetails.IN_TRANSIT_STATES
              + " states.");
    }
  }

  public List<UniverseResp> list(Customer customer) {
    List<UniverseResp> universes = new ArrayList<>();
    // TODO: Restrict the list api json payload, possibly to only include UUID, Name etc
    for (Universe universe : customer.getUniverses()) {
      UniverseResp universePayload =
          UniverseResp.create(universe, null, runtimeConfigFactory.globalRuntimeConf());
      universes.add(universePayload);
    }
    return universes;
  }

  public List<UniverseResp> findByName(String name) {
    return Universe.maybeGetUniverseByName(name)
        .map(
            value ->
                Collections.singletonList(
                    UniverseResp.create(value, null, runtimeConfigFactory.globalRuntimeConf())))
        .orElseGet(Collections::emptyList);
  }

  public UUID destroy(
      Customer customer, Universe universe, boolean isForceDelete, boolean isDeleteBackups) {
    LOG.info(
        "Destroy universe, customer uuid: {}, universe: {} [ {} ] ",
        customer.uuid,
        universe.name,
        universe.universeUUID);

    // Create the Commissioner task to destroy the universe.
    DestroyUniverse.Params taskParams = new DestroyUniverse.Params();
    taskParams.universeUUID = universe.universeUUID;
    // There is no staleness of a delete request. Perform it even if the universe has changed.
    taskParams.expectedUniverseVersion = -1;
    taskParams.customerUUID = customer.uuid;
    taskParams.isForceDelete = isForceDelete;
    taskParams.isDeleteBackups = isDeleteBackups;
    // Submit the task to destroy the universe.
    TaskType taskType = TaskType.DestroyUniverse;
    UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
    Cluster primaryCluster = universeDetails.getPrimaryCluster();
    if (primaryCluster.userIntent.providerType.equals(Common.CloudType.kubernetes)) {
      taskType = TaskType.DestroyKubernetesUniverse;
    }

    // Update all current tasks for this universe to be marked as done if it is a force delete.
    if (isForceDelete) {
      markAllUniverseTasksAsCompleted(universe.universeUUID);
    }

    UUID taskUUID = commissioner.submit(taskType, taskParams);
    LOG.info(
        "Submitted destroy universe for " + universe.universeUUID + ", task uuid = " + taskUUID);

    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Universe,
        CustomerTask.TaskType.Delete,
        universe.name);

    LOG.info(
        "Start destroyUniverse " + universe.universeUUID + " for customer [" + customer.name + "]");
    return taskUUID;
  }

  public UUID createCluster(
      Customer customer, Universe universe, UniverseDefinitionTaskParams taskParams) {
    LOG.info("Create cluster for {} in {}.", customer.uuid, universe.universeUUID);
    // Get the user submitted form data.

    if (taskParams.clusters == null || taskParams.clusters.size() != 1)
      throw new YWServiceException(
          BAD_REQUEST,
          "Invalid 'clusters' field/size: "
              + taskParams.clusters
              + " for "
              + universe.universeUUID);

    List<Cluster> newReadOnlyClusters = taskParams.clusters;
    List<Cluster> existingReadOnlyClusters = universe.getUniverseDetails().getReadOnlyClusters();
    LOG.info(
        "newReadOnly={}, existingRO={}.",
        newReadOnlyClusters.size(),
        existingReadOnlyClusters.size());

    if (existingReadOnlyClusters.size() > 0 && newReadOnlyClusters.size() > 0) {
      throw new YWServiceException(
          BAD_REQUEST, "Can only have one read-only cluster per universe for now.");
    }

    Cluster cluster = getOnlyReadReplicaOrBadRequest(newReadOnlyClusters);
    if (cluster.uuid == null) {
      String errMsg = "UUID of read-only cluster should be non-null.";
      LOG.error(errMsg);
      throw new YWServiceException(BAD_REQUEST, errMsg);
    }

    if (cluster.clusterType != UniverseDefinitionTaskParams.ClusterType.ASYNC) {
      String errMsg =
          "Read-only cluster type should be "
              + UniverseDefinitionTaskParams.ClusterType.ASYNC
              + " but is "
              + cluster.clusterType;
      LOG.error(errMsg);
      throw new YWServiceException(BAD_REQUEST, errMsg);
    }

    // Set the provider code.
    Cluster c = taskParams.clusters.get(0);
    Provider provider = Provider.getOrBadRequest(UUID.fromString(c.userIntent.provider));
    c.userIntent.providerType = Common.CloudType.valueOf(provider.code);
    c.validate();

    if (c.userIntent.providerType.equals(Common.CloudType.kubernetes)) {
      try {
        checkK8sProviderAvailability(provider, customer);
      } catch (IllegalArgumentException e) {
        throw new YWServiceException(BAD_REQUEST, e.getMessage());
      }
    }

    PlacementInfoUtil.updatePlacementInfo(taskParams.getNodesInCluster(c.uuid), c.placementInfo);

    // Submit the task to create the cluster.
    UUID taskUUID = commissioner.submit(TaskType.ReadOnlyClusterCreate, taskParams);
    LOG.info(
        "Submitted create cluster for {}:{}, task uuid = {}.",
        universe.universeUUID,
        universe.name,
        taskUUID);

    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Cluster,
        CustomerTask.TaskType.Create,
        universe.name);
    LOG.info(
        "Saved task uuid {} in customer tasks table for universe {}:{}",
        taskUUID,
        universe.universeUUID,
        universe.name);
    return taskUUID;
  }

  public UUID clusterDelete(
      Customer customer, Universe universe, UUID clusterUUID, Boolean isForceDelete) {
    List<Cluster> existingReadOnlyClusters = universe.getUniverseDetails().getReadOnlyClusters();

    Cluster cluster = getOnlyReadReplicaOrBadRequest(existingReadOnlyClusters);
    UUID uuid = cluster.uuid;
    if (!uuid.equals(clusterUUID)) {
      String errMsg =
          "Uuid " + clusterUUID + " to delete cluster not found, only " + uuid + " found.";
      LOG.error(errMsg);
      throw new YWServiceException(BAD_REQUEST, errMsg);
    }

    // Create the Commissioner task to destroy the universe.
    ReadOnlyClusterDelete.Params taskParams = new ReadOnlyClusterDelete.Params();
    taskParams.universeUUID = universe.universeUUID;
    taskParams.clusterUUID = clusterUUID;
    taskParams.isForceDelete = isForceDelete;
    taskParams.expectedUniverseVersion = universe.version;

    // Submit the task to delete the cluster.
    UUID taskUUID = commissioner.submit(TaskType.ReadOnlyClusterDelete, taskParams);
    LOG.info(
        "Submitted delete cluster for {} in {}, task uuid = {}.",
        clusterUUID,
        universe.name,
        taskUUID);

    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Cluster,
        CustomerTask.TaskType.Delete,
        universe.name);
    LOG.info(
        "Saved task uuid {} in customer tasks table for universe {}:{}",
        taskUUID,
        universe.universeUUID,
        universe.name);
    return taskUUID;
  }

  private Cluster getOnlyReadReplicaOrBadRequest(List<Cluster> readReplicaClusters) {
    if (readReplicaClusters.size() != 1) {
      String errMsg =
          "Only one read-only cluster expected, but we got " + readReplicaClusters.size();
      LOG.error(errMsg);
      throw new YWServiceException(BAD_REQUEST, errMsg);
    }
    return readReplicaClusters.get(0);
  }

  /**
   * Throw an exception if the given provider has an AZ with KUBENAMESPACE in the config and the
   * provdier has a cluster associated with it. Providers with namespace setting don't support
   * multiple clusters.
   *
   * @param providerToCheck Provider object
   */
  private static void checkK8sProviderAvailability(Provider providerToCheck, Customer customer) {
    boolean isNamespaceSet = false;
    for (Region r : Region.getByProvider(providerToCheck.uuid)) {
      for (AvailabilityZone az : AvailabilityZone.getAZsForRegion(r.uuid)) {
        if (az.getConfig().containsKey("KUBENAMESPACE")) {
          isNamespaceSet = true;
        }
      }
    }

    if (isNamespaceSet) {
      for (UUID universeUUID : Universe.getAllUUIDs(customer)) {
        Universe u = Universe.getOrBadRequest(universeUUID);
        List<Cluster> clusters = u.getUniverseDetails().getReadOnlyClusters();
        clusters.add(u.getUniverseDetails().getPrimaryCluster());
        for (Cluster c : clusters) {
          UUID providerUUID = UUID.fromString(c.userIntent.provider);
          if (providerUUID.equals(providerToCheck.uuid)) {
            String msg =
                "Universe "
                    + u.name
                    + " ("
                    + u.universeUUID
                    + ") already exists with provider "
                    + providerToCheck.name
                    + " ("
                    + providerToCheck.uuid
                    + "). Only one universe can be created with providers having KUBENAMESPACE set "
                    + "in the AZ config.";
            LOG.error(msg);
            throw new IllegalArgumentException(msg);
          }
        }
      }
    }
  }

  void markAllUniverseTasksAsCompleted(UUID universeUUID) {
    for (CustomerTask task : CustomerTask.findIncompleteByTargetUUID(universeUUID)) {
      task.markAsCompleted();
      TaskInfo taskInfo = TaskInfo.get(task.getTaskUUID());
      if (taskInfo != null) {
        taskInfo.setTaskState(TaskInfo.State.Failure);
        taskInfo.save();
      }
    }
  }

  public UUID upgrade(Customer customer, Universe universe, UpgradeParams taskParams) {
    if (taskParams.taskType == null) {
      throw new YWServiceException(BAD_REQUEST, "task type is required");
    }

    if (taskParams.upgradeOption == UpgradeParams.UpgradeOption.ROLLING_UPGRADE
        && universe.nodesInTransit()) {
      throw new YWServiceException(
          BAD_REQUEST,
          "Cannot perform rolling upgrade of universe "
              + universe.universeUUID
              + " as it has nodes in one of "
              + NodeDetails.IN_TRANSIT_STATES
              + " states.");
    }

    // TODO: we need to refactor this to read from cluster
    // instead of top level task param, for now just copy the master flag and tserver flag
    // from primary cluster.
    UniverseDefinitionTaskParams.UserIntent primaryIntent =
        taskParams.getPrimaryCluster().userIntent;
    primaryIntent.masterGFlags = trimFlags(primaryIntent.masterGFlags);
    primaryIntent.tserverGFlags = trimFlags(primaryIntent.tserverGFlags);
    taskParams.masterGFlags = primaryIntent.masterGFlags;
    taskParams.tserverGFlags = primaryIntent.tserverGFlags;

    CustomerTask.TaskType customerTaskType;
    // Validate if any required params are missed based on the taskType
    switch (taskParams.taskType) {
      case VMImage:
        if (!runtimeConfigFactory.forUniverse(universe).getBoolean("yb.cloud.enabled")) {
          throw new YWServiceException(
              Http.Status.METHOD_NOT_ALLOWED, "VM image upgrade is disabled");
        }

        Common.CloudType provider = primaryIntent.providerType;
        if (!(provider == Common.CloudType.gcp || provider == Common.CloudType.aws)) {
          throw new YWServiceException(
              BAD_REQUEST,
              "VM image upgrade is only supported for AWS / GCP, got: " + provider.toString());
        }

        boolean hasEphemeralStorage = false;
        if (provider == Common.CloudType.gcp) {
          if (primaryIntent.deviceInfo.storageType == PublicCloudConstants.StorageType.Scratch) {
            hasEphemeralStorage = true;
          }
        } else {
          if (taskParams.getPrimaryCluster().isAwsClusterWithEphemeralStorage()) {
            hasEphemeralStorage = true;
          }
        }

        if (hasEphemeralStorage) {
          throw new YWServiceException(
              BAD_REQUEST, "Cannot upgrade a universe with ephemeral storage");
        }

        if (taskParams.machineImages.isEmpty()) {
          throw new YWServiceException(
              BAD_REQUEST, "machineImages param is required for taskType: " + taskParams.taskType);
        }

        customerTaskType = CustomerTask.TaskType.UpgradeVMImage;
        break;
      case ResizeNode:
        if (!runtimeConfigFactory.forUniverse(universe).getBoolean("yb.cloud.enabled")) {
          throw new YWServiceException(
              Http.Status.METHOD_NOT_ALLOWED, "Smart resizing is disabled");
        }

        Common.CloudType providerType =
            universe.getUniverseDetails().getPrimaryCluster().userIntent.providerType;
        if (!(providerType.equals(Common.CloudType.gcp)
            || providerType.equals(Common.CloudType.aws))) {
          throw new YWServiceException(
              BAD_REQUEST,
              "Smart resizing is only supported for AWS / GCP, It is: " + providerType.toString());
        }

        customerTaskType = CustomerTask.TaskType.ResizeNode;
        break;
      case Software:
        customerTaskType = CustomerTask.TaskType.UpgradeSoftware;
        if (taskParams.ybSoftwareVersion == null || taskParams.ybSoftwareVersion.isEmpty()) {
          throw new YWServiceException(
              BAD_REQUEST,
              "ybSoftwareVersion param is required for taskType: " + taskParams.taskType);
        }
        break;
      case GFlags:
        customerTaskType = CustomerTask.TaskType.UpgradeGflags;
        // TODO(BUG): This looks like a bug. This should check for empty instead of null.
        // Fixing this cause unit test to break. Leaving the TODO for now.
        if (taskParams.masterGFlags == null && taskParams.tserverGFlags == null) {
          throw new YWServiceException(
              BAD_REQUEST, "gflags param is required for taskType: " + taskParams.taskType);
        }
        UniverseDefinitionTaskParams.UserIntent univIntent =
            universe.getUniverseDetails().getPrimaryCluster().userIntent;
        if (taskParams.masterGFlags != null
            && taskParams.masterGFlags.equals(univIntent.masterGFlags)
            && taskParams.tserverGFlags != null
            && taskParams.tserverGFlags.equals(univIntent.tserverGFlags)) {
          throw new YWServiceException(BAD_REQUEST, "Neither master nor tserver gflags changed.");
        }
        break;
      case Restart:
        customerTaskType = CustomerTask.TaskType.Restart;
        if (taskParams.upgradeOption != UpgradeParams.UpgradeOption.ROLLING_UPGRADE) {
          throw new YWServiceException(BAD_REQUEST, "Rolling restart has to be a ROLLING UPGRADE.");
        }
        break;
      case Certs:
        customerTaskType = CustomerTask.TaskType.UpdateCert;
        if (taskParams.certUUID == null) {
          throw new YWServiceException(
              BAD_REQUEST, "certUUID is required for taskType: " + taskParams.taskType);
        }
        if (!taskParams
            .getPrimaryCluster()
            .userIntent
            .providerType
            .equals(Common.CloudType.onprem)) {
          throw new YWServiceException(
              BAD_REQUEST, "Certs can only be rotated for onprem." + taskParams.taskType);
        }
        CertificateInfo cert = CertificateInfo.get(taskParams.certUUID);
        if (cert.certType != CertificateInfo.Type.CustomCertHostPath) {
          throw new YWServiceException(
              BAD_REQUEST, "Need a custom cert. Cannot use self-signed." + taskParams.taskType);
        }
        cert = CertificateInfo.get(universe.getUniverseDetails().rootCA);
        if (cert.certType != CertificateInfo.Type.CustomCertHostPath) {
          throw new YWServiceException(
              BAD_REQUEST, "Only custom certs can be rotated." + taskParams.taskType);
        }
        break;
      default:
        throw new YWServiceException(BAD_REQUEST, "Unexpected value: " + taskParams.taskType);
    }

    LOG.info("Got task type {}", customerTaskType.toString());
    taskParams.universeUUID = universe.universeUUID;
    taskParams.expectedUniverseVersion = universe.version;

    LOG.info(
        "Found universe {} : name={} at version={}.",
        universe.universeUUID,
        universe.name,
        universe.version);

    Map<String, String> universeConfig = universe.getConfig();
    TaskType taskType = TaskType.UpgradeUniverse;
    if (taskParams
        .getPrimaryCluster()
        .userIntent
        .providerType
        .equals(Common.CloudType.kubernetes)) {
      taskType = TaskType.UpgradeKubernetesUniverse;
      if (!universeConfig.containsKey(Universe.HELM2_LEGACY)) {
        throw new YWServiceException(
            BAD_REQUEST,
            "Cannot perform upgrade operation on universe. "
                + universe.universeUUID
                + " as it is not helm 3 compatible. "
                + "Manually migrate the deployment to helm3 "
                + "and then mark the universe as helm 3 compatible.");
      }
      if (!universeConfig.containsKey(Universe.HELM_NAMING_STYLE)) {
        universe.updateConfig(
            ImmutableMap.of(Universe.HELM_NAMING_STYLE, Universe.HelmNamingStyle.OLD.toString()));
      }
    }

    taskParams.rootCA = checkValidRootCA(universe.getUniverseDetails().rootCA);

    UUID taskUUID = commissioner.submit(taskType, taskParams);
    LOG.info(
        "Submitted upgrade universe for {} : {}, task uuid = {}.",
        universe.universeUUID,
        universe.name,
        taskUUID);

    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Universe,
        customerTaskType,
        universe.name);
    LOG.info(
        "Saved task uuid {} in customer tasks table for universe {} : {}.",
        taskUUID,
        universe.universeUUID,
        universe.name);
    return taskUUID;
  }

  public UUID updateDiskSize(
      Customer customer, Universe universe, DiskIncreaseFormData taskParams) {
    LOG.info("Disk Size Increase {} for {}.", customer.uuid, universe.universeUUID);
    if (taskParams.size == 0) {
      throw new YWServiceException(BAD_REQUEST, "Size cannot be 0.");
    }

    UniverseDefinitionTaskParams.UserIntent primaryIntent =
        taskParams.getPrimaryCluster().userIntent;
    if (taskParams.size <= primaryIntent.deviceInfo.volumeSize) {
      throw new YWServiceException(BAD_REQUEST, "Size can only be increased.");
    }
    if (primaryIntent.deviceInfo.storageType == PublicCloudConstants.StorageType.Scratch) {
      throw new YWServiceException(BAD_REQUEST, "Scratch type disk cannot be modified.");
    }
    if (taskParams.getPrimaryCluster().isAwsClusterWithEphemeralStorage()) {
      throw new YWServiceException(BAD_REQUEST, "Cannot modify instance volumes.");
    }

    primaryIntent.deviceInfo.volumeSize = taskParams.size;
    taskParams.universeUUID = universe.universeUUID;
    taskParams.expectedUniverseVersion = universe.version;
    LOG.info(
        "Found universe {} : name={} at version={}.",
        universe.universeUUID,
        universe.name,
        universe.version);

    TaskType taskType = TaskType.UpdateDiskSize;
    if (taskParams
        .getPrimaryCluster()
        .userIntent
        .providerType
        .equals(Common.CloudType.kubernetes)) {
      throw new YWServiceException(BAD_REQUEST, "Kubernetes disk size increase not yet supported.");
    }

    UUID taskUUID = commissioner.submit(taskType, taskParams);
    LOG.info(
        "Submitted update disk universe for {} : {}, task uuid = {}.",
        universe.universeUUID,
        universe.name,
        taskUUID);

    CustomerTask.TaskType customerTaskType = CustomerTask.TaskType.UpdateDiskSize;

    // Add this task uuid to the user universe.
    CustomerTask.create(
        customer,
        universe.universeUUID,
        taskUUID,
        CustomerTask.TargetType.Universe,
        customerTaskType,
        universe.name);
    LOG.info(
        "Saved task uuid {} in customer tasks table for universe {} : {}.",
        taskUUID,
        universe.universeUUID,
        universe.name);
    return taskUUID;
  }
}
