# YugabyteDB Grafana Dashboard

To import the YugabyteDB Grafana dashboard, please see this [Grafana documentation link](https://grafana.com/docs/grafana/latest/reference/export_import/#importing-a-dashboard).

This dashboard was tested with Grafana v6.0.0 and v7.0.3.

## Best Practice:
- In cases where YugabyteDB is being [manually deployed](https://docs.yugabyte.com/latest/deploy/manual-deployment/), please specify a unique value for the flag `--metric_node_name` for each server in order to see distinct graphs.
- This dashboard uses the label `node_prefix` to separate multiple YugabyteDB clusters. Creating individual scrape jobs for each cluster will allow the graphs to be separated cleanly.

## Prometheus configuration
Here is a sample Prometheus configuration with required relabel configurations.
- Make sure you replace the IP addresses with correct IP addresses of the machines where YB-Master and YB-TServer services are running.
- Replace `cluster-1` with a desired identifier for the particular cluster.

  ```yaml
  global:
    scrape_interval:     5s # Set the scrape interval to every 5 seconds. Default is every 1 minute.
    evaluation_interval: 5s # Evaluate rules every 5 seconds. The default is every 1 minute.
    # scrape_timeout is set to the global default (10s).

  # YugaByte DB configuration to scrape Prometheus time-series metrics
  scrape_configs:
    - job_name: "yugabytedb-cluster-1"
      metrics_path: /prometheus-metrics
      relabel_configs:
        - target_label: "node_prefix"
          replacement: "cluster-1"
      metric_relabel_configs:
        # Save the name of the metric so we can group_by since we cannot by __name__ directly...
        - source_labels: ["__name__"]
          regex: "(.*)"
          target_label: "saved_name"
          replacement: "$1"
        # The following basically retrofit the handler_latency_* metrics to label format.
        - source_labels: ["__name__"]
          regex: "handler_latency_(yb_[^_]*)_([^_]*)_([^_]*)(.*)"
          target_label: "server_type"
          replacement: "$1"
        - source_labels: ["__name__"]
          regex: "handler_latency_(yb_[^_]*)_([^_]*)_([^_]*)(.*)"
          target_label: "service_type"
          replacement: "$2"
        - source_labels: ["__name__"]
          regex: "handler_latency_(yb_[^_]*)_([^_]*)_([^_]*)(_sum|_count)?"
          target_label: "service_method"
          replacement: "$3"
        - source_labels: ["__name__"]
          regex: "handler_latency_(yb_[^_]*)_([^_]*)_([^_]*)(_sum|_count)?"
          target_label: "__name__"
          replacement: "rpc_latency$4"

      static_configs:
        - targets: ["10.0.0.1:7000", "10.0.0.2:7000", "10.0.0.3:7000"]
          labels:
            group: "yb-master"
            export_type: "master_export"

        - targets: ["10.0.0.101:9000", "10.0.0.102:9000", "10.0.0.103:9000"]
          labels:
            group: "yb-tserver"
            export_type: "tserver_export"

        - targets: ["10.0.0.101:12000", "10.0.0.102:12000", "10.0.0.103:12000"]
          labels:
            group: "ycql"
            export_type: "cql_export"

        - targets: ["10.0.0.101:13000", "10.0.0.102:13000", "10.0.0.103:13000"]
          labels:
            group: "ysql"
            export_type: "ysql_export"

        - targets: ["10.0.0.101:11000", "10.0.0.102:11000", "10.0.0.103:11000"]
          labels:
            group: "yedis"
            export_type: "redis_export"
  ```
