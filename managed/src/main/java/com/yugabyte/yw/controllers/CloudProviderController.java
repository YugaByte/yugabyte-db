// Copyright (c) Yugabyte, Inc.
package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.yugabyte.yw.cloud.AWSInitializer;
import com.yugabyte.yw.cloud.GCPInitializer;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.CloudBootstrap;
import com.yugabyte.yw.commissioner.tasks.CloudCleanup;
import com.yugabyte.yw.commissioner.tasks.params.CloudTaskParams;
import com.yugabyte.yw.commissioner.tasks.params.KubernetesClusterInitParams;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.common.ConfigHelper;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.CloudProviderFormData;
import com.yugabyte.yw.forms.CloudBootstrapFormData;
import com.yugabyte.yw.forms.KubernetesProviderFormData;
import com.yugabyte.yw.forms.KubernetesProviderFormData.RegionData;
import com.yugabyte.yw.forms.KubernetesProviderFormData.RegionData.ZoneData;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.models.helpers.TaskType;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import com.google.inject.Inject;

import play.api.Play;
import play.data.Form;
import play.data.FormFactory;
import play.Environment;
import play.libs.Json;
import play.mvc.Result;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import javax.persistence.PersistenceException;

import static com.yugabyte.yw.common.ConfigHelper.ConfigType.DockerInstanceTypeMetadata;
import static com.yugabyte.yw.common.ConfigHelper.ConfigType.DockerRegionMetadata;
import static com.yugabyte.yw.models.helpers.CommonUtils.DEFAULT_YB_HOME_DIR;

