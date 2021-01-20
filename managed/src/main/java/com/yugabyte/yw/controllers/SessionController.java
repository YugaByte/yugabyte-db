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

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableMap;
import com.google.inject.Inject;
import com.yugabyte.yw.common.*;
import com.yugabyte.yw.forms.CustomerLoginFormData;
import com.yugabyte.yw.forms.CustomerRegisterFormData;
import com.yugabyte.yw.forms.SetSecurityFormData;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Users;

import org.apache.commons.io.input.ReversedLinesFileReader;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.pac4j.core.profile.CommonProfile;
import org.pac4j.core.profile.ProfileManager;
import org.pac4j.play.java.Secure;
import org.pac4j.play.PlayWebContext;
import org.pac4j.play.store.PlaySessionStore;

import play.Configuration;
import play.Environment;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.libs.ws.StandaloneWSResponse;
import play.libs.ws.WSClient;
import play.libs.ws.WSRequest;
import play.mvc.*;

import java.io.File;
import java.io.InputStream;
import java.io.IOException;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.persistence.PersistenceException;

import static com.yugabyte.yw.common.ConfigHelper.ConfigType.Security;
import static com.yugabyte.yw.models.Users.Role;

public class SessionController extends Controller {
  public static final Logger LOG = LoggerFactory.getLogger(SessionController.class);

  final static Pattern PROXY_PATTERN = Pattern.compile("^(.+):([0-9]{1,5})/.*$");

  @Inject
  FormFactory formFactory;

  @Inject
  Configuration appConfig;

  @Inject
  ConfigHelper configHelper;

  @Inject
  Environment environment;

  @Inject
  WSClient ws;

  @Inject
  private PlaySessionStore playSessionStore;

  @Inject
  ApiHelper apiHelper;

  public static final String AUTH_TOKEN = "authToken";
  public static final String API_TOKEN = "apiToken";
  public static final String CUSTOMER_UUID = "customerUUID";
  public static final String USER_UUID = "userUUID";
  private static final Integer FOREVER = 2147483647;

  private CommonProfile getProfile() {
    final PlayWebContext context = new PlayWebContext(ctx(), playSessionStore);
    final ProfileManager<CommonProfile> profileManager = new ProfileManager(context);
    return profileManager.get(true).get();
  }

  public Result login() {
    ObjectNode responseJson = Json.newObject();
    boolean useOAuth = appConfig.getBoolean("yb.security.use_oauth", false);
    if (useOAuth) {
      responseJson.put("error", "Platform login not supported when using SSO.");
      return badRequest(responseJson);
    }
    Form<CustomerLoginFormData> formData = formFactory.form(CustomerLoginFormData.class)
                                                      .bindFromRequest();

    if (formData.hasErrors()) {
      responseJson.set("error", formData.errorsAsJson());
      return badRequest(responseJson);
    }

    CustomerLoginFormData data = formData.get();
    Users user = Users.authWithPassword(data.email.toLowerCase(), data.password);

    if (user == null) {
      responseJson.put("error", "Invalid User Credentials");
      return unauthorized(responseJson);
    }
    Customer cust = Customer.get(user.customerUUID);

    String authToken = user.createAuthToken();
    ObjectNode authTokenJson = Json.newObject();
    authTokenJson.put(AUTH_TOKEN, authToken);
    authTokenJson.put(CUSTOMER_UUID, cust.uuid.toString());
    authTokenJson.put(USER_UUID, user.uuid.toString());
    response().setCookie(Http.Cookie.builder(AUTH_TOKEN, authToken)
                                    .withSecure(ctx().request().secure()).build());
    response().setCookie(Http.Cookie.builder("customerId", cust.uuid.toString())
                                    .withSecure(ctx().request().secure()).build());
    response().setCookie(Http.Cookie.builder("userId", user.uuid.toString())
                                    .withSecure(ctx().request().secure()).build());
    return ok(authTokenJson);
  }

  public Result getPlatformConfig() {
    boolean useOAuth = appConfig.getBoolean("yb.security.use_oauth", false);
    String platformConfig = "window.YB_Platform_Config = window.YB_Platform_Config || %s";
    ObjectNode responseJson = Json.newObject();
    responseJson.put("use_oauth", useOAuth);
    platformConfig = String.format(platformConfig, responseJson.toString());
    return ok(platformConfig);
  }

  @Secure(clients = "OidcClient")
  public Result thirdPartyLogin() {
    ObjectNode responseJson = Json.newObject();
    CommonProfile profile = getProfile();
    String emailAttr = appConfig.getString("yb.security.oidcEmailAttribute", "");
    String email = "";
    if (emailAttr.equals("")) {
      email = profile.getEmail();
    } else {
      email = (String) profile.getAttribute(emailAttr);
    }
    Users user = Users.getByEmail(email.toLowerCase());
    if (user == null) {
      final PlayWebContext context = new PlayWebContext(ctx(), playSessionStore);
      final ProfileManager<CommonProfile> profileManager = new ProfileManager(context);
      profileManager.logout();
      playSessionStore.destroySession(context);
    } else {
      Customer cust = Customer.get(user.customerUUID);
      ctx().args.put("customer", cust);
      ctx().args.put("user", user);
      response().setCookie(Http.Cookie.builder("customerId", cust.uuid.toString())
                                      .withSecure(ctx().request().secure()).build());
      response().setCookie(Http.Cookie.builder("userId", user.uuid.toString())
                                      .withSecure(ctx().request().secure()).build());
    }
    if (environment.isDev()) {
      return redirect("http://localhost:3000/");
    } else {
      return redirect("/");
    }
  }

