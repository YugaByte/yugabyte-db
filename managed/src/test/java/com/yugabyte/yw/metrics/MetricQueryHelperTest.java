// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.metrics;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.models.MetricConfig;
import com.yugabyte.yw.metrics.MetricQueryResponse;

import org.joda.time.DateTime;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.runners.MockitoJUnitRunner;
import play.libs.Json;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static junit.framework.TestCase.assertTrue;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.hamcrest.CoreMatchers.instanceOf;
import org.hamcrest.core.*;
import static org.hamcrest.core.AllOf.allOf;
import static org.hamcrest.core.Is.is;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertEquals;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Matchers.anyString;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;


@RunWith(MockitoJUnitRunner.class)
public class MetricQueryHelperTest extends FakeDBApplication {

  @InjectMocks
  MetricQueryHelper metricQueryHelper;

  @Mock
  play.Configuration mockAppConfig;

  MetricConfig validMetric;

  @Before
  public void setUp() {
    JsonNode configJson = Json.parse("{\"metric\": \"my_valid_metric\", \"function\": \"sum\"}");
    validMetric = MetricConfig.create("valid_metric", configJson);
    validMetric.save();
    when(mockAppConfig.getString("yb.metrics.url")).thenReturn("foo://bar");
  }

  @Test
  public void testQueryWithInvalidParams() {
    try {
      metricQueryHelper.query(Collections.emptyList(), Collections.emptyMap());
    } catch (RuntimeException re) {
      assertThat(re.getMessage(), allOf(notNullValue(), equalTo("Empty metricKeys data provided.")));
    }
  }

  @Test
  public void testQueryWithInvalidFilterParams() {
    HashMap<String, String> params = new HashMap<>();
    params.put("start", "1479281737");
    params.put("filters", "my-own-filter");

    try {
      metricQueryHelper.query(ImmutableList.of("valid_metric"), params);
    } catch (RuntimeException re) {
      assertThat(re.getMessage(), allOf(notNullValue(), equalTo("Invalid filter params provided, it should be a hash.")));
    }
  }

  @Test
  public void testQuerySingleMetricWithoutEndTime() {
    DateTime date = DateTime.now().minusMinutes(1);
    Integer startTimestamp = Math.toIntExact(date.getMillis() / 1000);
    HashMap<String, String> params = new HashMap<>();
    params.put("start", startTimestamp.toString());

    JsonNode responseJson = Json.parse("{\"status\":\"success\",\"data\":{\"resultType\":\"vector\",\"result\":[{\"metric\":\n" +
                                         " {\"cpu\":\"system\"},\"value\":[1479278137,\"0.027751899056199826\"]}]}}");

    ArgumentCaptor<String> queryUrl = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<Map> queryParam = ArgumentCaptor.forClass(Map.class);

    when(mockApiHelper.getRequest(anyString(), anyMap(), anyMap())).thenReturn(responseJson);
    metricQueryHelper.query(ImmutableList.of("valid_metric"), params);
    verify(mockApiHelper).getRequest(queryUrl.capture(), anyMap(), (Map<String, String>) queryParam.capture());

    assertThat(queryUrl.getValue(), allOf(notNullValue(), equalTo("foo://bar/query")));
    assertThat(
      queryParam.getValue(),
      allOf(notNullValue(), IsInstanceOf.instanceOf(HashMap.class))
    );

    Map<String, String> graphQueryParam = queryParam.getValue();
    assertThat(graphQueryParam.get("query"), allOf(notNullValue(), equalTo("sum(my_valid_metric)")));
    assertThat(Integer.parseInt(graphQueryParam.get("time")), allOf(notNullValue(), equalTo(startTimestamp)));
    assertThat(Integer.parseInt(graphQueryParam.get("step")), is(notNullValue()));
    assertThat(Integer.parseInt(graphQueryParam.get("_")), is(notNullValue()));
  }

  @Test
  public void testQuerySingleMetricWithEndTime() {
    DateTime date = DateTime.now();
    Integer startTimestamp = Math.toIntExact(date.minusMinutes(10).getMillis() / 1000);
    Integer endTimestamp = Math.toIntExact(date.getMillis() / 1000);
    HashMap<String, String> params = new HashMap<>();
    params.put("start", startTimestamp.toString());
    params.put("end", endTimestamp.toString());

    JsonNode responseJson = Json.parse("{\"status\":\"success\",\"data\":{\"resultType\":\"vector\",\"result\":[{\"metric\":\n" +
                                         " {\"cpu\":\"system\"},\"value\":[1479278137,\"0.027751899056199826\"]}]}}");

    ArgumentCaptor<String> queryUrl = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<Map> queryParam = ArgumentCaptor.forClass(Map.class);

    when(mockApiHelper.getRequest(anyString(), anyMap(), anyMap())).thenReturn(responseJson);
    metricQueryHelper.query(ImmutableList.of("valid_metric"), params);
    verify(mockApiHelper).getRequest(queryUrl.capture(), anyMap(), (Map<String, String>) queryParam.capture());

    assertThat(queryUrl.getValue(), allOf(notNullValue(), equalTo("foo://bar/query_range")));
    assertThat(
      queryParam.getValue(),
      allOf(notNullValue(), IsInstanceOf.instanceOf(HashMap.class))
    );

    Map<String, String> graphQueryParam = queryParam.getValue();
    assertThat(graphQueryParam.get("query"), allOf(notNullValue(), equalTo("sum(my_valid_metric)")));
    assertThat(Integer.parseInt(graphQueryParam.get("start")), allOf(notNullValue(), equalTo(startTimestamp)));
    assertThat(Integer.parseInt(graphQueryParam.get("end")), allOf(notNullValue(), equalTo(endTimestamp)));
    assertThat(Integer.parseInt(graphQueryParam.get("step")), allOf(notNullValue(), equalTo(6)));
  }

