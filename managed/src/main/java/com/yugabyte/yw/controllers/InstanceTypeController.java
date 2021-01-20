// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.typesafe.config.Config;
import com.yugabyte.yw.cloud.CloudAPI;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.models.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;

import java.util.*;
import java.util.stream.Collectors;

import static com.yugabyte.yw.commissioner.Common.CloudType.onprem;
import static java.util.function.Function.identity;
import static java.util.stream.Collectors.*;

public class InstanceTypeController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(InstanceTypeController.class);
  private final Config config;
  private final FormFactory formFactory;
  private final CloudAPI.Factory cloudAPIFactory;

  // TODO: Remove this when we have HelperMethod in place to get Config details
  @Inject
  public InstanceTypeController(Config config, FormFactory formFactory,
                                CloudAPI.Factory cloudAPIFactory) {
    this.config = config;
    this.formFactory = formFactory;
    this.cloudAPIFactory = cloudAPIFactory;
  }

  /**
   * GET endpoint for listing instance types
   *
   * @param customerUUID, UUID of customer
   * @param providerUUID, UUID of provider
   * @return JSON response with instance types
   */
  public Result list(UUID customerUUID, UUID providerUUID, List<String> zoneCodes) {
    Set<String> filterByZoneCodes = new HashSet<>(zoneCodes);
    Provider provider = Provider.get(customerUUID, providerUUID);
    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }
    Map<String, InstanceType> instanceTypesMap;
    try {
      instanceTypesMap = InstanceType.findByProvider(provider, config).stream()
        .collect(toMap(InstanceType::getInstanceTypeCode, identity()));
    } catch (Exception e) {
      LOG.error("Unable to list Instance types {}:{} in DB.", providerUUID, e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to list InstanceType");
    }

    return maybeFilterByZoneOfferings(filterByZoneCodes, provider, instanceTypesMap);
  }

  private Result maybeFilterByZoneOfferings(Set<String> filterByZoneCodes, Provider provider,
                                            Map<String, InstanceType> instanceTypesMap) {
    if (filterByZoneCodes.isEmpty()) {
      LOG.debug("No zones specified. Skipping filtering by zone.");
    } else {
      CloudAPI cloudAPI = cloudAPIFactory.get(provider.code);
      if (cloudAPI != null) {
        try {
          LOG.debug("Full list of instance types: {}. Filtering it based on offerings.",
            instanceTypesMap.keySet());
          Map<Region, Set<String>> azByRegionMap = filterByZoneCodes.stream()
            .map(AvailabilityZone::getByCode)
            .collect(groupingBy(az -> az.region, mapping(az -> az.code, toSet())));

          LOG.debug("AZs looked up from db {}", azByRegionMap);

          Map<String, Set<String>> offeringsByInstanceType =
            cloudAPI.offeredZonesByInstanceType(provider, azByRegionMap, instanceTypesMap.keySet());

          LOG.debug("Instance Type Offerings from cloud: {}.", offeringsByInstanceType);

          List<InstanceType> filteredInstanceTypes =
            offeringsByInstanceType.entrySet().stream()
              .filter(kv -> kv.getValue().size() >= filterByZoneCodes.size())
              .map(Map.Entry::getKey)
              .map(instanceTypesMap::get)
              .collect(Collectors.toList());

          LOG.info("Num instanceTypes excluded {} because they were not offered in selected AZs.",
            instanceTypesMap.size() - filteredInstanceTypes.size());

          return ApiResponse.success(filteredInstanceTypes);
        } catch (Exception exception) {
          LOG.warn("There was an error {} talking to {} cloud API or filtering instance types " +
              "based on per zone offerings for user selected zones: {}. We won't filter.",
            exception.toString(), provider.code, filterByZoneCodes);
        }
      } else {
        LOG.info("No Cloud API defined for {}. Skipping filtering by zone.", provider.code);
      }
    }
    return ApiResponse.success(instanceTypesMap.values());
  }

  /**
   * POST endpoint for creating new instance type
   *
   * @param customerUUID, UUID of customer
   * @param providerUUID, UUID of provider
   * @return JSON response of newly created instance type
   */
  public Result create(UUID customerUUID, UUID providerUUID) {
    Form<InstanceType> formData = formFactory.form(InstanceType.class).bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    Provider provider = Provider.get(customerUUID, providerUUID);
    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }

    try {
      InstanceType it = InstanceType.upsert(formData.get().getProviderCode(),
        formData.get().getInstanceTypeCode(),
        formData.get().numCores,
        formData.get().memSizeGB,
        formData.get().instanceTypeDetails);
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.rawData()));
      return ApiResponse.success(it);
    } catch (Exception e) {
      LOG.error("Unable to create instance type {}: {}", formData.rawData(), e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to create InstanceType");
    }
  }

  /**
   * DELETE endpoint for deleting instance types.
   *
   * @param customerUUID,     UUID of customer
   * @param providerUUID,     UUID of provider
   * @param instanceTypeCode, Instance TaskType code.
   * @return JSON response to denote if the delete was successful or not.
   */
  public Result delete(UUID customerUUID, UUID providerUUID, String instanceTypeCode) {
    Provider provider = Provider.get(customerUUID, providerUUID);

    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }

    try {
      InstanceType instanceType = InstanceType.get(provider.code, instanceTypeCode);
      if (instanceType == null) {
        return ApiResponse.error(BAD_REQUEST, "Instance Type not found: " + instanceTypeCode);
      }

      instanceType.setActive(false);
      instanceType.save();
      ObjectNode responseJson = Json.newObject();
      Audit.createAuditEntry(ctx(), request());
      responseJson.put("success", true);
      return ApiResponse.success(responseJson);
    } catch (Exception e) {
      LOG.error("Unable to delete instance type {}: {}", instanceTypeCode, e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR,
        "Unable to delete InstanceType: " + instanceTypeCode);
    }
  }

  /**
   * Info endpoint for getting instance type information.
   *
   * @param customerUUID,     UUID of customer
   * @param providerUUID,     UUID of provider.
   * @param instanceTypeCode, Instance type code.
   * @return JSON response with instance type information.
   */
  public Result index(UUID customerUUID, UUID providerUUID, String instanceTypeCode) {
    Provider provider = Provider.get(customerUUID, providerUUID);

    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }

    InstanceType instanceType = InstanceType.get(provider.code, instanceTypeCode);
    if (instanceType == null) {
      return ApiResponse.error(BAD_REQUEST, "Instance Type not found: " + instanceTypeCode);
    }
    // Mount paths are not persisted for non-onprem clouds, but we know the default details.
    if (!provider.code.equals(onprem.toString())) {
      instanceType.instanceTypeDetails.setDefaultMountPaths();
    }
    return ApiResponse.success(instanceType);
  }

  /**
   * Metadata endpoint for getting a list of all supported types of EBS volumes.
   *
   * @return a list of all supported types of EBS volumes.
   */
  public Result getEBSTypes() {
    return ok(Json.toJson(Arrays.stream(PublicCloudConstants.StorageType.values())
      .filter(name -> name.getCloudType().equals(Common.CloudType.aws)).toArray()));
  }

  /**
   * Metadata endpoint for getting a list of all supported types of GCP disks.
   *
   * @return a list of all supported types of GCP disks.
   */
  public Result getGCPTypes() {
    return ok(Json.toJson(Arrays.stream(PublicCloudConstants.StorageType.values())
      .filter(name -> name.getCloudType().equals(Common.CloudType.gcp)).toArray()));
  }

  /**
   * Metadata endpoint for getting a list of all supported types of AZU disks.
   *
   * @return a list of all supported types of AZU disks.
   */
  public Result getAZUTypes() {
    return ok(Json.toJson(Arrays.stream(PublicCloudConstants.StorageType.values())
      .filter(name -> name.getCloudType().equals(Common.CloudType.azu)).toArray()));
  }
}
