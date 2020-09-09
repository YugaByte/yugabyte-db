/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */


package com.yugabyte.yw.cloud;

import java.time.Instant;
import java.time.format.DateTimeFormatter;
import java.util.Iterator;
import java.util.List;
import java.util.UUID;

import com.google.inject.Singleton;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.PriceComponent;
import com.yugabyte.yw.models.PriceComponent.PriceDetails;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.InstanceType.InstanceTypeDetails;
import com.yugabyte.yw.models.InstanceType.VolumeType;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;

import play.libs.Json;
import play.mvc.Result;

import static play.mvc.Http.Status.INTERNAL_SERVER_ERROR;
import static play.mvc.Http.Status.BAD_REQUEST;

@Singleton
public class AZUInitializer extends AbstractInitializer {

  private Provider provider;

   /**
   * Entry point to initialize AZU. This will create the various InstanceTypes and their
   * corresponding PriceComponents per Region for AZU.
   *
   * @param customerUUID UUID of the Customer.
   * @param providerUUID UUID of the Customer's configured AZU.
   * @return A response result that can be returned to the user to indicate success/failure.
   */
  @Override
  public Result initialize(UUID customerUUID, UUID providerUUID) {
    try {
      // Validate input
      Customer customer = Customer.get(customerUUID);
      if (customer == null) {
        return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
      }
      provider = Provider.get(customerUUID, providerUUID);
      if (provider == null) {
        return ApiResponse.error(BAD_REQUEST, "Invalid Provider UUID: " + providerUUID);
      }

      List<Region> regionList = Region.fetchValidRegions(customerUUID, providerUUID, 0);
      Common.CloudType cloudType = Common.CloudType.valueOf(provider.code);

      JsonNode instanceTypes = cloudQueryHelper.getInstanceTypes(
        regionList, Json.stringify(Json.toJson(provider.getCloudParams())));

      Iterator<String> itr = instanceTypes.fieldNames();

      while(itr.hasNext()) {

        String instanceTypeCode = itr.next();
        JsonNode instanceTypeToDetailsMap = instanceTypes.get(instanceTypeCode);

        InstanceTypeDetails instanceTypeDetails = InstanceTypeDetails.createAZUDefault();

        InstanceType.upsert(provider.code,
                            instanceTypeCode,
                            instanceTypeToDetailsMap.get("numCores").asInt(),
                            instanceTypeToDetailsMap.get("memSizeGb").asDouble(),
                            instanceTypeDetails
        );

      }

    }
    catch (Exception e) {
      LOG.error("Azure Initialize failed", e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }

    return ApiResponse.success("Azure Initialized");

  }
}
