// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.yugabyte.yw.common.ApiResponse;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.forms.AvailabilityZoneFormData;
import com.yugabyte.yw.forms.AvailabilityZoneFormData.AvailabilityZoneData;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;

import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;

public class AvailabilityZoneController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(AvailabilityZoneController.class);

  @Inject
  FormFactory formFactory;

  /**
   * GET endpoint for listing availability zones
   * @return JSON response with availability zone's
   */
  public Result list(UUID customerUUID, UUID providerUUID, UUID regionUUID) {
    Region region = Region.get(customerUUID, providerUUID, regionUUID);

    if (region == null) {
      LOG.warn("PlacementRegion not found, cloud provider: " + providerUUID + ", region: " + regionUUID);
      return ApiResponse.error(BAD_REQUEST, "Invalid PlacementRegion/Provider UUID");
    }

    try {
      List<AvailabilityZone>  zoneList = AvailabilityZone.find.query().where()
          .eq("region", region)
          .findList();
      return ApiResponse.success(zoneList);
    } catch (Exception e) {
      LOG.error(e.getMessage());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to fetch zones");
    }
  }

  /**
   * POST endpoint for creating new region(s)
   * @return JSON response of newly created region(s)
   */
  public Result create(UUID customerUUID, UUID providerUUID, UUID regionUUID) {
    Form<AvailabilityZoneFormData> formData = formFactory.form(AvailabilityZoneFormData.class)
                                                         .bindFromRequest();
    Region region = Region.get(customerUUID, providerUUID, regionUUID);

    if (region == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid PlacementRegion/Provider UUID");
    }

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    List<AvailabilityZoneData> azDataList = formData.get().availabilityZones;
    Map<String, AvailabilityZone> availabilityZones = new HashMap<>();
    try {
      for (AvailabilityZoneData azData : azDataList) {
        AvailabilityZone az = AvailabilityZone.create(region, azData.code, azData.name, azData.subnet);
        availabilityZones.put(az.code, az);
      }
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return ApiResponse.success(availabilityZones);
    } catch (Exception e) {
      LOG.error(e.getMessage());
      AvailabilityZoneData failedAz = azDataList.get(availabilityZones.size());
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to create zone: " + failedAz.code);
    }
  }

  /**
   * DELETE endpoint for deleting a existing availability zone.
   * @param providerUUID Provider UUID
   * @param regionUUID Region UUID
   * @param azUUID AvailabilityZone UUID
   * @return JSON response on whether or not delete region was successful or not.
   */
  public Result delete(UUID customerUUID, UUID providerUUID, UUID regionUUID, UUID azUUID) {
    Region region = Region.get(customerUUID, providerUUID, regionUUID);

    if (region == null) {
      ApiResponse.error(BAD_REQUEST, "Invalid PlacementRegion/Provider UUID");
    }

    AvailabilityZone az = AvailabilityZone.find.query().where().
            idEq(azUUID).eq("region_uuid", regionUUID).findOne();

    if (az == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Region/AZ UUID:" + azUUID);
    }

    try {
      az.setActiveFlag(false);
      az.update();
      ObjectNode responseJson = Json.newObject();
      Audit.createAuditEntry(ctx(), request());
      responseJson.put("success", true);
      return ApiResponse.success(responseJson);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to flag AZ UUID as deleted: " + azUUID);
    }
  }
}
