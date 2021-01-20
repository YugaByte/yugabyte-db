// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import play.Configuration;

import play.mvc.Action;
import play.mvc.Http;
import play.mvc.Result;
import play.mvc.Results;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.UUID;

import com.google.inject.Inject;

import com.yugabyte.yw.common.ConfigHelper;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Users;

import org.pac4j.core.profile.CommonProfile;
import org.pac4j.core.profile.ProfileManager;
import org.pac4j.play.PlayWebContext;
import org.pac4j.play.store.PlaySessionStore;

import static com.yugabyte.yw.common.ConfigHelper.ConfigType.Security;
import static com.yugabyte.yw.models.Users.Role;

public class TokenAuthenticator extends Action.Simple {
  public static final String COOKIE_AUTH_TOKEN = "authToken";
  public static final String AUTH_TOKEN_HEADER =  "X-AUTH-TOKEN";
  public static final String COOKIE_API_TOKEN = "apiToken";
  public static final String API_TOKEN_HEADER = "X-AUTH-YW-API-TOKEN";
  public static final String COOKIE_PLAY_SESSION = "PLAY_SESSION";

  @Inject
  ConfigHelper configHelper;

  @Inject
  Configuration appConfig;

  @Inject
  private PlaySessionStore playSessionStore;

  private Users getCurrentAuthenticatedUser(Http.Context ctx) {
    String token;
    Users user = null;
    boolean useOAuth = appConfig.getBoolean("yb.security.use_oauth", false);
    Http.Cookie cookieValue = ctx.request().cookie(COOKIE_PLAY_SESSION);

    if (useOAuth) {
      final PlayWebContext context = new PlayWebContext(ctx, playSessionStore);
      final ProfileManager<CommonProfile> profileManager = new ProfileManager(context);
      if (profileManager.isAuthenticated()) {
        String emailAttr = appConfig.getString("yb.security.oidcEmailAttribute", "");
        String email = "";
        if (emailAttr.equals("")) {
          email = profileManager.get(true).get().getEmail();
        } else {
          email = (String) profileManager.get(true).get().getAttribute(emailAttr);
        }
        user = Users.getByEmail(email.toLowerCase());
      }
    } else {
      token = fetchToken(ctx, false /* isApiToken */);
      user = Users.authWithToken(token);
    }
    if (user == null && cookieValue == null) {
      token = fetchToken(ctx, true /* isApiToken */);
      if (token != null) {
        user = Users.authWithApiToken(token);
      }
    }
    return user;
  }

  @Override
  public CompletionStage<Result> call(Http.Context ctx) {
    String path = ctx.request().path();
    String endPoint = "";
    String requestType = ctx.request().method();
    Pattern pattern = Pattern.compile(".*/customers/([a-zA-Z0-9-]+)(/.*)?");
    Matcher matcher = pattern.matcher(path);
    UUID custUUID = null;
    if (matcher.find()) {
      custUUID = UUID.fromString(matcher.group(1));
      endPoint = ((endPoint = matcher.group(2)) != null) ? endPoint : "";
    }
    Customer cust = null;
    Users user = getCurrentAuthenticatedUser(ctx);

    if (user != null) {
      cust = Customer.get(user.customerUUID);
    } else {
      return CompletableFuture.completedFuture(Results.forbidden("Unable To Authenticate User"));
    }

    // Some authenticated calls don't actually need to be authenticated
    // (e.g. /metadata/column_types). Only check auth_token is valid in that case.
    if (cust != null && (custUUID == null || custUUID.equals(cust.uuid))) {
      if (!checkAccessLevel(endPoint, user, requestType)) {
        return CompletableFuture.completedFuture(Results.forbidden("User doesn't have access"));
      }
      ctx.request().withUsername(user.getEmail());
      ctx.args.put("customer", cust);
      ctx.args.put("user", user);
    } else {
      // Send Forbidden Response if Authentication Fails.
      return CompletableFuture.completedFuture(Results.forbidden("Unable To Authenticate User"));
    }
    return delegate.call(ctx);
  }

  public boolean superAdminAuthentication(Http.Context ctx) {
    String token = fetchToken(ctx, true);
    Users user = null;
    if (token != null) {
      user = Users.authWithApiToken(token);
    } else {
      token = fetchToken(ctx, false);
      user = Users.authWithToken(token);
    }
    if (user != null) {
      if (user.getRole() == Role.SuperAdmin) {
        return true;
      }
    }
    return false;
  }

  private String fetchToken(Http.Context ctx, boolean isApiToken) {
    String header, cookie;
    if (isApiToken) {
      header = API_TOKEN_HEADER;
      cookie = COOKIE_API_TOKEN;
    } else {
      header = AUTH_TOKEN_HEADER;
      cookie = COOKIE_AUTH_TOKEN;
    }
    String[] headerValue = ctx.request().headers().get(header);
    Http.Cookie cookieValue = ctx.request().cookie(cookie);

    if ((headerValue != null) && (headerValue.length == 1)) {
      return headerValue[0];
    } else if (cookieValue != null) {
      // If we are accessing authenticated pages, the auth token would be in the cookie
      return cookieValue.value();
    }
    return null;
  }

  // Check role, and if the API call is accessible.
  private boolean checkAccessLevel(String endPoint, Users user, String requestType) {
    // Users should be allowed to change their password.
    // Even admin users should not be allowed to change another
    // user's password.
    if (endPoint != null) {
      if (endPoint.endsWith("/change_password")) {
        UUID userUUID = UUID.fromString(endPoint.split("/")[2]);
        if (userUUID.equals(user.uuid)) {
          return true;
        }
        return false;
      }
    }

    // All users have access to get, metrics and setting an API token.
    if (requestType.equals("GET") || endPoint.equals("/metrics") ||
        endPoint.equals("/api_token")) {
      return true;
    }
    // If the user is readonly, then don't get any further access.
    if (user.getRole() == Role.ReadOnly) {
      return false;
    }
    // All users other than read only get access to backup endpoints.
    if (endPoint.endsWith("/create_backup") || endPoint.endsWith("/multi_table_backup") ||
        endPoint.endsWith("/restore")) {
      return true;
    }
    // If the user is backupAdmin, they don't get further access.
    if (user.getRole() == Role.BackupAdmin) {
      return false;
    }
    // If the user has reached here, they have complete access.
    return true;
  }
}
