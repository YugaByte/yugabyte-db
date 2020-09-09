// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import java.util.Arrays;
import java.util.List;
import java.util.UUID;

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Region;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.Provider;

import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;

import static com.yugabyte.yw.commissioner.Common.CloudType.aws;
import static com.yugabyte.yw.commissioner.Common.CloudType.onprem;

public class InstanceTypeController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(InstanceTypeController.class);

  @Inject
  FormFactory formFactory;

  @Inject
  CloudQueryHelper cloudQueryHelper;

  /**
   * GET endpoint for listing instance types
   *
   * @param customerUUID, UUID of customer
   * @param providerUUID, UUID of provider
   * @return JSON response with instance types's
   */
  public Result list(UUID customerUUID, UUID providerUUID) {
    List<InstanceType> instanceTypeList = null;
    Provider provider = Provider.get(customerUUID, providerUUID);

    if (provider == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
    }

    try {
      instanceTypeList = InstanceType.findByProvider(provider);
    } catch (Exception e) {
      LOG.error("Unable to list Instance types {}:{}", providerUUID, e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to list InstanceType");
    }
    return ApiResponse.success(instanceTypeList);
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
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return ApiResponse.success(it);
    } catch (Exception e) {
      LOG.error("Unable to create instance type {}: {}", formData.data(), e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to create InstanceType" );
    }
  }

  /**
   * DELETE endpoint for deleting instance types.
   * @param customerUUID, UUID of customer
   * @param providerUUID, UUID of provider
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
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to delete InstanceType: " + instanceTypeCode);
    }
  }

  /**
   * Info endpoint for getting instance type information.
   * @param customerUUID, UUID of customer
   * @param providerUUID, UUID of provider.
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
   * @return a list of all supported types of EBS volumes.
   */
  public Result getEBSTypes() {
    return ok(Json.toJson(Arrays.stream(PublicCloudConstants.StorageType.values())
            .filter(name->name.getCloudType().equals(Common.CloudType.aws)).toArray()));
  }

  /**
   * Metadata endpoint for getting a list of all supported types of GCP disks.
   * @return a list of all supported types of GCP disks.
   */
  public Result getGCPTypes() {
    return ok(Json.toJson(Arrays.stream(PublicCloudConstants.StorageType.values())
            .filter(name->name.getCloudType().equals(Common.CloudType.gcp)).toArray()));
  }

  /**
   * Metadata endpoint for getting a list of all supported types of AZU disks.
   * @return a list of all supported types of AZU disks.
   */
  public Result getAZUTypes() {
    return ok(Json.toJson(Arrays.stream(PublicCloudConstants.StorageType.values())
            .filter(name->name.getCloudType().equals(Common.CloudType.azu)).toArray()));
  }
}
