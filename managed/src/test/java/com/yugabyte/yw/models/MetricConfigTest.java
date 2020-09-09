// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.common.FakeDBApplication;
import org.junit.Test;
import play.libs.Json;
import static org.mockito.Matchers.any;

import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import javax.inject.Inject;

import static org.mockito.Mockito.when;
import org.hamcrest.core.*;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.instanceOf;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertEquals;

public class MetricConfigTest extends FakeDBApplication {

  public static int DEFAULT_RANGE_SECS = 60;

  @Test
  public void testMetricsYaml() {
    Map<String, Object> map = new HashMap();
    when(mockYamlWrapper.load(any())).thenReturn(map);
    // Make sure all the configs inside of metrics yaml are valid.
    Map<String, Object> configs = (HashMap<String, Object>) mockYamlWrapper.load("metrics.yml");
    MetricConfig.loadConfig(configs);
    for (MetricConfig config : MetricConfig.find.all()) {
      assertThat(
        config.getLayout(),
        allOf(notNullValue(), IsInstanceOf.instanceOf(MetricConfig.Layout.class))
      );
      assertThat(config.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS), allOf(notNullValue()));
    }
  }

  @Test
  public void testFilterPatterns() {
    ObjectNode configJson = Json.newObject();
    configJson.put("metric", "sample");
    List<String> patterns = Arrays.asList("*", "|", "+", "$");
    for (String pattern : patterns) {
      ObjectNode filterJson = Json.newObject();
      String filterString = "foo"+pattern+"bar";
      filterJson.put("filter", filterString);
      configJson.set("filters", filterJson);
      MetricConfig metricConfig = MetricConfig.create("metric-"+ pattern, configJson);
      metricConfig.save();
      String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
      assertThat(query, allOf(notNullValue(), equalTo("sample{filter=~\"" + filterString + "\"}")));
    }
  }

  @Test
  public void testAvgMetric() {
    JsonNode configJson = Json.parse(
        "{\"metric\": \"log_sync_latency.avg\", \"function\": \"irate|avg\"," +
        "\"range\": true," +
        "\"filters\": {\"export_type\":\"tserver_export\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("(avg(irate(log_sync_latency_sum{export_type=\"tserver_export\"}[60s]))) / (avg(irate(log_sync_latency_count{export_type=\"tserver_export\"}[60s])))")));
  }

  @Test
  public void testDivisionMetric() {
    JsonNode configJson = Json.parse(
        "{\"metric\": \"test_usage/test_request\", \"function\": \"irate|avg\"," +
        "\"filters\": {\"pod_name\":\"yb-tserver-(.*)\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    JsonNode containerUsage = Json.parse(
        "{\"metric\": \"test_usage\", \"function\": \"irate|avg\"," +
        "\"range\": true," +
        "\"filters\": {\"pod_name\":\"yb-tserver-(.*)\"}}");
    JsonNode containerRequest = Json.parse(
        "{\"metric\": \"test_request\", \"function\": \"avg\"," +
        "\"filters\": {\"pod_name\":\"yb-tserver-(.*)\"}}");
    MetricConfig usage = MetricConfig.create("test_usage", containerUsage);
    MetricConfig request = MetricConfig.create("test_request", containerRequest);
    usage.save();
    request.save();
    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("((avg(irate(test_usage{pod_name=~\"yb-tserver-(.*)\"}[60s])))/(avg(test_request{pod_name=~\"yb-tserver-(.*)\"})))*100")));
  }

  @Test
  public void testMultiMetric() {
    JsonNode configJson = Json.parse(
        "{\"metric\": \"log_sync_latency.avg|log_group_commit_latency.avg|log_append_latency.avg\"," +
        "\"function\": \"avg\", \"range\": true}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    Map<String, String> queries = metricConfig.getQueries(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertEquals(queries.size(), 3);
    for (Map.Entry<String, String> e : queries.entrySet()) {
      assertThat(e.getValue(),
        allOf(equalTo(metricConfig.getQuery(e.getKey(), new HashMap<>(), DEFAULT_RANGE_SECS))));
    }
  }

  @Test
  public void testMultiMetricWithMultiFilters() {
    JsonNode configJson = Json.parse(
        "{\"metric\": \"rpc_latency_count\", \"function\": \"irate|sum\"," +
        "\"range\": true," +
        "\"filters\": {\"export_type\": \"tserver_export\"," +
        "\"service_type\": \"TabletServerService\"}," +
        "\"service_method\": \"Read|Write\"}," +
        " \"group_by\": \"service_method\"}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo(
            "sum(irate(rpc_latency_count{export_type=\"tserver_export\", " +
            "service_type=\"TabletServerService\"}[60s]))")));
  }

  @Test
  public void testMultiMetricWithComplexFilters() {
    JsonNode configJson = Json.parse(
        "{\"metric\": \"log_sync_latency.avg|log_group_commit_latency.avg|log_append_latency.avg\"," +
        "\"function\": \"avg\", \"filters\": {\"node_prefix\": \"foo|bar\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    Map<String, String> queries = metricConfig.getQueries(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertEquals(queries.size(), 3);
    for (Map.Entry<String, String> e : queries.entrySet()) {
      assertThat(e.getValue(),
        allOf(equalTo(metricConfig.getQuery(e.getKey(), new HashMap<>(), DEFAULT_RANGE_SECS))));
    }
  }

  @Test
  public void testQueryFailure() {
    MetricConfig metricConfig = MetricConfig.create("metric", Json.newObject());
    metricConfig.save();
    try {
      metricConfig.getQueries(new HashMap<>(), DEFAULT_RANGE_SECS);
    } catch (RuntimeException re) {
      assertThat(re.getMessage(), allOf(notNullValue(), equalTo("Invalid MetricConfig: metric attribute is required")));
    }
  }

  @Test
  public void testSimpleQuery() {
    MetricConfig metricConfig = MetricConfig.create("metric", Json.parse("{\"metric\": \"metric\"}"));
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("metric")));
  }

  @Test
  public void testQueryWithFunctionAndRange() {
    JsonNode configJson = Json.parse("{\"metric\": \"metric\", \"range\": true, " +
                                       "\"function\": \"rate\"}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), 30);
    assertThat(query, allOf(notNullValue(), equalTo("rate(metric[30s])")));
  }

  @Test
  public void testQueryWithRangeWithoutFunction() {
    JsonNode configJson = Json.parse("{\"metric\": \"metric\", \"range\": true}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), 30);
    assertThat(query, allOf(notNullValue(), equalTo("metric")));
  }

  @Test
  public void testQueryWithSingleFilters() {
    JsonNode configJson = Json.parse("{\"metric\": \"metric\", \"range\": true," +
                                       "\"function\": \"rate\", \"filters\": {\"memory\": \"used\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();

    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("rate(metric{memory=\"used\"}[60s])")));
  }

  @Test
  public void testQueryWithAdditionalFiltersWithoutOriginalFilters() {
    JsonNode configJson = Json.parse(
      "{\"metric\": \"metric\", \"range\": true, \"function\": \"rate\"}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();

    String query = metricConfig.getQuery(ImmutableMap.of("extra", "1"), DEFAULT_RANGE_SECS);
    assertThat(query,
      allOf(notNullValue(), equalTo("rate(metric{extra=\"1\"}[60s])")));

  }

  @Test
  public void testQueryWithAdditionalFilters() {
    JsonNode configJson = Json.parse(
      "{\"metric\": \"metric\", \"range\": true," +
      "\"function\": \"rate\", \"filters\": {\"memory\": \"used\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();

    String query = metricConfig.getQuery(ImmutableMap.of("extra", "1"), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("rate(metric{memory=\"used\", extra=\"1\"}[60s])")));
  }

  @Test
  public void testQueryWithComplexFilters() {
    JsonNode configJson = Json.parse(
      "{\"metric\": \"metric\", \"range\": true," +
      "\"function\": \"rate\", \"filters\": {\"memory\": \"used|buffered|free\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("rate(metric{memory=~\"used|buffered|free\"}[60s])")));
  }

  @Test
  public void testQueryWithMultipleFunctions() {
    JsonNode configJson = Json.parse("{\"metric\": \"metric\", \"range\": true," +
                                       "\"function\": \"rate|avg\", \"filters\": {\"memory\": \"used\"}}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();

    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("avg(rate(metric{memory=\"used\"}[60s]))")));
  }

  @Test
  public void testQueryWithOperator() {
    JsonNode configJson = Json.parse(
      "{\"metric\": \"metric\", \"range\": true," +
      "\"function\": \"rate|avg\", \"filters\": {\"memory\": \"used\"}, \"operator\": \"/10\"}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();

    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo("avg(rate(metric{memory=\"used\"}[60s])) /10")));
  }

  @Test
  public void testWithEmptyMetric() {
    JsonNode configJson = Json.parse(
        "{\"metric\":\"\", \"function\":\"avg\", \"filters\": {" +
        "\"saved_name\": \"node_memory_Cached|node_memory_Buffers|node_memory_MemFree\"," +
        "\"group_by\": \"saved_name\"}}");

    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();

    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query, allOf(notNullValue(), equalTo(
            "avg({saved_name=~\"node_memory_Cached|node_memory_Buffers|node_memory_MemFree\", " +
            "group_by=\"saved_name\"})")));
  }

  @Test
  public void testMetricAggregationKeywordWithout() {
    JsonNode configJson = Json.parse(
        "{\"metric\": \"node_disk_bytes_read\"," +
        "\"function\": \"rate|sum without (device)|avg\"," +
        "\"range\": true}");
    MetricConfig metricConfig = MetricConfig.create("metric", configJson);
    metricConfig.save();
    String query = metricConfig.getQuery(new HashMap<>(), DEFAULT_RANGE_SECS);
    assertThat(query,
      allOf(notNullValue(), equalTo("avg(sum without (device)(rate(node_disk_bytes_read[60s])))")));
  }
}