  @Test
  public void testDirectQuerySingleValue() {

    JsonNode responseJson = Json.parse("{\"status\":\"success\",\"data\":{\"resultType\":\"vector\",\"result\":[{\"metric\":\n" +
    " {\"__name__\":\"foobar\", \"node_prefix\":\"yb-test-1\"},\"value\":[1479278137,\"0.027751899056199826\"]}]}}");

    when(mockApiHelper.getRequest(anyString(), anyMap(), anyMap())).thenReturn(responseJson);

    ArrayList<MetricQueryResponse.Entry> results = metricQueryHelper.queryDirect("foobar");
    assertEquals(results.size(), 1);
    assertEquals(results.get(0).labels.size(), 2);
    assertEquals(results.get(0).labels.get("node_prefix"), "yb-test-1");
    assertEquals(results.get(0).values.size(), 1);
    assertEquals(results.get(0).values.get(0).getLeft().intValue(), 1479278137);
    assertEquals(results.get(0).values.get(0).getRight().doubleValue(), 0.028, 0.005);
  }

  @Test
  public void testDirectQueryMultipleValues() {

    JsonNode responseJson = Json.parse("{\"status\":\"success\",\"data\":{\"resultType\":\"vector\",\"result\":[{\"metric\":\n" +
    " {\"__name__\":\"foobar\", \"node_prefix\":\"yb-test-1\"},\"values\":[[1479278132,\"0.037751899056199826\"], [1479278137,\"0.027751899056199826\"]]}]}}");

    when(mockApiHelper.getRequest(anyString(), anyMap(), anyMap())).thenReturn(responseJson);

    ArrayList<MetricQueryResponse.Entry> results = metricQueryHelper.queryDirect("foobar");
    assertEquals(results.size(), 1);
    assertEquals(results.get(0).labels.size(), 2);
    assertEquals(results.get(0).labels.get("node_prefix"), "yb-test-1");
    assertEquals(results.get(0).values.size(), 2);
    assertEquals(results.get(0).values.get(0).getLeft().intValue(), 1479278132);
    assertEquals(results.get(0).values.get(1).getLeft().intValue(), 1479278137);
    assertEquals(results.get(0).values.get(0).getRight().doubleValue(), 0.038, 0.005);
    assertEquals(results.get(0).values.get(1).getRight().doubleValue(), 0.028, 0.005);

  }

  @Test
  public void testQueryMultipleMetrics() {
    HashMap<String, String> params = new HashMap<>();
    params.put("start", "1481147528");
    params.put("end", "1481147648");

    JsonNode configJson = Json.parse("{\"metric\": \"my_valid_metric2\", \"function\": \"avg\"}");
    MetricConfig validMetric2 = MetricConfig.create("valid_metric2", configJson);
    validMetric2.save();

    JsonNode responseJson = Json.parse("{\"status\":\"success\",\"data\":{\"resultType\":\"vector\",\"result\":[{\"metric\":\n" +
                                         " {\"cpu\":\"system\"},\"value\":[1479278137,\"0.027751899056199826\"]}]}}");

    ArgumentCaptor<String> queryUrl = ArgumentCaptor.forClass(String.class);
    ArgumentCaptor<Map> queryParam = ArgumentCaptor.forClass(Map.class);
    List<String> metricKeys = ImmutableList.of("valid_metric2", "valid_metric");

    when(mockApiHelper.getRequest(anyString(), anyMap(), anyMap())).thenReturn(responseJson);
    JsonNode result = metricQueryHelper.query(metricKeys, params);
    verify(mockApiHelper, times(2)).getRequest(queryUrl.capture(), anyMap(), (Map<String, String>) queryParam.capture());
    assertThat(queryUrl.getValue(), allOf(notNullValue(), equalTo("foo://bar/query_range")));
    assertThat(
      queryParam.getValue(),
      allOf(notNullValue(), IsInstanceOf.instanceOf(HashMap.class))
    );

    List<String> expectedQueryStrings = new ArrayList<>();
    expectedQueryStrings.add(validMetric.getQuery(new HashMap<>(), 60 /* queryRangeSecs */));
    expectedQueryStrings.add(validMetric2.getQuery(new HashMap<>(), 60 /* queryRangeSecs */));

    for (Map<String, String> capturedQueryParam: queryParam.getAllValues()) {
      assertTrue(expectedQueryStrings.contains(capturedQueryParam.get("query")));
      assertTrue(metricKeys.contains(capturedQueryParam.get("queryKey")));
      assertThat(Integer.parseInt(capturedQueryParam.get("start").toString()), allOf(notNullValue(), equalTo(1481147528)));
      assertThat(Integer.parseInt(capturedQueryParam.get("step").toString()), allOf(notNullValue(), equalTo(1)));
      assertThat(Integer.parseInt(capturedQueryParam.get("end").toString()), allOf(notNullValue(), equalTo(1481147648)));
    }
  }
}
