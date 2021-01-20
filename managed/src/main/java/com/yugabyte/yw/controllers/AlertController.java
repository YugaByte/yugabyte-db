// Copyright 2020 YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.yugabyte.yw.controllers;

import com.google.inject.Inject;

import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.forms.AlertDefinitionFormData;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.forms.AlertFormData;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.mvc.Result;
import play.data.Form;
import play.data.FormFactory;
import com.fasterxml.jackson.databind.node.ArrayNode;
import play.libs.Json;

import java.util.List;
import java.util.UUID;

public class AlertController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(AlertController.class);

  @Inject
  FormFactory formFactory;

  /**
   * Lists alerts for given customer.
   */
  public Result list(UUID customerUUID) {
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    ArrayNode alerts = Json.newArray();
    for (Alert alert: Alert.list(customerUUID)) {
        alerts.add(alert.toJson());
    }

    return ok(alerts);
  }

  public Result listActive(UUID customerUUID) {
    try {
      if (Customer.get(customerUUID) == null) {
        return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
      }

      ArrayNode alerts = Json.newArray();
      for (Alert alert: Alert.listActive(customerUUID)) {
        alerts.add(alert.toJson());
      }

      return ok(alerts);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  /**
   * Upserts alert of specified errCode with new message and createTime. Creates alert if needed.
   * This may only be used to create or update alerts that have 1 or fewer entries in the DB.
   * e.g. Creating two different alerts with errCode='LOW_ULIMITS' and then calling this would
   * error. Creating one alert with errCode=`LOW_ULIMITS` and then calling update would change
   * the previously created alert.
   */
  public Result upsert(UUID customerUUID) {
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    Form<AlertFormData> formData = formFactory.form(AlertFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }

    AlertFormData data = formData.get();
    List<Alert> alerts = Alert.list(customerUUID, data.errCode);
    if (alerts.size() > 1) {
      return ApiResponse.error(CONFLICT,
        "May only update alerts that have been created once."
        + "Use POST instead to create new alert.");
    } else if (alerts.size() == 1) {
      alerts.get(0).update(data.message);
    } else {
      Alert alert = Alert.create(customerUUID, data.errCode, data.type, data.message);
    }
    Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
    return ok();
  }

  /**
   * Creates new alert.
   */
  public Result create(UUID customerUUID) {
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    Form<AlertFormData> formData = formFactory.form(AlertFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    AlertFormData data = formData.get();
    Alert alert = Alert.create(customerUUID, data.errCode, data.type, data.message);
    Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
    return ok();
  }

  public Result createDefinition(UUID customerUUID, UUID universeUUID) {
    try {
      if (Customer.get(customerUUID) == null) {
        return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
      }

      Universe universe = Universe.get(universeUUID);

      Form<AlertDefinitionFormData> formData =
        formFactory.form(AlertDefinitionFormData.class).bindFromRequest();
      if (formData.hasErrors()) {
        return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
      }

      AlertDefinitionFormData data = formData.get();

      AlertDefinition definition = AlertDefinition.create(
        customerUUID,
        universeUUID,
        data.name,
        data.template.buildQuery(universe.getUniverseDetails().nodePrefix, data.value),
        data.isActive
      );

      return ok(Json.toJson(definition));
    } catch (Exception e) {
      LOG.error("Error creating alert definition", e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }

  public Result getAlertDefinition(UUID customerUUID, UUID universeUUID, String name) {
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    AlertDefinition definition = AlertDefinition.get(customerUUID, universeUUID, name);

    if (definition == null) {
      return ApiResponse.error(BAD_REQUEST, "Could not find Alert Definition");
    }

    return ok(Json.toJson(definition));
  }

  public Result updateAlertDefinition(UUID customerUUID, UUID alertDefinitionUUID) {
    try {
      if (Customer.get(customerUUID) == null) {
        return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
      }

      AlertDefinition definition = AlertDefinition.get(alertDefinitionUUID);
      if (definition == null) {
        return ApiResponse.error(BAD_REQUEST, "Invalid Alert Definition UUID: " + alertDefinitionUUID);
      }

      Universe universe = Universe.get(definition.universeUUID);

      Form<AlertDefinitionFormData> formData =
        formFactory.form(AlertDefinitionFormData.class).bindFromRequest();
      if (formData.hasErrors()) {
        return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
      }

      AlertDefinitionFormData data = formData.get();
      definition = AlertDefinition.update(
        definition.uuid,
        data.template.buildQuery(universe.getUniverseDetails().nodePrefix, data.value),
        data.isActive
      );

      return ok(Json.toJson(definition));
    } catch (Exception e) {
      LOG.error("Error updating alert definition", e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }
}
