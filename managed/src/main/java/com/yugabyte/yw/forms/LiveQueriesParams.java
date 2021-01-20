// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.JsonNode;
import java.util.List;

/**
 * This class will be used by the API to generate entries on Queries tab
 */
public class LiveQueriesParams {

  public static class YSQLQueryParams {
    public int db_oid;

    public String db_name;

    public String query;

    public String process_start_time;

    @JsonIgnore
    public int process_running_for_ms;

    @JsonIgnore
    public String transaction_start_time;

    @JsonIgnore
    public int transaction_running_for_ms;

    public int query_running_for_ms;

    public String query_start_time;

    public String application_name;

    public String backend_type;

    public String backend_status;

    public String host;

    public String port;
  }

  public static class YCQLQueryParams {
    public String remote_ip;

    public String state;

    public ConnectionDetails connection_details;

    @JsonIgnore
    public int processed_call_count;

    public List<QueryCallsInFlight> calls_in_flight;
  }

  public static class QueryCallsInFlight {
    public int elapsed_millis;

    public CassandraQueryDetails cql_details;

  }

  public static class CassandraQueryDetails {
    public String type;

    public List<JsonNode> call_details;
  }

  public static class ConnectionDetails {
    public JsonNode cql_connection_details;
  }
}
