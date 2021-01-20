// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import org.hamcrest.CoreMatchers;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.Matchers;
import org.mockito.Mock;
import org.mockito.Mockito;
import org.mockito.runners.MockitoJUnitRunner;
import play.libs.Json;
import play.libs.ws.WSClient;
import play.libs.ws.WSRequest;
import play.libs.ws.WSResponse;

import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;
import java.net.URLStreamHandler;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.instanceOf;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThat;
import static org.mockito.Matchers.anyString;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.spy;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
public class ApiHelperTest {
  @Mock
  WSClient mockClient;
  @Mock
  WSRequest mockRequest;
  @Mock
  WSResponse mockResponse;
  @Mock
  HttpURLConnection mockConnection;

  @InjectMocks
  ApiHelper apiHelper;

  @Test
  public void testGetRequestValidJSONWithUrl() {
    CompletionStage<WSResponse> mockCompletion = CompletableFuture.completedFuture(mockResponse);
    ObjectNode jsonResponse = Json.newObject();
    jsonResponse.put("Foo", "Bar");
    when(mockClient.url(anyString())).thenReturn(mockRequest);
    when(mockRequest.get()).thenReturn(mockCompletion);
    when(mockResponse.asJson()).thenReturn(jsonResponse);
    JsonNode result = apiHelper.getRequest("http://foo.com/test");
    Mockito.verify(mockClient, times(1)).url("http://foo.com/test");
    assertEquals(result.get("Foo").asText(), "Bar");
  }

  @Test
  public void testGetRequestInvalidJSONWithUrl() {
    CompletionStage<WSResponse> mockCompletion = CompletableFuture.completedFuture(mockResponse);
    when(mockClient.url(anyString())).thenReturn(mockRequest);
    when(mockRequest.get()).thenReturn(mockCompletion);
    doThrow(new RuntimeException("Incorrect JSON")).when(mockResponse).asJson();
    JsonNode result = apiHelper.getRequest("http://foo.com/test");
    Mockito.verify(mockClient, times(1)).url("http://foo.com/test");
    assertThat(result.get("error").asText(), CoreMatchers.equalTo("java.lang.RuntimeException: Incorrect JSON"));
  }

  @Test
  public void testGetRequestWithHeaders() {
    CompletionStage<WSResponse> mockCompletion = CompletableFuture.completedFuture(mockResponse);
    ObjectNode jsonResponse = Json.newObject();
    jsonResponse.put("Foo", "Bar");
    when(mockClient.url(anyString())).thenReturn(mockRequest);
    when(mockRequest.get()).thenReturn(mockCompletion);
    when(mockResponse.asJson()).thenReturn(jsonResponse);

    HashMap<String, String> headers = new HashMap<>();
    headers.put("header", "sample");
    JsonNode result = apiHelper.getRequest("http://foo.com/test", headers);
    Mockito.verify(mockClient, times(1)).url("http://foo.com/test");
    Mockito.verify(mockRequest).setHeader("header", "sample");
    assertEquals(result.get("Foo").asText(), "Bar");
  }

  @Test
  public void testGetRequestWithParams() {
    CompletionStage<WSResponse> mockCompletion = CompletableFuture.completedFuture(mockResponse);
    ObjectNode jsonResponse = Json.newObject();
    jsonResponse.put("Foo", "Bar");
    when(mockClient.url(anyString())).thenReturn(mockRequest);
    when(mockRequest.get()).thenReturn(mockCompletion);
    when(mockResponse.asJson()).thenReturn(jsonResponse);

    HashMap<String, String> params = new HashMap<>();
    params.put("param", "foo");
    JsonNode result = apiHelper.getRequest("http://foo.com/test", new HashMap<String, String>(), params);
    Mockito.verify(mockClient, times(1)).url("http://foo.com/test");
    Mockito.verify(mockRequest).setQueryParameter("param", "foo");
    assertEquals(result.get("Foo").asText(), "Bar");
  }

  @Test
  public void testPostRequestWithValidURLAndData() {
    CompletionStage<WSResponse> mockCompletion = CompletableFuture.completedFuture(mockResponse);
    when(mockClient.url(anyString())).thenReturn(mockRequest);
    ObjectNode postData = Json.newObject();
    postData.put("Foo", "Bar");
    ObjectNode jsonResponse = Json.newObject();
    jsonResponse.put("Success", true);

    when(mockRequest.post(Matchers.any(JsonNode.class))).thenReturn(mockCompletion);
    when(mockResponse.asJson()).thenReturn(jsonResponse);
    JsonNode result = apiHelper.postRequest("http://foo.com/test", postData);
    Mockito.verify(mockClient, times(1)).url("http://foo.com/test");
    Mockito.verify(mockRequest, times(1)).post(postData);
    assertEquals(result.get("Success").asBoolean(), true);
  }

  private void testGetHeaderRequestHelper(String urlPath, boolean isSuccess) {
    final URLStreamHandler handler = new URLStreamHandler() {
      @Override
      protected URLConnection openConnection(final URL arg0) throws IOException {
        return mockConnection;
      }
    };
    ApiHelper mockApiHelper = spy(apiHelper);
    try {
      when(mockConnection.getResponseMessage()).thenReturn(isSuccess ? "OK" : "Not Found");
      URL url = new URL(null, urlPath, handler);
      when(mockApiHelper.getUrl(urlPath)).thenReturn(url);
    } catch (Exception e) {
      e.printStackTrace();
      assertNull(e.getMessage());
    }
    ObjectNode result = mockApiHelper.getHeaderStatus(urlPath);
    if (isSuccess) {
      assertEquals(result.get("status").asText(), "OK");
    } else {
      assertEquals(result.get("status").asText(), "Not Found");
    }
  }

  @Test
  public void testGetHeaderRequestOKL() {
    testGetHeaderRequestHelper("http://www.yugabyte.com", true);
  }

  @Test
  public void testGetHeaderRequestNonOK() {
    testGetHeaderRequestHelper("http://www.yugabyte.com", false);
  }

  @Test
  public void testGetHeaderRequestWithInvalidURL() {
    testGetHeaderRequestHelper("file:///my/yugabyte/com", false);
  }

  @Test
  public void testBuildUrl() {
    ApiHelper mockApiHelper = spy(apiHelper);
    String baseUrl = "http://test.com";
    String expectedResult = String.format("%s?testKey2=test2&testKey1=test1", baseUrl);
    String[] values1 = {"test1"};
    String[] values2 = {"test2"};
    Map<String, String[]> queryParams = new HashMap<>();
    queryParams.put("testKey1", values1);
    queryParams.put("testKey2", values2);
    assertEquals(mockApiHelper.buildUrl(baseUrl, queryParams), expectedResult);
  }
}
