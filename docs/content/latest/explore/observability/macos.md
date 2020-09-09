---
title: Explore observability with Prometheus on macOS
headerTitle: Observability
linkTitle: Observability 
description: Learn about observability with Prometheus in a local three-node YugabyteDB cluster on macOS.
aliases:
  - /latest/explore/cloud-native/observability/
  - /latest/explore/observability/
  - /latest/explore/observability-macos/
  - /latest/explore/cloud-native/binary/observability/
menu:
  latest:
    identifier: observability-1-macos
    parent: explore
    weight: 240
isTocNested: true
showAsideToc: true
---

 <ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/observability/macos" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/observability/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/explore/observability/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>
<!--
  <li >
    <a href="/latest/explore/observability-kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
-->
</ul>

You can monitor your local YugabyteDB cluster with a local instance of [Prometheus](https://prometheus.io/), a popular standard for time-series monitoring of cloud native infrastructure. YugabyteDB services and APIs expose metrics in the Prometheus format at the `/prometheus-metrics` endpoint. For details on the metrics targets for YugabyteDB, see [Monitoring with Prometheus](../../../reference/configuration/default-ports/#monitoring-with-prometheus).

This tutorial uses the [yb-ctl](../../../admin/yb-ctl) local cluster management utility.

## Prerequisite

Prometheus is installed on your local machine. If you have not done so already, follow the links below.

- [Download Prometheus](https://prometheus.io/download/)
- [Get Started with Prometheus](https://prometheus.io/docs/prometheus/latest/getting_started/)

## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./bin/yb-ctl destroy
```

Start a new local YugabyteDB cluster - by default, this will create a three-node universe with a replication factor of `3`.

```sh
$ ./bin/yb-ctl create
```

## 2. Run the YugabyteDB workload generator

Download the [YugabyteDB workload generator](https://github.com/yugabyte/yb-sample-apps) JAR file (`yb-sample-apps.jar`) by running the following command.

```sh
$ wget https://github.com/yugabyte/yb-sample-apps/releases/download/v1.3.0/yb-sample-apps.jar?raw=true -O yb-sample-apps.jar 
```

Run the `CassandraKeyValue` workload in a separate shell.

```sh
$ java -jar ./yb-sample-apps.jar \
    --workload CassandraKeyValue \
    --nodes 127.0.0.1:9042 \
    --num_threads_read 1 \
    --num_threads_write 1
```

## 3. Prepare Prometheus configuration file

Copy the following into a file called `yugabytedb.yml`.

```sh
global:
  scrape_interval:     5s # Set the scrape interval to every 5 seconds. Default is every 1 minute.
  evaluation_interval: 5s # Evaluate rules every 5 seconds. The default is every 1 minute.
  # scrape_timeout is set to the global default (10s).

# YugabyteDB configuration to scrape Prometheus time-series metrics
scrape_configs:
  - job_name: 'yugabytedb'
    metrics_path: /prometheus-metrics

    static_configs:
      - targets: ['127.0.0.1:7000', '127.0.0.2:7000', '127.0.0.3:7000']
        labels:
          group: 'yb-master'

      - targets: ['127.0.0.1:9000', '127.0.0.2:9000', '127.0.0.3:9000']
        labels:
          group: 'yb-tserver'

      - targets: ['127.0.0.1:11000', '127.0.0.2:11000', '127.0.0.3:11000']
        labels:
          group: 'yedis'

      - targets: ['127.0.0.1:12000', '127.0.0.2:12000', '127.0.0.3:12000']
        labels:
          group: 'ycql'

      - targets: ['127.0.0.1:13000', '127.0.0.2:13000', '127.0.0.3:13000']
        labels:
          group: 'ysql'
```

## 4. Start Prometheus server

Go to the directory where Prometheus is installed and start the Prometheus server as below.

```sh
$ ./prometheus --config.file=yugabytedb.yml
```

Open the Prometheus UI at http://localhost:9090 and then navigate to the Targets page under Status.

![Prometheus Targets](/images/ce/prom-targets.png)

## 5. Analyze key metrics

On the Prometheus Graph UI, you can now plot the read IOPS and write IOPS for the `CassandraKeyValue` sample app. As you can see from the [source code](https://github.com/yugabyte/yugabyte-db/blob/master/java/yb-loadtester/src/main/java/com/yugabyte/sample/apps/CassandraKeyValue.java) of the app, it uses only SELECT statements for reads and INSERT statements for writes (aside from the initial CREATE TABLE). This means you can measure throughput and latency by simply using the metrics corresponding to the SELECT and INSERT statements.

Paste the following expressions into the **Expression** box and click **Execute** followed by **Add Graph**.

### Throughput

> Read IOPS

```sh
sum(irate(handler_latency_yb_cqlserver_SQLProcessor_SelectStmt_count[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-read-iops.png)

>  Write IOPS

```sh
sum(irate(handler_latency_yb_cqlserver_SQLProcessor_InsertStmt_count[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-write-iops.png)

### Latency

>  Read Latency (in microseconds)

```sh
avg(irate(handler_latency_yb_cqlserver_SQLProcessor_SelectStmt_sum[1m])) / avg(irate(handler_latency_yb_cqlserver_SQLProcessor_SelectStmt_count[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-read-latency.png)

> Write Latency (in microseconds)

```sh
avg(irate(handler_latency_yb_cqlserver_SQLProcessor_InsertStmt_sum[1m])) / avg(irate(handler_latency_yb_cqlserver_SQLProcessor_InsertStmt_count[1m]))
```

![Prometheus Read IOPS](/images/ce/prom-write-latency.png)

## 6. [Optional] Clean up

Optionally, you can shut down the local cluster created in Step 1.

```sh
$ ./bin/yb-ctl destroy
```
