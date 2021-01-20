// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.google.inject.Singleton;
import play.libs.Json;
import play.libs.ws.WSClient;
import play.libs.ws.WSRequest;
import play.libs.ws.WSResponse;

import java.net.URL;
import java.net.HttpURLConnection;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;

/**
 * Helper class API specific stuff
 */

@Singleton
public class ApiHelper {

  @Inject
  WSClient wsClient;

  public JsonNode postRequest(String url, JsonNode data)  {
    return postRequest(url, data, new HashMap<>());
  }

  public JsonNode postRequest(String url, JsonNode data, Map<String, String> headers) {
    WSRequest request = requestWithHeaders(url, headers);
    CompletionStage<JsonNode> jsonPromise = request.post(data).thenApply(WSResponse::asJson);
    return handleJSONPromise(jsonPromise);
  }

  // Helper method to creaete url object for given webpage string.
  public URL getUrl(String url) {
    try {
      return new URL(url);
    } catch (Exception e) {
      return null;
    }
  }

  // Helper function to get the full body of the webpage via an http request to the given url.
  public String getBody(String url)  {
    WSRequest request = wsClient.url(url);
    CompletionStage<String> jsonPromise = request.get().thenApply(WSResponse::getBody);
    String pageText = null;
    try {
      pageText = jsonPromise.toCompletableFuture().get();
    } catch (InterruptedException | ExecutionException e) {
      pageText = e.getMessage();
    }
    return pageText;
  }

  // API to get the header response for a http request to the given url.
  public ObjectNode getHeaderStatus(String url) {
    ObjectNode objNode = Json.newObject();
    try {
      URL urlObj = getUrl(url);
      if (urlObj != null) {
        objNode.put("status", ((HttpURLConnection)urlObj.openConnection()).getResponseMessage());
      } else {
        objNode.put("status", "Could not connect to URL " + url);
      }
    } catch (Exception e) {
      objNode.put("status", e.getMessage());
    }

    return objNode;
  }

  public JsonNode getRequest(String url) {
    return getRequest(url, new HashMap<>());
  }

  public JsonNode getRequest(String url, Map<String, String> headers) {
    return getRequest(url, headers, new HashMap<>());
  }

  public JsonNode getRequest(String url, Map<String, String> headers, Map<String, String> params) {
    WSRequest request = requestWithHeaders(url, headers);
    if (!params.isEmpty()) {
      for (Map.Entry<String, String> entry : params.entrySet()) {
        request.setQueryParameter(entry.getKey(), entry.getValue());
      }
    }
    CompletionStage<JsonNode> jsonPromise = request
      .get()
      .thenApply(WSResponse::asJson);
    return handleJSONPromise(jsonPromise);
  }

  private JsonNode handleJSONPromise(CompletionStage<JsonNode> jsonPromise) {
    try {
      return jsonPromise.toCompletableFuture().get();
    } catch (InterruptedException | ExecutionException e) {
      return ApiResponse.errorJSON(e.getMessage());
    }
  }

  private WSRequest requestWithHeaders(String url, Map<String, String> headers) {
    WSRequest request = wsClient.url(url);
    if (!headers.isEmpty()) {
      for (Map.Entry<String, String> entry : headers.entrySet()) {
        request.setHeader(entry.getKey(), entry.getValue());
      }
    }
    return request;
  }

  public String buildUrl(String baseUrl, Map<String, String[]> queryParams) {
    if (queryParams.size() > 0) {
      baseUrl += "?";
      StringBuilder requestUrlBuilder = new StringBuilder(baseUrl);
      for (Map.Entry<String, String[]> entry : queryParams.entrySet()) {
        requestUrlBuilder
          .append(entry.getKey()).append("=")
          .append(entry.getValue()[0])
          .append("&");
      }

      baseUrl = requestUrlBuilder.toString();
      baseUrl = baseUrl.substring(0, baseUrl.length() - 1);
    }

    return baseUrl;
  }

  public String replaceProxyLinks(String responseBody, UUID universeUUID, String proxyAddr) {
    String prefix = String.format("/universes/%s/proxy/%s/", universeUUID.toString(), proxyAddr);
    return responseBody.replaceAll("src='/", String.format("src='%s", prefix))
      .replaceAll("src=\"/", String.format("src=\"%s", prefix))
      .replaceAll("href=\"/", String.format("href=\"%s", prefix))
      .replaceAll("href='/", String.format("href='%s", prefix))
      .replaceAll("http://", String.format("/universes/%s/proxy/", universeUUID.toString()));
  }
}