  public Result insecure_login() {
    ObjectNode responseJson = Json.newObject();
    List<Customer> allCustomers = Customer.getAll();
    if (allCustomers.size() != 1) {
      responseJson.put("error", "Cannot allow insecure with multiple customers.");
      return unauthorized(responseJson);
    }
    String securityLevel = (String) configHelper.getConfig(ConfigHelper.ConfigType.Security)
                                                .get("level");
    if (securityLevel != null && securityLevel.equals("insecure")) {
      List<Users> users = Users.getAllReadOnly();
      if (users == null || users.isEmpty()) {
        responseJson.put("error", "No read only customer exists.");
        return unauthorized(responseJson);
      }
      Users user = users.get(0);
      if (user == null) {
        responseJson.put("error", "Invalid User saved.");
        return unauthorized(responseJson);
      }
      String apiToken = user.getApiToken();
      if (apiToken == null || apiToken.isEmpty()) {
        apiToken = user.upsertApiToken();
      }

      ObjectNode apiTokenJson = Json.newObject();
      apiTokenJson.put(API_TOKEN, apiToken);
      apiTokenJson.put(CUSTOMER_UUID, user.customerUUID.toString());
      apiTokenJson.put(USER_UUID, user.uuid.toString());
      response().setCookie(Http.Cookie.builder(API_TOKEN, apiToken)
                                      .withSecure(ctx().request().secure()).build());
      return ok(apiTokenJson);
    }
    responseJson.put("error", "Insecure login unavailable.");
    return unauthorized(responseJson);
  }

  // Any changes to security should be authenticated.
  @With(TokenAuthenticator.class)
  public Result set_security(UUID customerUUID) {
    Form<SetSecurityFormData> formData = formFactory.form(SetSecurityFormData.class)
                                                    .bindFromRequest();
    ObjectNode responseJson = Json.newObject();
    List<Customer> allCustomers = Customer.getAll();
    if (allCustomers.size() != 1) {
      responseJson.put("error", "Cannot allow insecure with multiple customers.");
      return unauthorized(responseJson);
    }
    if (formData.hasErrors()) {
      responseJson.set("error", formData.errorsAsJson());
      return badRequest(responseJson);
    }

    SetSecurityFormData data = formData.get();
    configHelper.loadConfigToDB(Security, ImmutableMap.of("level", data.level));
    if (data.level.equals("insecure")) {
      Users user = (Users) Http.Context.current().args.get("user");
      String apiToken = user.getApiToken();
      if (apiToken == null || apiToken.isEmpty()) {
        user.upsertApiToken();
      }

      try {
        InputStream featureStream = environment.resourceAsStream("ossFeatureConfig.json");
        ObjectMapper mapper = new ObjectMapper();
        JsonNode features = mapper.readTree(featureStream);
        Customer.get(customerUUID).upsertFeatures(features);
      } catch (IOException e) {
        LOG.error("Failed to parse sample feature config file for OSS mode.");
      }
    }
    return ok();
  }

  @With(TokenAuthenticator.class)
  public Result api_token(UUID customerUUID) {
    Users user = (Users) Http.Context.current().args.get("user");

    if (user == null) {
      return ApiResponse.error(BAD_REQUEST, "Could not find User from given credentials.");
    }

    String apiToken = user.upsertApiToken();
    ObjectNode apiTokenJson = Json.newObject();
    apiTokenJson.put(API_TOKEN, apiToken);
    response().setCookie(Http.Cookie.builder(API_TOKEN, apiToken)
              .withSecure(ctx().request().secure())
              .withMaxAge(FOREVER).build());
    return ok(apiTokenJson);
  }

  public Result register() {
    Form<CustomerRegisterFormData> formData = formFactory.form(CustomerRegisterFormData.class)
                                                         .bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    boolean multiTenant = appConfig.getBoolean("yb.multiTenant", false);
    boolean useOAuth = appConfig.getBoolean("yb.security.use_oauth", false);
    int customerCount = Customer.find.all().size();
    if (!multiTenant && customerCount >= 1) {
      return ApiResponse.error(BAD_REQUEST, "Cannot register multiple "+
                               "accounts in Single tenancy.");
    }
    if (useOAuth && customerCount >= 1) {
      return ApiResponse.error(BAD_REQUEST, "Cannot register multiple "+
                               "accounts with SSO enabled platform.");
    }
    CustomerRegisterFormData data = formData.get();
    if (customerCount == 0) {
      return registerCustomer(data, true);
    } else {
      TokenAuthenticator tokenAuth = new TokenAuthenticator();
      if (tokenAuth.superAdminAuthentication(ctx())) {
        return registerCustomer(data, false);
      } else {
        return ApiResponse.error(BAD_REQUEST, "Only Super Admins can register tenant.");
      }
    }
  }

