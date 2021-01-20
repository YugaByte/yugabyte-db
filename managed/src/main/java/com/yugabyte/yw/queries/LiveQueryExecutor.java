// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.queries;


import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.forms.LiveQueriesParams;
import com.yugabyte.yw.models.MetricConfig;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.Configuration;
import play.libs.Json;

import java.util.*;
import java.util.concurrent.Callable;

public class LiveQueryExecutor implements Callable<JsonNode> {
  public static final Logger LOG = LoggerFactory.getLogger(LiveQueryExecutor.class);

  public enum QueryApi {
    YSQL,
    YCQL
  }

  private ApiHelper apiHelper;
  // hostname can be either IP address or DNS
  private String hostName;
  private String nodeName;
  private int port;
  private QueryApi apiType;

  public LiveQueryExecutor(ApiHelper apiHelper, String nodeName, String hostName,
                           int port, QueryApi api) {
    this.apiHelper = apiHelper;
    this.nodeName = nodeName;
    this.hostName = hostName;
    this.port = port;
    this.apiType = api;
  }

  @Override
  public JsonNode call() throws Exception {
    String url = String.format("http://%s:%d/rpcz", hostName, port);
    try {
      JsonNode response = apiHelper.getRequest(url);
      if (apiType == QueryApi.YSQL) {
        return processYSQLRowData(response);
      } else {
        return processYCQLRowData(response);
      }
    } catch (Exception e) {
      LOG.error("Exception while fetching url: {}; message: {}", url, e.getStackTrace());
      ObjectNode errorJson = Json.newObject();
      errorJson.put("error", e.getMessage());
      errorJson.put("type", apiType == QueryApi.YSQL ? "ysql" : "ycql");
      return errorJson;
    }
  }

  // Processes YSQL connection data from /rpcz endpoint and transforms to row data
  private JsonNode processYSQLRowData(JsonNode response) {
    ObjectNode responseJson = Json.newObject();
    ObjectMapper mapper = new ObjectMapper();
    if (response.has("connections")) {
      for (JsonNode objNode : response.get("connections")) {
        if (objNode.has("backend_type") &&
          objNode.get("backend_type").asText().equalsIgnoreCase("client backend") &&
          objNode.has("backend_status") &&
          !objNode.get("backend_status").asText().equalsIgnoreCase("idle")) {
          try {
            // Rather than access the JSON through .get()'s we convert to POJO
            LiveQueriesParams.YSQLQueryParams params = mapper.treeToValue(
              objNode,
              LiveQueriesParams.YSQLQueryParams.class
            );
            ObjectNode rowData = Json.newObject();
            // Random UUID intended for table row key
            rowData.put("id", UUID.randomUUID().toString());
            rowData.put("nodeName", nodeName);
            rowData.put("privateIp", hostName);
            rowData.put("dbName", params.db_name);
            rowData.put("sessionStatus", params.backend_status);
            rowData.put("query", params.query);
            rowData.put("elapsedMillis", params.query_running_for_ms);
            rowData.put("queryStartTime", params.query_start_time);
            rowData.put("appName", params.application_name);
            rowData.put("clientHost", params.host);
            rowData.put("clientPort", params.port);
            if (!responseJson.has("ysql")) {
              ArrayNode ysqlArray = responseJson.putArray("ysql");
              ysqlArray.add(rowData);
            } else {
              ArrayNode ysqlArray = (ArrayNode) responseJson.get("ysql");
              ysqlArray.add(rowData);
            }
          } catch (JsonProcessingException exception) {
            // Try to process all connections even if there is an exception
            LOG.error("Unable to process JSON from YSQL query. {}", objNode.asText());
          }
        }
      }
    }
    return responseJson;
  }

  // Similar to above helper function except for YCQL connection info
  private JsonNode processYCQLRowData(JsonNode response) {
    ObjectNode responseJson = Json.newObject();
    ObjectMapper mapper = new ObjectMapper();
    if (response.has("inbound_connections")) {
      for (JsonNode objNode : response.get("inbound_connections")) {
        try {
          LiveQueriesParams.YCQLQueryParams params = mapper.treeToValue(
            objNode,
            LiveQueriesParams.YCQLQueryParams.class
          );
          if (params.calls_in_flight != null) {
            for (LiveQueriesParams.QueryCallsInFlight query : params.calls_in_flight) {
              // Get SQL query string, joining multiple entries if necessary
              StringBuilder queryStringBuilder = new StringBuilder();
              ObjectNode rowData = Json.newObject();
              for (JsonNode callDetail : query.cql_details.call_details) {
                if (queryStringBuilder.length() > 0) {
                  queryStringBuilder.append(" ");
                }
                queryStringBuilder.append(callDetail.get("sql_string").asText());
              }
              String keyspace = params.connection_details.cql_connection_details
                .get("keyspace").asText();
              String[] splitIp = params.remote_ip.split(":");
              // Random UUID intended for table row key
              rowData.put("id", UUID.randomUUID().toString());
              rowData.put("nodeName", nodeName);
              rowData.put("privateIp", hostName);
              rowData.put("keyspace", keyspace);
              rowData.put("query", queryStringBuilder.toString());
              rowData.put("type", query.cql_details.type);
              rowData.put("elapsedMillis", query.elapsed_millis);
              rowData.put("clientHost", splitIp[0]);
              rowData.put("clientPort", splitIp[1]);

              if (!responseJson.has("ycql")) {
                ArrayNode ycqlArray = responseJson.putArray("ycql");
                ycqlArray.add(rowData);
              } else {
                ArrayNode ycqlArray = (ArrayNode) responseJson.get("ycql");
                ycqlArray.add(rowData);
              }
            }
          }
        } catch (JsonProcessingException exception) {
          LOG.error("Unable to process JSON from YCQL query connection", objNode.asText());
        }
      }
    }
    return responseJson;
  }
}
