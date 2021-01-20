// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import java.io.InputStream;
import java.io.IOException;

import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.yugabyte.yw.common.ApiResponse;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.inject.Inject;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Users;
import com.yugabyte.yw.forms.UserRegisterFormData;

import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;

import play.Environment;

import static com.yugabyte.yw.models.Users.Role;

public class UsersController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(UsersController.class);

  @Inject
  FormFactory formFactory;

  @Inject
  Environment environment;

  /**
   * GET endpoint for listing the provider User.
   * @return JSON response with user.
   */
  public Result index(UUID customerUUID, UUID userUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    try {
      Users user = Users.get(userUUID);
      return ApiResponse.success(user);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to fetch user.");
    }
  }

  /**
   * GET endpoint for listing all available Users for a customer
   * @return JSON response with users belonging to the customer.
   */
  public Result list(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    try {
      List<Users> users = Users.getAll(customerUUID);
      return ApiResponse.success(users);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to fetch users.");
    }
  }

  /**
   * POST endpoint for creating new Users.
   * @return JSON response of newly created user.
   */
  public Result create(UUID customerUUID) {
    ObjectNode responseJson = Json.newObject();
    ObjectNode errorJson = Json.newObject();

    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    Form<UserRegisterFormData> formData = formFactory.form(UserRegisterFormData.class)
        .bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    Users user;
    try {
      user = Users.create(formData.get().email, formData.get().password,
                          formData.get().role, customerUUID);
      updateFeatures(user);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Could not create user");
    }
    ObjectNode userInfo = Json.newObject()
            .put("email", formData.get().email)
            .put("role", formData.get().role.name())
            .put("customerUUID", customerUUID.toString());
    Audit.createAuditEntry(ctx(), request(), userInfo);
    return ApiResponse.success(user);

  }

  /**
   * DELETE endpoint for deleting an existing user.
   * @return JSON response on whether or not delete user was successful or not.
   */
  public Result delete(UUID customerUUID, UUID userUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }
    Users user = Users.get(userUUID);
    if (user == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid User UUID:" + userUUID);
    }
    if (!user.customerUUID.equals(customerUUID)) {
      return ApiResponse.error(BAD_REQUEST,
          String.format("User UUID %s does not belong to customer %s",
                        userUUID.toString(), customerUUID.toString()));
    }
    if (user.getIsPrimary()) {
      return ApiResponse.error(BAD_REQUEST,
          String.format("Cannot delete primary user %s for customer %s",
                        userUUID.toString(), customerUUID.toString()));
    }
    if (user.delete()) {
      ObjectNode responseJson = Json.newObject();
      responseJson.put("success", true);
      Audit.createAuditEntry(ctx(), request());
      return ApiResponse.success(responseJson);
    } else {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to delete User UUID: " + userUUID);
    }
  }

  /**
   * PUT endpoint for changing the role of an existing user.
   * @return JSON response on whether role change was successful or not.
   */
  public Result changeRole(UUID customerUUID, UUID userUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }
    Users user = Users.get(userUUID);
    if (user == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid User UUID:" + userUUID);
    }
    if (!user.customerUUID.equals(customerUUID)) {
      return ApiResponse.error(BAD_REQUEST,
          String.format("User UUID %s does not belong to customer %s",
                        userUUID.toString(), customerUUID.toString()));
    }
    if (request().getQueryString("role") != null) {
        String role = request().getQueryString("role");
        if (Role.SuperAdmin == user.getRole()) {
          return ApiResponse.error(BAD_REQUEST, "Can't change super admin role.");
        }
        try {
          user.setRole(Role.valueOf(role));
          user.save();
          updateFeatures(user);
        } catch (Exception e) {
          return ApiResponse.error(BAD_REQUEST, "Incorrect Role Specified");
        }
    } else {
      return ApiResponse.error(BAD_REQUEST, "Invalid Request");
    }
    Audit.createAuditEntry(ctx(), request());
    return ApiResponse.success();
  }

  /**
   * PUT endpoint for changing the password of an existing user.
   * @return JSON response on whether role change was successful or not.
   */
  public Result changePassword(UUID customerUUID, UUID userUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }
    Users user = Users.get(userUUID);
    if (user == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid User UUID:" + userUUID);
    }
    if (!user.customerUUID.equals(customerUUID)) {
      return ApiResponse.error(BAD_REQUEST,
          String.format("User UUID %s does not belong to customer %s",
                        userUUID.toString(), customerUUID.toString()));
    }
    Form<UserRegisterFormData> formData = formFactory.form(UserRegisterFormData.class)
        .bindFromRequest();
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    if (formData.get().email.equals(user.email)) {
      if (formData.get().password.equals(formData.get().confirmPassword)) {
        user.setPassword(formData.get().password);
        user.save();
        return ApiResponse.success();
      }
    }
    return ApiResponse.error(BAD_REQUEST, "Invalid User Credentials.");
  }

  private void updateFeatures(Users user) {
    try {
      Customer customer = Customer.get(user.customerUUID);
      String configFile = user.getRole().getFeaturesFile();
      if (customer.code.equals("cloud")) {
        configFile = "cloudFeatureConfig.json";
      }
      if (configFile == null) {
        user.setFeatures(Json.newObject());
        user.save();
        return;
      }
      InputStream featureStream = environment.resourceAsStream(configFile);
      ObjectMapper mapper = new ObjectMapper();
      JsonNode features = mapper.readTree(featureStream);
      user.upsertFeatures(features);
    } catch (IOException e) {
      LOG.error("Failed to parse sample feature config file for OSS mode.");
    }
  }
}