  private Result registerCustomer(CustomerRegisterFormData data, boolean isSuper) {
    try {
      Customer cust = Customer.create(data.code, data.name);
      if (cust == null) {
        return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to register the customer");
      }
      Role role = Role.Admin;
      if (isSuper) {
        role = Role.SuperAdmin;
      }
      Users user = Users.create(data.email, data.password, role,
          cust.uuid, /* Primary user*/ true);
      String authToken = user.createAuthToken();
      ObjectNode authTokenJson = Json.newObject();
      authTokenJson.put(AUTH_TOKEN, authToken);
      authTokenJson.put(CUSTOMER_UUID, cust.uuid.toString());
      authTokenJson.put(USER_UUID, user.uuid.toString());
      response().setCookie(Http.Cookie.builder(AUTH_TOKEN, authToken)
                                      .withSecure(ctx().request().secure()).build());
      return ok(authTokenJson);
    } catch (PersistenceException pe) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Customer already registered.");
    }
  }

  @With(TokenAuthenticator.class)
  public Result logout() {
    response().discardCookie(AUTH_TOKEN);
    Users user = (Users) Http.Context.current().args.get("user");
    if (user != null) {
      user.deleteAuthToken();
    }
    return ok();
  }

  public Result getUITheme() {
    try {
      return Results.ok(environment.resourceAsStream("theme/theme.css"));
    } catch (NullPointerException ne) {
      return ApiResponse.error(BAD_REQUEST, "Theme file doesn't exists.");
    }
  }

  public Result customerCount() {
    int customerCount = Customer.find.all().size();
    ObjectNode response = Json.newObject();
    response.put("count", customerCount);
    return ApiResponse.success(response);
  }

  public Result appVersion() {
    return ApiResponse.success(configHelper.getConfig(ConfigHelper.ConfigType.SoftwareVersion));
  }

  @With(TokenAuthenticator.class)
  public Result getLogs(Integer maxLines) {
    String appHomeDir = appConfig.getString("application.home", ".");
    String logDir = appConfig.getString("log.override.path", String.format("%s/logs", appHomeDir));
    File file = new File(String.format("%s/application.log", logDir));
    // TODO(bogdan): This is not really pagination friendly as it re-reads everything all the time.
    // TODO(bogdan): Need to figure out if there's a rotation-friendly log-reader..
    try {
      ReversedLinesFileReader reader = new ReversedLinesFileReader(file);
      int index = 0;
      ObjectNode result = Json.newObject();
      ArrayNode lines = Json.newArray();
      while (index++ < maxLines) {
        String line = reader.readLine();
        if (line != null) {
          lines.add(line);
        } else {
          // No more lines.
          break;
        }
      }
      result.put("lines", lines);

      return ApiResponse.success(result);
    } catch (IOException ex) {
      LOG.error("Log file open failed.", ex);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Could not open log file with error " +
                               ex.getMessage());
    }
  }

  @With(TokenAuthenticator.class)
  public Result proxyRequest(UUID universeUUID, String requestUrl) {
    try {
      // Validate that the request is of <ip/hostname>:<port> format
      Matcher matcher = PROXY_PATTERN.matcher(requestUrl);
      if (!matcher.matches()) {
        LOG.error("Request {} does not match expected pattern", requestUrl);
        return ApiResponse.error(BAD_REQUEST, "Invalid proxy request");
      }

      // Extract host + port from request
      String host = matcher.group(1);
      String port = matcher.group(2);
      String addr = String.format("%s:%s", host, port);

      // Validate that the proxy request is for a node from the specified universe
      Universe universe = Universe.get(universeUUID);
      if (!universe.nodeExists(host, Integer.parseInt(port))) {
        LOG.error("Universe {} does not contain node address {}", universeUUID, addr);
        return ApiResponse.error(BAD_REQUEST, "Invalid proxy request");
      }

      // Add query params to proxied request
      requestUrl = apiHelper.buildUrl(requestUrl, request().queryString());

      // Make the request
      WSRequest request = ws.url("http://" + requestUrl);
      CompletionStage<? extends StandaloneWSResponse> response = request.get();
      StandaloneWSResponse r = response.toCompletableFuture().get(1, TimeUnit.MINUTES);

      // Format the response body
      if (r.getStatus() == 200) {
        Result result;
        String url = request.getUrl();
        if (url.contains(".png") || url.contains(".ico") || url.contains("fontawesome")) {
          result = ok(r.getBodyAsBytes().toArray());
        } else {
          result = ok(apiHelper.replaceProxyLinks(r.getBody(), universeUUID, addr));
        }

        // Set response headers
        for (Map.Entry<String, List<String>> entry : r.getHeaders().entrySet()) {
          if (!entry.getKey().equals("Content-Length") && !entry.getKey().equals("Content-Type")) {
            result = result.withHeader(entry.getKey(), String.join(",", entry.getValue()));
          }
        }

        return result.as(r.getContentType());
      } else {
        return ApiResponse.error(BAD_REQUEST, r.getStatusText());
      }
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }
  }
}
