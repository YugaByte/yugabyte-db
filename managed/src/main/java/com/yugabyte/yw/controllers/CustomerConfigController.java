// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import com.google.inject.Inject;

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.helpers.CustomerConfigValidator;
import com.yugabyte.yw.commissioner.Common.CloudType;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.libs.Json;
import play.mvc.Result;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

public class CustomerConfigController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(CustomerConfigController.class);

  @Inject
  private CustomerConfigValidator configValidator;

  public Result create(UUID customerUUID) {
    ObjectNode formData = (ObjectNode) request().body().asJson();
    ObjectNode errorJson = configValidator.validateFormData(formData);
    if (errorJson.size() > 0) {
      return ApiResponse.error(BAD_REQUEST, errorJson);
    }

    errorJson = configValidator.validateDataContent(formData);
    if (errorJson.size() > 0) {
      return ApiResponse.error(BAD_REQUEST, errorJson);
    }
    if (formData.get("name").asText().equals("S3") &&
      formData.get("data").get("AWS_ACCESS_KEY_ID") != null) {
      String region = Region.getByProvider(Provider.get(customerUUID, Common.CloudType.aws).uuid)
          .get(0).code.replace('-', '_').toUpperCase();
      errorJson = configValidator.validateS3DataContent(formData, region);
    }
    if (errorJson.size() > 0) {
      return ApiResponse.error(BAD_REQUEST, errorJson);
    }

    CustomerConfig customerConfig = CustomerConfig.createWithFormData(customerUUID, formData);
    Audit.createAuditEntry(ctx(), request(), formData);
    return ApiResponse.success(customerConfig);
  }

  public Result delete(UUID customerUUID, UUID configUUID) {
    CustomerConfig customerConfig = CustomerConfig.get(customerUUID, configUUID);
    if (customerConfig == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid configUUID: " + configUUID);
    }
    if (!customerConfig.delete()) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR,
          "Customer Configuration could not be deleted.");
    }
    Audit.createAuditEntry(ctx(), request());
    return ApiResponse.success("configUUID deleted");
  }

  public Result list(UUID customerUUID) {
    return ApiResponse.success(CustomerConfig.getAll(customerUUID));
  }
}
