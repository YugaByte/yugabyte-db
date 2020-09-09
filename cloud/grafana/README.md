# YugabyteDB Grafana Dashboard

To import the YugabyteDB Grafana dashboard, please see this [Grafana documentation link](https://grafana.com/docs/grafana/latest/reference/export_import/#importing-a-dashboard).

This dashboard was tested with Grafana v6.0.0 and v7.0.3.

## Best Practice:
- In cases where YugabyteDB is being [manually deployed](https://docs.yugabyte.com/latest/deploy/manual-deployment/), please specify a unique value for the flag `--metric_node_name` for each server in order to see distinct graphs.
- The dashboard uses the Prometheus' job name to separate multiple YugabyteDB clusters. Creating individual scrape jobs for each cluster will allow the graphs to be separated cleanly.