public class CloudProviderController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(CloudProviderController.class);


  private static JsonNode KUBERNETES_CLOUD_INSTANCE_TYPE = Json.parse(
      "{\"instanceTypeCode\": \"cloud\", \"numCores\": 0.5, \"memSizeGB\": 1.5}");
  private static JsonNode KUBERNETES_DEV_INSTANCE_TYPE = Json.parse(
      "{\"instanceTypeCode\": \"dev\", \"numCores\": 0.5, \"memSizeGB\": 0.5}");
  private static JsonNode KUBERNETES_INSTANCE_TYPES = Json.parse("[" +
      "{\"instanceTypeCode\": \"xsmall\", \"numCores\": 2, \"memSizeGB\": 4}," +
      "{\"instanceTypeCode\": \"small\", \"numCores\": 4, \"memSizeGB\": 7.5}," +
      "{\"instanceTypeCode\": \"medium\", \"numCores\": 8, \"memSizeGB\": 15}," +
      "{\"instanceTypeCode\": \"large\", \"numCores\": 16, \"memSizeGB\": 15}," +
      "{\"instanceTypeCode\": \"xlarge\", \"numCores\": 32, \"memSizeGB\": 30}]");

  @Inject
  FormFactory formFactory;

  @Inject
  AWSInitializer awsInitializer;

  @Inject
  GCPInitializer gcpInitializer;

  @Inject
  Commissioner commissioner;

  @Inject
  ConfigHelper configHelper;

  @Inject
  AccessManager accessManager;

  @Inject
  DnsManager dnsManager;

  @Inject
  private play.Environment environment;

  /**
   * GET endpoint for listing providers
   * @return JSON response with provider's
   */
  public Result list(UUID customerUUID) {
    List<Provider> providerList = Provider.getAll(customerUUID);
    ArrayNode providers = Json.newArray();
    providerList.forEach((provider) -> {
      ObjectNode providerJson = (ObjectNode) Json.toJson(provider);
      providerJson.set("config", provider.getMaskedConfig());
      providers.add(providerJson);
    });
    return ApiResponse.success(providers);
  }

  // This endpoint we are using only for deleting provider for integration test purpose. our
  // UI should call cleanup endpoint.
  public Result delete(UUID customerUUID, UUID providerUUID) {
    Provider provider = Provider.get(customerUUID, providerUUID);

    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }

    Customer customer = Customer.get(customerUUID);
    if (customer.getUniversesForProvider(providerUUID).size() > 0) {
      return ApiResponse.error(BAD_REQUEST, "Cannot delete Provider with Universes");
    }

    // TODO: move this to task framework
    try {
      for (AccessKey accessKey : AccessKey.getAll(providerUUID)) {
        if (!accessKey.getKeyInfo().provisionInstanceScript.isEmpty()) {
          new File(accessKey.getKeyInfo().provisionInstanceScript).delete();
        }
        for (Region region : Region.getByProvider(providerUUID)) {
          accessManager.deleteKey(region.uuid, accessKey.getKeyCode());
        }
        accessKey.delete();
      }
      NodeInstance.deleteByProvider(providerUUID);
      provider.delete();
      Audit.createAuditEntry(ctx(), request());
      return ApiResponse.success("Deleted provider: " + providerUUID);
    } catch (RuntimeException e) {
      LOG.error(e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to delete provider: " + providerUUID);
    }
  }

  /**
   * POST endpoint for creating new providers
   * @return JSON response of newly created provider
   */
  public Result create(UUID customerUUID) throws IOException {
    Form<CloudProviderFormData> formData = formFactory.form(CloudProviderFormData.class).bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    Common.CloudType providerCode = formData.get().code;
    if (!providerCode.equals(Common.CloudType.kubernetes)) {
      Provider provider = Provider.get(customerUUID, providerCode);
      if (provider != null) {
        return ApiResponse.error(BAD_REQUEST, "Duplicate provider code: " + providerCode);
      }
    }

    // Since the Map<String, String> doesn't get parsed, so for now we would just
    // parse it from the requestBody
    JsonNode requestBody = request().body().asJson();
    Map<String, String> config = processConfig(requestBody, providerCode);

    try {
      Provider provider = Provider.create(customerUUID, providerCode, formData.get().name, config);
      if (!config.isEmpty()) {
        switch (provider.code) {
          case "aws":
            String hostedZoneId = provider.getAwsHostedZoneId();
            if (hostedZoneId != null) {
              return validateAwsHostedZoneUpdate(provider, hostedZoneId);
            }
            break;
          case "gcp":
            updateGCPConfig(provider, config);
            break;
          case "kubernetes":
            updateKubeConfig(provider, config, false);
            try {
              createKubernetesInstanceTypes(provider, customerUUID);
            } catch (javax.persistence.PersistenceException ex) {
              // TODO: make instance types more multi-tenant friendly...
            }
            break;
        }
      }
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return ApiResponse.success(provider);
    } catch (RuntimeException e) {
      String errorMsg = "Unable to create provider: " + providerCode;
      LOG.error(errorMsg, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, errorMsg);
    }
  }

  // For creating the a multi-cluster kubernetes provider.
  public Result createKubernetes(UUID customerUUID) throws IOException {
    JsonNode requestBody = request().body().asJson();
    ObjectMapper mapper = new ObjectMapper();
    KubernetesProviderFormData formData;
    try {
      formData = mapper.treeToValue(requestBody, KubernetesProviderFormData.class);
    } catch (RuntimeException e) {
      return ApiResponse.error(BAD_REQUEST, "Invalid JSON");
    }

    Common.CloudType providerCode = formData.code;
    if (!providerCode.equals(Common.CloudType.kubernetes)) {
      return ApiResponse.error(BAD_REQUEST, "API for only kubernetes provider creation: " + providerCode);
    }

    boolean hasConfig = formData.config.containsKey("KUBECONFIG_NAME");
    if (formData.regionList.isEmpty()) {
      return ApiResponse.error(BAD_REQUEST, "Need regions in provider");
    }
    for (RegionData rd : formData.regionList) {
      if (rd.config != null) {
        if (rd.config.containsKey("KUBECONFIG_NAME")) {
          if (hasConfig) {
            return ApiResponse.error(BAD_REQUEST, "Kubeconfig can't be at two levels");
          } else {
            hasConfig = true;
          }
        }
      }
      if (rd.zoneList.isEmpty()) {
        return ApiResponse.error(BAD_REQUEST, "No zone provided in region");
      }
      for (ZoneData zd : rd.zoneList) {
        if (zd.config != null) {
          if (zd.config.containsKey("KUBECONFIG_NAME")) {
            if (hasConfig) {
              return ApiResponse.error(BAD_REQUEST, "Kubeconfig can't be at two levels");
            }
          } else if (!hasConfig) {
            return ApiResponse.error(BAD_REQUEST, "No Kubeconfig found for zone(s)");
          }
        }
      }
      hasConfig = formData.config.containsKey("KUBECONFIG_NAME");
    }

    Provider provider = null;
    try {
      Map<String, String> config = formData.config;
      provider = Provider.create(customerUUID, providerCode, formData.name);
      boolean isConfigInProvider = updateKubeConfig(provider, config, false);
      if (isConfigInProvider) {
      }
      List<RegionData> regionList = formData.regionList;
      for (RegionData rd : regionList) {
        Map<String, String> regionConfig = rd.config;
        Region region = Region.create(provider, rd.code, rd.name, null, rd.latitude, rd.longitude);
        boolean isConfigInRegion = updateKubeConfig(provider, region, regionConfig, false);
        if (isConfigInRegion) {
        }
        for (ZoneData zd : rd.zoneList) {
          Map<String, String> zoneConfig = zd.config;
          AvailabilityZone az = AvailabilityZone.create(region, zd.code, zd.name, null);
          boolean isConfigInZone = updateKubeConfig(provider, region, az, zoneConfig, false);
          if (isConfigInZone) {
          }
        }
      }
      try {
        createKubernetesInstanceTypes(provider, customerUUID);
      } catch (javax.persistence.PersistenceException ex) {
        provider.delete();
        return ApiResponse.error(INTERNAL_SERVER_ERROR, "Couldn't create instance types");
        // TODO: make instance types more multi-tenant friendly...
      }
      Audit.createAuditEntry(ctx(), request(), requestBody);
      return ApiResponse.success(provider);
    } catch (RuntimeException e) {
      if (provider != null) {
        provider.delete();
      }
      String errorMsg = "Unable to create provider: " + providerCode;
      LOG.error(errorMsg, e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, errorMsg);
    }
  }

  private boolean updateKubeConfig(Provider provider, Map<String, String> config, boolean edit) {
    return updateKubeConfig(provider, null, config, edit);
  }

  private boolean updateKubeConfig(Provider provider, Region region, Map<String, String> config, boolean edit) {
    return updateKubeConfig(provider, region, null, config, edit);
  }

  private boolean updateKubeConfig(Provider provider, Region region, AvailabilityZone zone, Map<String, String> config, boolean edit) {
    String kubeConfigFile = null;
    String pullSecretFile = null;

    if (config == null) {
      return false;
    }

    String path = provider.uuid.toString();
    if (region != null) {
      path = path + "/" + region.uuid.toString();
      if (zone != null) {
        path = path + "/" + zone.uuid.toString();
      }
    }
    boolean hasKubeConfig = config.containsKey("KUBECONFIG_NAME");
    if (hasKubeConfig) {
      try {
        kubeConfigFile = accessManager.createKubernetesConfig(
            path, config, edit
        );

        // Remove the kubeconfig file related configs from provider config.
        config.remove("KUBECONFIG_NAME");
        config.remove("KUBECONFIG_CONTENT");

        if (kubeConfigFile != null) {
          config.put("KUBECONFIG", kubeConfigFile);
        }

      } catch (IOException e) {
        LOG.error(e.getMessage());
        throw new RuntimeException("Unable to create Kube Config file.");
      }
    }

    if (region == null) {
      if (config.containsKey("KUBECONFIG_PULL_SECRET_NAME")) {
        if (config.get("KUBECONFIG_PULL_SECRET_NAME") != null) {
          try {
            pullSecretFile = accessManager.createPullSecret(
                provider.uuid, config, edit
            );
          } catch (IOException e) {
            LOG.error(e.getMessage());
            throw new RuntimeException("Unable to create Pull Secret file.");
          }
        }
      }
      config.remove("KUBECONFIG_PULL_SECRET_NAME");
      config.remove("KUBECONFIG_PULL_SECRET_CONTENT");
      if (pullSecretFile != null) {
        config.put("KUBECONFIG_PULL_SECRET", pullSecretFile);
      }

      provider.setConfig(config);
    } else if (zone == null) {
      region.setConfig(config);
    } else {
      zone.setConfig(config);
    }
    return hasKubeConfig;
  }

  private void updateGCPConfig(Provider provider, Map<String, String> config) {
    // Remove the key to avoid generating a credentials file unnecessarily.
    config.remove("GCE_HOST_PROJECT");
    // If we were not given a config file, then no need to do anything here.
    if (config.isEmpty()) {
      return;
    }

    String gcpCredentialsFile = null;
    try {
      gcpCredentialsFile = accessManager.createCredentialsFile(
          provider.uuid, Json.toJson(config));
    } catch (IOException e) {
      LOG.error(e.getMessage());
      throw new RuntimeException("Unable to create GCP Credentials file.");
    }

    Map<String, String> newConfig = new HashMap<String, String>();
    if (config.get("project_id") != null) {
      newConfig.put("GCE_PROJECT", config.get("project_id"));
    }
    if (config.get("client_email") != null) {
      newConfig.put("GCE_EMAIL", config.get("client_email"));
    }
    if (gcpCredentialsFile != null) {
      newConfig.put("GOOGLE_APPLICATION_CREDENTIALS", gcpCredentialsFile);
    }
    provider.setConfig(newConfig);
    provider.save();
  }

  private void createKubernetesInstanceTypes(Provider provider, UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    KUBERNETES_INSTANCE_TYPES.forEach((instanceType -> {
      InstanceType.InstanceTypeDetails idt = new InstanceType.InstanceTypeDetails();
      idt.setVolumeDetailsList(1, 100, InstanceType.VolumeType.SSD);
      InstanceType.upsert(provider.code,
          instanceType.get("instanceTypeCode").asText(),
          instanceType.get("numCores").asDouble(),
          instanceType.get("memSizeGB").asDouble(),
          idt
      );
    }));
    if (environment.isDev()) {
      InstanceType.InstanceTypeDetails idt = new InstanceType.InstanceTypeDetails();
      idt.setVolumeDetailsList(1, 100, InstanceType.VolumeType.SSD);
      InstanceType.upsert(provider.code,
          KUBERNETES_DEV_INSTANCE_TYPE.get("instanceTypeCode").asText(),
          KUBERNETES_DEV_INSTANCE_TYPE.get("numCores").asDouble(),
          KUBERNETES_DEV_INSTANCE_TYPE.get("memSizeGB").asDouble(),
          idt
      );
    }
    if (customer.code.equals("cloud")) {
      InstanceType.InstanceTypeDetails idt = new InstanceType.InstanceTypeDetails();
      idt.setVolumeDetailsList(1, 5, InstanceType.VolumeType.SSD);
      InstanceType.upsert(provider.code,
          KUBERNETES_CLOUD_INSTANCE_TYPE.get("instanceTypeCode").asText(),
          KUBERNETES_CLOUD_INSTANCE_TYPE.get("numCores").asDouble(),
          KUBERNETES_CLOUD_INSTANCE_TYPE.get("memSizeGB").asDouble(),
          idt
      );
    }
  }

  // TODO: This is temporary endpoint, so we can setup docker, will move this
  // to standard provider bootstrap route soon.
  public Result setupDocker(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      ApiResponse.error(BAD_REQUEST, "Invalid Customer Context.");
    }
    Provider provider = Provider.get(customerUUID, Common.CloudType.docker);
    if (provider != null) {
      return ApiResponse.success(provider);
    }

    try {
      Provider newProvider = Provider.create(customerUUID, Common.CloudType.docker, "Docker");
      Map<String, Object> regionMetadata = configHelper.getConfig(DockerRegionMetadata);
      regionMetadata.forEach((regionCode, metadata) -> {
        Region region = Region.createWithMetadata(newProvider, regionCode, Json.toJson(metadata));
        Arrays.asList("a", "b", "c").forEach((zoneSuffix) -> {
          String zoneName = regionCode + zoneSuffix;
          AvailabilityZone.create(region, zoneName, zoneName, "yugabyte-bridge");
        });
      });
      Map<String, Object> instanceTypeMetadata = configHelper.getConfig(DockerInstanceTypeMetadata);
      instanceTypeMetadata.forEach((itCode, metadata) ->
          InstanceType.createWithMetadata(newProvider, itCode, Json.toJson(metadata)));
      Audit.createAuditEntry(ctx(), request());
      return ApiResponse.success(newProvider);
    } catch (Exception e) {
      LOG.error(e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to create docker provider");
    }
  }

  public Result initialize(UUID customerUUID, UUID providerUUID) {
    Provider provider = Provider.get(customerUUID, providerUUID);
    if (provider == null) {
      ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }
    if (provider.code.equals("gcp")) {
      return gcpInitializer.initialize(customerUUID, providerUUID);
    }
    return awsInitializer.initialize(customerUUID, providerUUID);
  }

  public Result bootstrap(UUID customerUUID, UUID providerUUID) {
    // TODO(bogdan): Need to manually parse maps, maybe add try/catch on parse?
    JsonNode requestBody = request().body().asJson();
    CloudBootstrap.Params taskParams = Json.fromJson(requestBody, CloudBootstrap.Params.class);

    Provider provider = Provider.get(customerUUID, providerUUID);
    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID:" + providerUUID);
    }
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      ApiResponse.error(BAD_REQUEST, "Invalid Customer Context.");
    }
    // Set the top-level provider info.
    taskParams.providerUUID = providerUUID;
    if (taskParams.destVpcId != null && !taskParams.destVpcId.isEmpty()) {
      if (provider.code.equals("gcp")) {
        // We need to save the destVpcId into the provider config, because we'll need it during
        // instance creation. Technically, we could make it a ybcloud parameter, but we'd still need to
        // store it somewhere and the config is the easiest place to put it. As such, since all the
        // config is loaded up as env vars anyway, might as well use in in devops like that...
        Map<String, String> config = provider.getConfig();
        config.put("CUSTOM_GCE_NETWORK", taskParams.destVpcId);
        provider.setConfig(config);
        provider.save();
      } else if (provider.code.equals("aws")) {
        taskParams.destVpcId = null;
      }
    }

    // If the regionList is still empty by here, then we need to list the regions available.
    if (taskParams.perRegionMetadata == null) {
      taskParams.perRegionMetadata = new HashMap<>();
    }
    if (taskParams.perRegionMetadata.isEmpty()) {
      CloudQueryHelper queryHelper = Play.current().injector().instanceOf(CloudQueryHelper.class);
      JsonNode regionInfo = queryHelper.getRegions(provider.uuid);
      if (regionInfo instanceof ArrayNode) {
        ArrayNode regionListArray = (ArrayNode) regionInfo;
        for (JsonNode region : regionListArray) {
          taskParams.perRegionMetadata.put(
              region.asText(), new CloudBootstrap.Params.PerRegionMetadata());
        }
      }
    }

    UUID taskUUID = commissioner.submit(TaskType.CloudBootstrap, taskParams);
    CustomerTask.create(customer,
      providerUUID,
      taskUUID,
      CustomerTask.TargetType.Provider,
      CustomerTask.TaskType.Create,
      provider.name);

    ObjectNode resultNode = Json.newObject();
    resultNode.put("taskUUID", taskUUID.toString());
    Audit.createAuditEntry(ctx(), request(), requestBody, taskUUID);
    return ApiResponse.success(resultNode);
  }

  public Result cleanup(UUID customerUUID, UUID providerUUID) {
    // TODO(bogdan): this is not currently used, be careful about the API...
    Form<CloudBootstrapFormData> formData = formFactory.form(CloudBootstrapFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    Provider provider = Provider.get(customerUUID, providerUUID);
    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID:" + providerUUID);
    } else {
      return ApiResponse.error(BAD_REQUEST, "Internal API issues");
    }

    /*
    CloudCleanup.Params taskParams = new CloudCleanup.Params();
    taskParams.providerUUID = providerUUID;
    taskParams.regionList = formData.get().regionList;
    UUID taskUUID = commissioner.submit(TaskType.CloudCleanup, taskParams);

    // TODO: add customer task
    ObjectNode resultNode = Json.newObject();
    resultNode.put("taskUUID", taskUUID.toString());
    return ApiResponse.success(resultNode);
    */
  }

  public Result edit(UUID customerUUID, UUID providerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      ApiResponse.error(BAD_REQUEST, "Invalid Customer Context.");
    }
    JsonNode formData =  request().body().asJson();
    Provider provider = Provider.get(customerUUID, providerUUID);
    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }

    if (provider.code.equals("aws")) {
      String hostedZoneId = formData.get("hostedZoneId").asText();
      if (hostedZoneId == null || hostedZoneId.length() == 0) {
        return ApiResponse.error(BAD_REQUEST, "Required field hosted zone id");
      }
      return validateAwsHostedZoneUpdate(provider, hostedZoneId);
    } else if (provider.code.equals("kubernetes")) {
      Map<String, String> config = processConfig(formData, Common.CloudType.kubernetes);
      if (config != null) {
        updateKubeConfig(provider, config, true);
        Audit.createAuditEntry(ctx(), request(), formData);
        return ApiResponse.success(provider);
      }
      else {
        return ApiResponse.error(INTERNAL_SERVER_ERROR, "Could not parse config");
      }
    } else {
      return ApiResponse.error(BAD_REQUEST, "Expected aws/k8s, but found providers with code: " + provider.code);
    }
  }

  private Result validateAwsHostedZoneUpdate(Provider provider, String hostedZoneId) {
    // TODO: do we have a good abstraction to inspect this AND know that it's an error outside?
    ShellResponse response = dnsManager.listDnsRecord(
        provider.uuid, hostedZoneId);
    if (response.code != 0) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Invalid devops API response: " + response.message);
    }
    // The result returned from devops should be of the form
    // {
    //    "name": "dev.yugabyte.com."
    // }
    JsonNode hostedZoneData = Json.parse(response.message);
    hostedZoneData = hostedZoneData.get("name");
    if (hostedZoneData == null || hostedZoneData.asText().isEmpty()) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Invalid devops API response: " + response.message);
    }
    try {
      provider.updateHostedZone(hostedZoneId, hostedZoneData.asText());
    } catch (RuntimeException e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
    Audit.createAuditEntry(ctx(), request());
    return ApiResponse.success(provider);
  }

  private Map<String, String> processConfig(JsonNode requestBody, Common.CloudType providerCode) {
    Map<String, String> config = new HashMap<String,String>();
    JsonNode configNode = requestBody.get("config");
    // Confirm we had a "config" key and it was not null.
    if (configNode != null && !configNode.isNull()) {
      if (providerCode.equals(Common.CloudType.gcp)) {
        // We may receive a config file, or we may be asked to use the local service account.
        // Default to using config file.
        boolean shouldUseHostCredentials = configNode.has("use_host_credentials")
                                             && configNode.get("use_host_credentials").asBoolean();
        JsonNode contents = configNode.get("config_file_contents");
        if (!shouldUseHostCredentials && contents != null) {
          config = Json.fromJson(contents, Map.class);
        }

        contents = configNode.get("host_project_id");
        if (contents != null && !contents.textValue().isEmpty()) {
          config.put("GCE_HOST_PROJECT", contents.textValue());
        }

        contents = configNode.get("YB_FIREWALL_TAGS");
        if (contents != null && !contents.textValue().isEmpty()) {
          config.put("YB_FIREWALL_TAGS", contents.textValue());
        }
      } else {
        config = Json.fromJson(configNode, Map.class);
      }
    }
    return config;
  }
}
