---
title: Prometheus Integration
headerTitle: Prometheus Integration
linkTitle: Prometheus Integration 
description: Learn about exporting YugabyteDB metrics and monitoring the cluster with Prometheus.
menu:
  latest:
    identifier: observability-3-docker
    parent: explore-observability
    weight: 240
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/observability/prometheus-integration/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/observability/prometheus-integration/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/explore/observability/prometheus-integration/docker" class="nav-link active">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>
<!--
  <li >
    <a href="/latest/explore/observability/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
-->
</ul>

You can monitor your local YugabyteDB cluster with a local instance of [Prometheus](https://prometheus.io/), a popular standard for time-series monitoring of cloud native infrastructure. YugabyteDB services and APIs expose metrics in the Prometheus format at the `/prometheus-metrics` endpoint. For details on the metrics targets for YugabyteDB, see [Prometheus monitoring](../../../reference/configuration/default-ports/#prometheus-monitoring).

This tutorial uses the [yb-docker-ctl](../../../admin/yb-docker-ctl) local cluster management utility.

## Prerequisite

Install a local YugabyteDB universe on Docker using the steps below.

```sh
mkdir ~/yugabyte && cd ~/yugabyte
wget https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/bin/yb-docker-ctl && chmod +x yb-docker-ctl
docker pull yugabytedb/yugabyte
```

## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./yb-docker-ctl destroy
```

Start a new local universe with replication factor of `3`.

```sh
$ ./yb-docker-ctl create  --rf 3
```

## 2. Run the YugabyteDB workload generator

Pull the [yb-sample-apps](https://github.com/yugabyte/yb-sample-apps) Docker container image. This container image has built-in Java client programs for various workloads including SQL inserts and updates.

```sh
$ docker pull yugabytedb/yb-sample-apps
```

Run the `CassandraKeyValue` workload application in a separate shell.

```sh
$ docker run --name yb-sample-apps --hostname yb-sample-apps --net yb-net yugabytedb/yb-sample-apps \
    --workload CassandraKeyValue \
    --nodes yb-tserver-n1:9042 \
    --num_threads_write 1 \
    --num_threads_read 4
```

## 3. Prepare Prometheus configuration file

Copy the following into a file called `yugabytedb.yml`. Move this file to the `/tmp` directory so that you can bind the file to the Prometheus container later on.

```yaml
global:
  scrape_interval:     5s # Set the scrape interval to every 5 seconds. Default is every 1 minute.
  evaluation_interval: 5s # Evaluate rules every 5 seconds. The default is every 1 minute.
  # scrape_timeout is set to the global default (10s).

# YugabyteDB configuration to scrape Prometheus time-series metrics
scrape_configs:
  - job_name: "yugabytedb"
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
      - targets: ["yb-master-n1:7000", "yb-master-n2:7000", "yb-master-n3:7000"]
        labels:
          export_type: "master_export"

      - targets: ["yb-tserver-n1:9000", "yb-tserver-n2:9000", "yb-tserver-n3:9000"]
        labels:
          export_type: "tserver_export"

      - targets: ["yb-tserver-n1:12000", "yb-tserver-n2:12000", "yb-tserver-n3:12000"]
        labels:
          export_type: "cql_export"

      - targets: ["yb-tserver-n1:13000", "yb-tserver-n2:13000", "yb-tserver-n3:13000"]
        labels:
          export_type: "ysql_export"

      - targets: ["yb-tserver-n1:11000", "yb-tserver-n2:11000", "yb-tserver-n3:11000"]
        labels:
          export_type: "redis_export"
```

## 4. Start Prometheus server

Start the Prometheus server as below. The `prom/prometheus` container image will be pulled from the Docker registry if not already present on the localhost.

```sh
$ docker run \
    -p 9090:9090 \
    -v /tmp/yugabytedb.yml:/etc/prometheus/prometheus.yml \
    --net yb-net \
    prom/prometheus
```

Open the Prometheus UI at http://localhost:9090 and then navigate to the Targets page under Status.

![Prometheus Targets](/images/ce/prom-targets-docker.png)

## 5. Analyze key metrics

On the Prometheus Graph UI, you can now plot the read/write throughput and latency for the `CassandraKeyValue` sample app. As you can see from the [source code](https://github.com/yugabyte/yugabyte-db/blob/master/java/yb-loadtester/src/main/java/com/yugabyte/sample/apps/CassandraKeyValue.java) of the app, it uses only SELECT statements for reads and INSERT statements for writes (aside from the initial CREATE TABLE). This means you can measure throughput and latency by simply using the metrics corresponding to the SELECT and INSERT statements.

Paste the following expressions into the **Expression** box and click **Execute** followed by **Add Graph**.

### Throughput

> Read IOPS

```sh
sum(irate(rpc_latency_count{server_type="yb_cqlserver", service_type="SQLProcessor", service_method="SelectStmt"}[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-read-iops.png)

>  Write IOPS

```sh
sum(irate(rpc_latency_count{server_type="yb_cqlserver", service_type="SQLProcessor", service_method="InsertStmt"}[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-write-iops.png)

### Latency

> Read Latency (in microseconds)

```sh
avg(irate(rpc_latency_sum{server_type="yb_cqlserver", service_type="SQLProcessor", service_method="SelectStmt"}[1m])) /
avg(irate(rpc_latency_count{server_type="yb_cqlserver", service_type="SQLProcessor", service_method="SelectStmt"}[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-read-latency.png)

> Write Latency (in microseconds)

```sh
avg(irate(rpc_latency_sum{server_type="yb_cqlserver", service_type="SQLProcessor", service_method="InsertStmt"}[1m])) /
avg(irate(rpc_latency_count{server_type="yb_cqlserver", service_type="SQLProcessor", service_method="InsertStmt"}[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-write-latency.png)

## 6. Clean up (optional)

Optionally, you can shut down the local cluster created in Step 1.

```sh
$ ./yb-docker-ctl destroy
```

## What's next?
You can [setup Grafana](https://prometheus.io/docs/visualization/grafana/) and import the [YugabyteDB dashboard](https://grafana.com/grafana/dashboards/12620 "YugabyteDB dashboard on grafana.com") for better visualization of the metrics being collected by Prometheus.
