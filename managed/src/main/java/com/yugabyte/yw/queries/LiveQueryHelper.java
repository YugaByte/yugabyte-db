// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.queries;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import com.google.inject.Inject;
import com.google.inject.Singleton;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import org.joda.time.DateTime;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.Configuration;
import play.libs.Json;

import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.ArrayList;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

@Singleton
public class LiveQueryHelper {
  public static final Logger LOG = LoggerFactory.getLogger(LiveQueryHelper.class);
  public static final Integer QUERY_EXECUTOR_THREAD_POOL = 5;

  @Inject
  ApiHelper apiHelper;

  public JsonNode query(Universe universe) {
    ExecutorService threadPool = Executors.newFixedThreadPool(QUERY_EXECUTOR_THREAD_POOL);
    Set<Future<JsonNode>> futures = new HashSet<Future<JsonNode>>();
    ObjectNode responseJson = Json.newObject();
    ObjectNode ysqlJson = Json.newObject();
    ysqlJson.put("errorCount", 0);
    ysqlJson.putArray("queries");
    ObjectNode ycqlJson = Json.newObject();
    ycqlJson.put("errorCount", 0);
    ycqlJson.putArray("queries");
    for (NodeDetails node : universe.getNodes()) {
      if (node.isActive() && node.isTserver) {
        String ip = node.cloudInfo.private_ip == null ?
          node.cloudInfo.private_dns :
          node.cloudInfo.private_ip;
        Callable<JsonNode> callable = new LiveQueryExecutor(
          apiHelper,
          node.nodeName,
          ip,
          node.ysqlServerHttpPort,
          LiveQueryExecutor.QueryApi.YSQL
        );
        Future<JsonNode> future = threadPool.submit(callable);
        futures.add(future);

        callable = new LiveQueryExecutor(
          apiHelper,
          node.nodeName,
          ip,
          node.yqlServerHttpPort,
          LiveQueryExecutor.QueryApi.YCQL
        );
        future = threadPool.submit(callable);
        futures.add(future);
      }
    }

    try {
      for (Future<JsonNode> future : futures) {
        JsonNode response = future.get();
        if (response.has("error")) {
          String type = response.get("type").asText();
          if (type == "ysql") {
            ysqlJson.put("errorCount", ysqlJson.get("errorCount").asInt() + 1);
          } else if (type == "ycql") {
            ycqlJson.put("errorCount", ycqlJson.get("errorCount").asInt() + 1);
          }
        } else {
          if (response.has("ysql")) {
            ArrayNode arr = (ArrayNode) ysqlJson.get("queries");
            concatArrayNodes(arr, response.get("ysql"));

          } else if (response.has("ycql")) {
            ArrayNode arr = (ArrayNode) ycqlJson.get("queries");
            concatArrayNodes(arr, response.get("ycql"));
          }
        }
      }
    } catch (InterruptedException e) {
      LOG.error("Error fetching live query data", e);
    } catch (ExecutionException e) {
      LOG.error("Error fetching live query data", e);
      e.printStackTrace();
    } finally {
      threadPool.shutdown();
    }

    responseJson.set("ysql", ysqlJson);
    responseJson.set("ycql", ycqlJson);

    threadPool.shutdown();
    return responseJson;
  }

  private void concatArrayNodes(ArrayNode destination, JsonNode source) {
    for (JsonNode node : source) {
      destination.add(node);
    }
  }
}
