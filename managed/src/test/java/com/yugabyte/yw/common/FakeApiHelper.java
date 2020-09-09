// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;


import akka.stream.Materializer;
import akka.stream.javadsl.Source;
import akka.util.ByteString;
import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Users;
import play.mvc.Http;
import play.mvc.Result;
import play.test.Helpers;
import play.libs.Files;

import java.util.List;
import java.util.Random;

import static play.test.Helpers.route;
import static com.yugabyte.yw.models.Users.Role;

public class FakeApiHelper {
  private static String getAuthToken() {
    Customer customer = Customer.find.query().where().eq("code", "tc").findOne();
    Users user;
    if (customer == null) {
      customer = Customer.create("vc", "Valid Customer");
      user = Users.create("foo@bar.com", "password", Role.Admin, customer.uuid);
    }
    user = Users.find.query().where().eq("customer_uuid", customer.uuid).findOne();
    return user.createAuthToken();
  }

  public static Result doRequest(String method, String url) {
    return doRequestWithAuthToken(method, url, getAuthToken());
  }

  public static Result doRequestWithAuthToken(String method, String url, String authToken) {
    Http.RequestBuilder request = Helpers.fakeRequest(method, url)
            .header("X-AUTH-TOKEN", authToken);
    return route(request);
  }

  public static Result doRequestWithBody(String method, String url, JsonNode body) {
    return doRequestWithAuthTokenAndBody(method, url, getAuthToken(), body);
  }

  public static Result doRequestWithAuthTokenAndBody(String method, String url, String authToken, JsonNode body) {
    Http.RequestBuilder request = Helpers.fakeRequest(method, url)
            .header("X-AUTH-TOKEN", authToken)
            .bodyJson(body);
    return route(request);
  }

  public static Result doRequestWithMultipartData(String method, String url,
                                                  List<Http.MultipartFormData.Part<Source<ByteString, ?>>> data,
                                                  Materializer mat) {
    return doRequestWithAuthTokenAndMultipartData(method, url, getAuthToken(), data, mat);
  }

  public static Result doRequestWithAuthTokenAndMultipartData(
      String method,
      String url,
      String authToken,
      List<Http.MultipartFormData.Part<Source<ByteString, ?>>> data,
      Materializer mat
  ) {
    Http.RequestBuilder request = Helpers.fakeRequest(method, url)
        .header("X-AUTH-TOKEN", authToken)
        .bodyMultipart(data, Files.singletonTemporaryFileCreator(), mat);
    return route(request);
  }
}
