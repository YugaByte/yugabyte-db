/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.common.collect.ImmutableMap;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.AbstractTaskBase;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;
import com.yugabyte.yw.commissioner.tasks.UniverseTaskBase;
import com.yugabyte.yw.common.CertificateHelper;
import com.yugabyte.yw.common.KubernetesManager;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.AbstractTaskParams;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import play.Application;
import play.Environment;
import play.api.Play;
import play.libs.Json;
import org.yaml.snakeyaml.Yaml;

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Map.Entry;
import java.util.Set;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class KubernetesCommandExecutor extends UniverseTaskBase {
  public enum CommandType {
    CREATE_NAMESPACE,
    APPLY_SECRET,
    HELM_INIT,
    HELM_INSTALL,
    HELM_UPGRADE,
    UPDATE_NUM_NODES,
    HELM_DELETE,
    VOLUME_DELETE,
    NAMESPACE_DELETE,
    POD_INFO,
    // The following flag is deprecated.
    INIT_YSQL;

    public String getSubTaskGroupName() {
      switch (this) {
        case CREATE_NAMESPACE:
          return UserTaskDetails.SubTaskGroupType.CreateNamespace.name();
        case APPLY_SECRET:
          return UserTaskDetails.SubTaskGroupType.ApplySecret.name();
        case HELM_INSTALL:
          return UserTaskDetails.SubTaskGroupType.HelmInstall.name();
        case HELM_UPGRADE:
          return UserTaskDetails.SubTaskGroupType.HelmUpgrade.name();
        case UPDATE_NUM_NODES:
          return UserTaskDetails.SubTaskGroupType.UpdateNumNodes.name();
        case HELM_DELETE:
          return UserTaskDetails.SubTaskGroupType.HelmDelete.name();
        case VOLUME_DELETE:
          return UserTaskDetails.SubTaskGroupType.KubernetesVolumeDelete.name();
        case NAMESPACE_DELETE:
          return UserTaskDetails.SubTaskGroupType.KubernetesNamespaceDelete.name();
        case POD_INFO:
          return UserTaskDetails.SubTaskGroupType.KubernetesPodInfo.name();
        case INIT_YSQL:
          return UserTaskDetails.SubTaskGroupType.KubernetesInitYSQL.name();
      }
      return null;
    }
  }

  @Inject
  KubernetesManager kubernetesManager;

  @Inject
  Application application;

  @Inject
  private play.Environment environment;

  static final Pattern nodeNamePattern = Pattern.compile(".*-n(\\d+)+");

  // Added constant to compute CPU burst limit
  static final double burstVal = 1.2;

  static final String defaultStorageClass = "standard";

  @Override
  public void initialize(ITaskParams params) {
    this.kubernetesManager = Play.current().injector().instanceOf(KubernetesManager.class);
    this.application = Play.current().injector().instanceOf(Application.class);
    this.environment = Play.current().injector().instanceOf(Environment.class);
    super.initialize(params);
  }

  public static class Params extends UniverseTaskParams {
    public UUID providerUUID;
    public CommandType commandType;
    // We use the nodePrefix as Helm Chart's release name,
    // so we would need that for any sort helm operations.
    public String nodePrefix;
    public String ybSoftwareVersion = null;
    public boolean enableNodeToNodeEncrypt = false;
    public boolean enableClientToNodeEncrypt = false;
    public UUID rootCA = null;
    public ServerType serverType = ServerType.EITHER;
    public int tserverPartition = 0;
    public int masterPartition = 0;

    // Master addresses in multi-az case (to have control over different deployments).
    public String masterAddresses = null;

    // PlacementInfo to correctly set the placement details on the servers at start
    // as well as to control the replicas for each deployment.
    public PlacementInfo placementInfo = null;

    // The target cluster's config.
    public Map<String, String> config = null;

  }

  protected KubernetesCommandExecutor.Params taskParams() {
    return (KubernetesCommandExecutor.Params)taskParams;
  }

  @Override
  public void run() {
    String overridesFile;
    boolean flag = false;

    // In case no config is provided, assume it is at the provider level
    // (for backwards compatibility).
    Map<String, String> config = taskParams().config;
    if (config == null) {
      config = Provider.get(taskParams().providerUUID).getConfig();
    }
    // TODO: add checks for the shell process handler return values.
    ShellResponse response = null;
    switch (taskParams().commandType) {
      case CREATE_NAMESPACE:
        response = kubernetesManager.createNamespace(config, taskParams().nodePrefix);
        break;
      case APPLY_SECRET:
        String pullSecret = this.getPullSecret();
        if (pullSecret != null) {
          response = kubernetesManager.applySecret(config, taskParams().nodePrefix, pullSecret);
        }
        break;
      case HELM_INSTALL:
        overridesFile = this.generateHelmOverride();
        response = kubernetesManager.helmInstall(config, taskParams().providerUUID, taskParams().nodePrefix, overridesFile);
        flag = true;
        break;
      case HELM_UPGRADE:
        overridesFile = this.generateHelmOverride();
        response = kubernetesManager.helmUpgrade(config, taskParams().nodePrefix, overridesFile);
        flag = true;
        break;
      case UPDATE_NUM_NODES:
        int numNodes = this.getNumNodes();
        if (numNodes > 0) {
          response = kubernetesManager.updateNumNodes(config, taskParams().nodePrefix, numNodes);
        }
        break;
      case HELM_DELETE:
        kubernetesManager.helmDelete(config, taskParams().nodePrefix);
        break;
      case VOLUME_DELETE:
        kubernetesManager.deleteStorage(config, taskParams().nodePrefix);
        break;
      case NAMESPACE_DELETE:
        kubernetesManager.deleteNamespace(config, taskParams().nodePrefix);
        break;
      case POD_INFO:
        processNodeInfo();
        break;
    }
    if (response != null) {
      if (response.code != 0 && flag) {
        response = getPodError(config);
      }
      processShellResponse(response);
    }
  }

  private ShellResponse getPodError(Map<String, String> config) {
    ShellResponse response = new ShellResponse();
    response.code = -1;
    ShellResponse podResponse = kubernetesManager.getPodInfos(config, taskParams().nodePrefix);
    JsonNode podInfos = parseShellResponseAsJson(podResponse);
    boolean flag = false;
    for (JsonNode podInfo: podInfos.path("items")) {
      flag = true;
      ObjectNode pod = Json.newObject();
      JsonNode statusNode =  podInfo.path("status");
      String podStatus = statusNode.path("phase").asText();
      if (!podStatus.equals("Running")) {
        JsonNode podConditions = statusNode.path("conditions");
        ArrayList conditions = Json.fromJson(podConditions, ArrayList.class);
        Iterator iter = conditions.iterator();
        while (iter.hasNext()) {
          JsonNode info = Json.toJson(iter.next());
          String status = info.path("status").asText();
          if (status.equals("False")) {
            response.message = info.path("message").asText();
            return response;
          }
        }
      }
    }
    if (!flag) {
      response.message = "No pods even scheduled. Previous step(s) incomplete";
    }
    else {
      response.message = "Pods are ready. Services still not running";
    }
    return response;
  }

  private Map<String, String> getClusterIpForLoadBalancer() {
    Universe u = Universe.get(taskParams().universeUUID);
    PlacementInfo pi = taskParams().placementInfo;

    Map<UUID, Map<String, String>> azToConfig = PlacementInfoUtil.getConfigPerAZ(pi);
    Map<UUID, String> azToDomain = PlacementInfoUtil.getDomainPerAZ(pi);
    boolean isMultiAz = PlacementInfoUtil.isMultiAZ(Provider.get(taskParams().providerUUID));

    Map<String, String> serviceToIP = new HashMap<String, String>();

    for (Entry<UUID, Map<String, String>> entry : azToConfig.entrySet()) {
      UUID azUUID = entry.getKey();
      String azName = AvailabilityZone.get(azUUID).code;
      String regionName = AvailabilityZone.get(azUUID).region.code;
      Map<String, String> config = entry.getValue();

      String namespace = taskParams().nodePrefix;

      ShellResponse svcResponse =
          kubernetesManager.getServices(config, namespace);
      JsonNode svcInfos = parseShellResponseAsJson(svcResponse);

      for (JsonNode svcInfo: svcInfos.path("items")) {
        JsonNode serviceMetadata =  svcInfo.path("metadata");
        JsonNode serviceSpec = svcInfo.path("spec");
        String serviceType = serviceSpec.path("type").asText();
        serviceToIP.put(serviceMetadata.path("name").asText(),
                        serviceSpec.path("clusterIP").asText());
      }
    }
    return serviceToIP;
  }

  private void processNodeInfo() {
    ObjectNode pods = Json.newObject();
    Universe u = Universe.get(taskParams().universeUUID);
    UUID placementUuid = u.getUniverseDetails().getPrimaryCluster().uuid;
    PlacementInfo pi = taskParams().placementInfo;

    Map<UUID, Map<String, String>> azToConfig = PlacementInfoUtil.getConfigPerAZ(pi);
    Map<UUID, String> azToDomain = PlacementInfoUtil.getDomainPerAZ(pi);
    boolean isMultiAz = PlacementInfoUtil.isMultiAZ(Provider.get(taskParams().providerUUID));

    for (Entry<UUID, Map<String, String>> entry : azToConfig.entrySet()) {
      UUID azUUID = entry.getKey();
      String azName = AvailabilityZone.get(azUUID).code;
      String regionName = AvailabilityZone.get(azUUID).region.code;
      Map<String, String> config = entry.getValue();

      String namespace = isMultiAz ?
          String.format("%s-%s", taskParams().nodePrefix, azName) : taskParams().nodePrefix;

      ShellResponse podResponse =
          kubernetesManager.getPodInfos(config, namespace);
      JsonNode podInfos = parseShellResponseAsJson(podResponse);

      for (JsonNode podInfo: podInfos.path("items")) {
        ObjectNode pod = Json.newObject();
        JsonNode statusNode =  podInfo.path("status");
        JsonNode podSpec = podInfo.path("spec");
        pod.put("startTime", statusNode.path("startTime").asText());
        pod.put("status", statusNode.path("phase").asText());
        pod.put("az_uuid", azUUID.toString());
        pod.put("az_name", azName);
        pod.put("region_name", regionName);
        // Pod name is differentiated by the zone of deployment appended to
        // the hostname of the pod in case of multi-az.
        String podName = isMultiAz ?
            String.format("%s_%s", podSpec.path("hostname").asText(), azName) :
            podSpec.path("hostname").asText();
        pods.set(podName, pod);
      }
    }

    Universe.UniverseUpdater updater = universe -> {
      UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
      Set<NodeDetails> defaultNodes = universeDetails.nodeDetailsSet;
      NodeDetails defaultNode = defaultNodes.iterator().next();
      Set<NodeDetails> nodeDetailsSet = new HashSet<>();
      Iterator<Map.Entry<String, JsonNode>> iter = pods.fields();
      while (iter.hasNext()) {
        NodeDetails nodeDetail = defaultNode.clone();
        Map.Entry<String, JsonNode> pod = iter.next();
        String hostname = pod.getKey();

        // The namespace of the deployment in multi-az is constructed by appending
        // the zone to the universe name.
        String namespace = isMultiAz ?
            PlacementInfoUtil.getKubernetesNamespace(taskParams().nodePrefix, hostname.split("_")[1]) :
            taskParams().nodePrefix;
        JsonNode podVals = pod.getValue();
        UUID azUUID = UUID.fromString(podVals.get("az_uuid").asText());
        String domain = azToDomain.get(azUUID);
        if (hostname.contains("master")) {
          nodeDetail.isTserver = false;
          nodeDetail.isMaster = true;
          nodeDetail.cloudInfo.private_ip = String.format("%s.%s.%s.%s", hostname.split("_")[0],
              "yb-masters", namespace, domain);
        }
        else {
          nodeDetail.isMaster = false;
          nodeDetail.isTserver = true;
          nodeDetail.cloudInfo.private_ip = String.format("%s.%s.%s.%s", hostname.split("_")[0],
              "yb-tservers", namespace, domain);
        }
        if (isMultiAz) {
          nodeDetail.cloudInfo.az = podVals.get("az_name").asText();
          nodeDetail.cloudInfo.region = podVals.get("region_name").asText();
        }
        nodeDetail.azUuid = azUUID;
        nodeDetail.placementUuid = placementUuid;
        nodeDetail.state = NodeDetails.NodeState.Live;
        nodeDetail.nodeName = hostname;
        nodeDetailsSet.add(nodeDetail);
      }
      universeDetails.nodeDetailsSet = nodeDetailsSet;
      universe.setUniverseDetails(universeDetails);
    };
    saveUniverseDetails(updater);
  }

  private String nodeNameToPodName(String nodeName, boolean isMaster) {
    Matcher matcher = nodeNamePattern.matcher(nodeName);
    if (!matcher.matches()) {
      throw new RuntimeException("Invalid nodeName : " + nodeName);
    }
    int nodeIdx = Integer.parseInt(matcher.group(1));
    return String.format("%s-%d", isMaster ? "yb-master": "yb-tserver", nodeIdx - 1);
  }

  private String getPullSecret() {
    // Since the pull secret will always be the same across clusters,
    // it is always at the provider level.
    Provider provider = Provider.get(taskParams().providerUUID);
    if (provider != null) {
      Map<String, String> config = provider.getConfig();
      if (config.containsKey("KUBECONFIG_IMAGE_PULL_SECRET_NAME")) {
        return config.get("KUBECONFIG_PULL_SECRET");
      }
    }
    return null;
  }

  private int getNumNodes() {
    Provider provider = Provider.get(taskParams().providerUUID);
    if (provider != null) {
      Universe u = Universe.get(taskParams().universeUUID);
      UniverseDefinitionTaskParams.UserIntent userIntent =
          u.getUniverseDetails().getPrimaryCluster().userIntent;
      return userIntent.numNodes;
    }
    return -1;
  }

  private String generateHelmOverride() {
    Map<String, Object> overrides = new HashMap<String, Object>();
    Yaml yaml = new Yaml();

    // TODO: decide if the user want to expose all the services or just master.
    overrides = (HashMap<String, Object>) yaml.load(
        application.resourceAsStream("k8s-expose-all.yml")
    );

    Provider provider = Provider.get(taskParams().providerUUID);
    Map<String, String> config = provider.getConfig();
    Map<String, String> azConfig = new HashMap<String, String>();
    Map<String, String> regionConfig = new HashMap<String, String>();

    Universe u = Universe.get(taskParams().universeUUID);
    // TODO: This only takes into account primary cluster for Kubernetes, we need to
    // address ReadReplica clusters as well.
    UniverseDefinitionTaskParams.UserIntent userIntent =
        u.getUniverseDetails().getPrimaryCluster().userIntent;
    InstanceType instanceType = InstanceType.get(userIntent.providerType, userIntent.instanceType);
    if (instanceType == null) {
      LOG.error("Unable to fetch InstanceType for {}, {}",
          userIntent.providerType, userIntent.instanceType);
      throw new RuntimeException("Unable to fetch InstanceType " + userIntent.providerType +
          ": " +  userIntent.instanceType);
    }

    int numNodes = 0, replicationFactorZone = 0, replicationFactor = 0;
    String placementCloud = null;
    String placementRegion = null;
    String placementZone = null;
    boolean isMultiAz = (taskParams().masterAddresses != null) ? true : false;

    PlacementInfo pi = isMultiAz ? taskParams().placementInfo :
        u.getUniverseDetails().getPrimaryCluster().placementInfo;;
    if (pi != null) {
      if (pi.cloudList.size() != 0) {
        PlacementInfo.PlacementCloud cloud = pi.cloudList.get(0);
        placementCloud = cloud.code;
        if (cloud.regionList.size() != 0) {
          PlacementInfo.PlacementRegion region = cloud.regionList.get(0);
          placementRegion = region.code;
          if (region.azList.size() != 0) {
            PlacementInfo.PlacementAZ zone = region.azList.get(0);
            // TODO: wtf, why do we have AZ name but not code at this level??
            placementZone = AvailabilityZone.get(zone.uuid).code;
            numNodes = zone.numNodesInAZ;
            replicationFactorZone = zone.replicationFactor;
            replicationFactor = userIntent.replicationFactor;
            azConfig = AvailabilityZone.get(zone.uuid).getConfig();
            regionConfig = Region.get(region.uuid).getConfig();
          }
        }
      }
    }

    Map<String, Object> storageOverrides = (HashMap) overrides.getOrDefault("storage", new HashMap<>());

    Map<String, Object> tserverDiskSpecs = (HashMap) storageOverrides.getOrDefault("tserver", new HashMap<>());
    Map<String, Object> masterDiskSpecs = (HashMap) storageOverrides.getOrDefault("master", new HashMap<>());
    // Override disk count and size for just the tserver pods according to user intent.
    if (userIntent.deviceInfo != null) {
      if (userIntent.deviceInfo.numVolumes != null) {
        tserverDiskSpecs.put("count", userIntent.deviceInfo.numVolumes);
      }
      if (userIntent.deviceInfo.volumeSize != null) {
        tserverDiskSpecs.put("size", String.format("%dGi", userIntent.deviceInfo.volumeSize));
      }
      // Storage class override applies to both tserver and master.
      if (userIntent.deviceInfo.storageClass != null) {
        tserverDiskSpecs.put("storageClass", userIntent.deviceInfo.storageClass);
        masterDiskSpecs.put("storageClass", userIntent.deviceInfo.storageClass);
      }
    }

    // Storage class needs to be updated if it is overriden in the zone config.
    if (azConfig.containsKey("STORAGE_CLASS")) {
        tserverDiskSpecs.put("storageClass", azConfig.get("STORAGE_CLASS"));
        masterDiskSpecs.put("storageClass", azConfig.get("STORAGE_CLASS"));
    }

    if (isMultiAz) {
      overrides.put("masterAddresses", taskParams().masterAddresses);
      // Don't want to use the AZ tag on minikube since there are no AZ tags
      if (!environment.isDev()) {
         overrides.put("AZ", placementZone);
      }
      overrides.put("isMultiAz", true);

      overrides.put("replicas", ImmutableMap.of("tserver", numNodes,
          "master", replicationFactorZone, "totalMasters", replicationFactor));
    } else {
      overrides.put("replicas", ImmutableMap.of("tserver", numNodes,
          "master", replicationFactor));
    }

    if (!tserverDiskSpecs.isEmpty()) {
      storageOverrides.put("tserver", tserverDiskSpecs);
    }
    if (instanceType.getInstanceTypeCode().equals("cloud")) {
      masterDiskSpecs.put("size", String.format("%dGi", 3));
    }
    if (!masterDiskSpecs.isEmpty()) {
      storageOverrides.put("master", masterDiskSpecs);
    }

    // Override resource request and limit based on instance type.
    Map<String, Object> tserverResource = new HashMap<>();
    Map<String, Object> tserverLimit = new HashMap<>();
    Map<String, Object> masterResource = new HashMap<>();
    Map<String, Object> masterLimit = new HashMap<>();

    tserverResource.put("cpu", instanceType.numCores);
    tserverResource.put("memory", String.format("%.2fGi", instanceType.memSizeGB));
    tserverLimit.put("cpu", instanceType.numCores * burstVal);
    tserverLimit.put("memory", String.format("%.2fGi", instanceType.memSizeGB));

    // If the instance type is not xsmall or dev, we would bump the master resource.
    if (!instanceType.getInstanceTypeCode().equals("xsmall") &&
        !instanceType.getInstanceTypeCode().equals("dev")) {
      masterResource.put("cpu", 2);
      masterResource.put("memory", "4Gi");
      masterLimit.put("cpu", 2 * burstVal);
      masterLimit.put("memory", "4Gi");
    }
    // For testing with multiple deployments locally.
    if (instanceType.getInstanceTypeCode().equals("dev")) {
      masterResource.put("cpu", 0.5);
      masterResource.put("memory", "0.5Gi");
      masterLimit.put("cpu", 0.5);
      masterLimit.put("memory", "0.5Gi");
    }
    // For cloud deployments, we want bigger bursts in CPU if available for better performance.
    // Memory should not be burstable as memory consumption above requests can lead to pods being
    // killed if the nodes is running out of resources.
    if (instanceType.getInstanceTypeCode().equals("cloud")) {
      tserverLimit.put("cpu", instanceType.numCores * 2);
      masterResource.put("cpu", 0.3);
      masterResource.put("memory", "1Gi");
      masterLimit.put("cpu", 0.6);
      masterLimit.put("memory", "1Gi");
    }

    Map<String, Object> resourceOverrides = new HashMap();
    if (!masterResource.isEmpty() && !masterLimit.isEmpty()) {
      resourceOverrides.put("master", ImmutableMap.of("requests", masterResource,
                                                      "limits", masterLimit));
    }
    resourceOverrides.put("tserver", ImmutableMap.of("requests", tserverResource,
                                                     "limits", tserverLimit));

    overrides.put("resource", resourceOverrides);

    Map<String, Object> imageInfo = new HashMap<>();
    // Override image tag based on ybsoftwareversion.
    String imageTag = taskParams().ybSoftwareVersion == null ? userIntent.ybSoftwareVersion : taskParams().ybSoftwareVersion;
    imageInfo.put("tag", imageTag);

    // Since the image registry will remain the same across differnet clusters,
    // it will always be at the provider level.
    if (config.containsKey("KUBECONFIG_IMAGE_REGISTRY")) {
      imageInfo.put("repository", config.get("KUBECONFIG_IMAGE_REGISTRY"));
    }
    if (config.containsKey("KUBECONFIG_IMAGE_PULL_SECRET_NAME")) {
      imageInfo.put("pullSecretName", config.get("KUBECONFIG_IMAGE_PULL_SECRET_NAME"));
    }
    overrides.put("Image", imageInfo);

    if (taskParams().rootCA != null) {
      Map<String, Object> tlsInfo = new HashMap<>();
      tlsInfo.put("enabled", true);
      tlsInfo.put("insecure", u.getUniverseDetails().allowInsecure);
      Map<String, Object> rootCA = new HashMap<>();
      rootCA.put("cert", CertificateHelper.getCertPEM(taskParams().rootCA));
      rootCA.put("key", CertificateHelper.getKeyPEM(taskParams().rootCA));
      tlsInfo.put("rootCA", rootCA);
      overrides.put("tls", tlsInfo);
    }
    if (userIntent.enableIPV6) {
      overrides.put("ip_version_support", "v6_only");
    }

    Map<String, Object> partition = new HashMap<>();
    partition.put("tserver", taskParams().tserverPartition);
    partition.put("master", taskParams().masterPartition);
    overrides.put("partition", partition);

    UUID placementUuid = u.getUniverseDetails().getPrimaryCluster().uuid;
    Map<String, Object> gflagOverrides = new HashMap<>();
    // Go over master flags.
    Map<String, Object> masterOverrides = new HashMap<String, Object>(userIntent.masterGFlags);
    if (placementCloud != null && masterOverrides.get("placement_cloud") == null) {
      masterOverrides.put("placement_cloud", placementCloud);
    }
    if (placementRegion != null && masterOverrides.get("placement_region") == null) {
      masterOverrides.put("placement_region", placementRegion);
    }
    if (placementZone != null && masterOverrides.get("placement_zone") == null) {
      masterOverrides.put("placement_zone", placementZone);
    }
    if (placementUuid != null && masterOverrides.get("placement_uuid") == null) {
      masterOverrides.put("placement_uuid", placementUuid.toString());
    }
    if (!masterOverrides.isEmpty()) {
      gflagOverrides.put("master", masterOverrides);
    }
    // Go over tserver flags.
    Map<String, Object> tserverOverrides = new HashMap<String, Object>(userIntent.tserverGFlags);
    if (placementCloud != null && tserverOverrides.get("placement_cloud") == null) {
      tserverOverrides.put("placement_cloud", placementCloud);
    }
    if (placementRegion != null && tserverOverrides.get("placement_region") == null) {
      tserverOverrides.put("placement_region", placementRegion);
    }
    if (placementZone != null && tserverOverrides.get("placement_zone") == null) {
      tserverOverrides.put("placement_zone", placementZone);
    }
    if (placementUuid != null && tserverOverrides.get("placement_uuid") == null) {
      tserverOverrides.put("placement_uuid", placementUuid.toString());
    }
    if (!tserverOverrides.isEmpty()) {
      gflagOverrides.put("tserver", tserverOverrides);
    }

    if (!gflagOverrides.isEmpty()) {
      overrides.put("gflags", gflagOverrides);
    }

    if (azConfig.containsKey("KUBE_DOMAIN")) {
      overrides.put("domainName", azConfig.get("KUBE_DOMAIN"));
    }

    overrides.put("disableYsql", !userIntent.enableYSQL);

    // For now the assumption is the all deployments will have the same kind of
    // loadbalancers, so the annotations will be at the provider level.
    // TODO (Arnav): Update this to use overrides created at the provider, region or
    // zone level.
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
        overrides.putAll(annotations);
      }
    }


    Map<String, String> universeConfig = u.getConfig();
    boolean helmLegacy = Universe.HelmLegacy.valueOf(universeConfig.get(Universe.HELM2_LEGACY))
        == Universe.HelmLegacy.V2TO3;

    if (helmLegacy) {
      overrides.put("helm2Legacy", helmLegacy);
      Map<String, String> serviceToIP = getClusterIpForLoadBalancer();
      ObjectMapper mapper = new ObjectMapper();
      ArrayList<Object> serviceEndpoints = (ArrayList) overrides.get("serviceEndpoints");
      for (Object serviceEndpoint: serviceEndpoints) {
        Map<String, Object> endpoint = mapper.convertValue(serviceEndpoint, Map.class);
        String endpointName = (String) endpoint.get("name");
        if (serviceToIP.containsKey(endpointName)) {
          endpoint.put("clusterIP", serviceToIP.get(endpointName));
        }
      }
    }

    try {
      Path tempFile = Files.createTempFile(taskParams().universeUUID.toString(), ".yml");
      BufferedWriter bw = new BufferedWriter(new FileWriter(tempFile.toFile()));
      yaml.dump(overrides, bw);
      return tempFile.toAbsolutePath().toString();
    } catch (IOException e) {
      LOG.error(e.getMessage());
      throw new RuntimeException("Error writing Helm Override file!");
    }
  }
}
