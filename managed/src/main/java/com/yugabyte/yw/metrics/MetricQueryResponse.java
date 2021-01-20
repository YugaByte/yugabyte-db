// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.metrics;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;

import com.yugabyte.yw.models.MetricConfig;

import play.libs.Json;

import org.apache.commons.lang3.tuple.ImmutablePair;


public class MetricQueryResponse {
  public static final Logger LOG = LoggerFactory.getLogger(MetricQueryResponse.class);

  public static class MetricsData {
    public String resultType;
    public ArrayNode result;
  }

  public String status;
  public MetricsData data;
  public String errorType;
  public String error;

  public static class Entry {
    public HashMap<String, String> labels;
    public ArrayList<ImmutablePair<Double, Double>> values;

    public String toString() {
      ObjectMapper objMapper = new ObjectMapper();
      try {
        return objMapper.writeValueAsString(this);
      } catch (JsonProcessingException je) {
        LOG.error("Invalid object", je);
        return "ResultEntry: [invalid]";
      }
    }

  }



  /**
   * Format MetricQueryResponse object as a json for graph(plot.ly) consumption.
   * @param layout, MetricConfig.Layout object
   * @return JsonNode, Json data that plot.ly can understand
   */
  public ArrayList<MetricGraphData> getGraphData(
      String metricName,
      MetricConfig.Layout layout) {
    ArrayList<MetricGraphData> metricGraphDataList = new ArrayList<>();
    SimpleDateFormat dateFormat = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss");

    for (final JsonNode objNode : data.result) {
      MetricGraphData metricGraphData = new MetricGraphData();
      JsonNode metricInfo = objNode.get("metric");

      if (metricInfo.has("node_prefix")) {
        metricGraphData.name = metricInfo.get("node_prefix").asText();
      } else if (metricInfo.size() == 1) {
        // If we have a group_by clause, the group by name would be the only
        // key in the metrics data, fetch that and use that as the name
        String key = metricInfo.fieldNames().next();
        metricGraphData.name = metricInfo.get(key).asText();
      } else if (metricInfo.size() == 0) {
        // TODO: This is 0 for the special metrics where we would like to grou by __name__ but
        // PromQL seems to not allow for that. As such, we default the metric name to the one
        // passed in.
        //
        // https://www.robustperception.io/whats-in-a-__name__
        metricGraphData.name = metricName;
      }

      if (metricInfo.size() <= 1) {
        if (layout.yaxis != null && layout.yaxis.alias.containsKey(metricGraphData.name)) {
          metricGraphData.name = layout.yaxis.alias.get(metricGraphData.name);
        }
      } else {
        if (layout.yaxis != null) {
          metricGraphData.labels = new HashMap<String, String>();
          for (Map.Entry<String, String> entry : layout.yaxis.alias.entrySet()) {
            boolean validLabels = false;
            for (String key : entry.getKey().split(",")) {
              validLabels = false;
              // Java conversion from Iterator to Iterable...
              for (JsonNode metricEntry : (Iterable<JsonNode>)() -> metricInfo.elements()) {
                // In case we want to graph per server, we want to display the node name.
                if (layout.yaxis.alias.containsKey("useInstanceName")) {
                  metricGraphData.name = metricInfo.get("exported_instance").asText();
                  // If the alias contains more entries, we want to highlight it via the
                  // saved name of the metric.
                  if (layout.yaxis.alias.entrySet().size() > 1) {
                    metricGraphData.name = metricGraphData.name + "-" +
                                           metricInfo.get("saved_name").asText();
                  }
                  validLabels = false;
                  break;
                }
                if (metricEntry.asText().equals(key)) {
                  validLabels = true;
                  break;
                }
              }
              if (!validLabels) {
                break;
              }
            }
            if (validLabels) {
              metricGraphData.name = entry.getValue();
            }
          }
        } else {
          metricGraphData.labels = new HashMap<String, String>();
          metricInfo.fields().forEachRemaining(handler -> {
            metricGraphData.labels.put(handler.getKey(), handler.getValue().asText());
          });
        }
      }

      if (objNode.has("values")) {
        for (final JsonNode valueNode: objNode.get("values")) {
          metricGraphData.x.add(valueNode.get(0).asLong() * 1000);
          JsonNode val = valueNode.get(1);
          if (val.asText().equals("NaN")) {
            metricGraphData.y.add(0);
          } else {
            metricGraphData.y.add(val);
          }
        }
      } else if (objNode.has("value")) {
        metricGraphData.x.add(objNode.get("value").get(0).asLong() * 1000);
        JsonNode val = objNode.get("value").get(1);
        if (val.asText().equals("NaN")) {
          metricGraphData.y.add(0);
        } else {
          metricGraphData.y.add(val);
        }
      }
      metricGraphData.type = "scatter";
      metricGraphDataList.add(metricGraphData);
    }
    return metricGraphDataList;
  }

  /**
   * Converts the JSON result of a prometheus HTTP query call to
   * the MetricQueryResponse.Entry format.
   */
  public ArrayList<MetricQueryResponse.Entry> getValues() {
    if (this.data == null || this.data.result == null) {
      return null;
    }
    ArrayList<MetricQueryResponse.Entry> result = new ArrayList<>();
    ObjectMapper objMapper = new ObjectMapper();
    for (final JsonNode entryNode : this.data.result) {
      try {
        final JsonNode metricNode = entryNode.get("metric");
        final JsonNode valueNode = entryNode.get("value");
        final JsonNode valuesNode = entryNode.get("values");
        if (metricNode == null || (valueNode == null && valuesNode == null)) {
          LOG.trace("Skipping json node while parsing prom response: {}", entryNode);
          continue;
        }
        MetricQueryResponse.Entry entry = new MetricQueryResponse.Entry();
        entry.labels = objMapper.convertValue(metricNode, HashMap.class);
        entry.values = new ArrayList<>();
        if (valueNode != null) {
          entry.values.add(new ImmutablePair<>(
            new Double(valueNode.get(0).asText()), // timestamp
            new Double(valueNode.get(1).asText()) // value
          ));
        } else if (valuesNode != null) {
          entry.values = new ArrayList<>();
          Iterator<JsonNode> elements = valuesNode.elements();
          while (elements.hasNext()) {
            final JsonNode eachValueNode = elements.next();
            entry.values.add(new ImmutablePair<>(
              new Double(eachValueNode.get(0).asText()), // timestamp
              new Double(eachValueNode.get(1).asText()) // value
            ));
          }
        }
        result.add(entry);
      } catch (Exception e) {
        LOG.debug("Skipping json node while parsing prometheus response: {}", entryNode, e);
      }
    }
    return result;
  }


}
