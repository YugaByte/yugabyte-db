---
title: Benchmark YCQL performance with key-value workloads
headerTitle: Key-value workload
linkTitle: Key-value workload
description: Benchmark YCQL performance with key-value workloads.
image: /images/section_icons/explore/high_performance.png
headcontent: Test YugabyteDB performance with a key-value workload.
aliases:
  - /latest/benchmark/performance/
  - /latest/benchmark/key-value-workload
menu:
  latest:
    identifier: key-value-workload-1-ycql
    parent: benchmark
    weight: 6
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./key-value-workload-ysql.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="{{< relref "./key-value-workload-ycql.md" >}}" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

Use this benchmark to test the performance of YugabyteDB using a key-value workload.

## Recommended configuration

### Cluster configuration

For this benchmark, you will set up a three-node YugabyteDB cluster with a replication factor of `3`.

### Machine configuration

- Amazon Web Services (AWS)

  - Instance type: i3.4xlarge
  - Storage: 2 x 1.9 TB NVMe SSD (comes preconfigured with the instance)

- Google Cloud Platform (GCP)

  - Instance type: n1-standard-16
  - Storage: 2 x 375 GB SSD

- on-premises data center

  - Instance: 16 CPU cores
  - Storage: 1 x 200 GB SSD (minimum)
  - RAM size: 30 GB (minimum)

### Benchmark tool

We will use the [YugabyteDB Workload Generator](https://github.com/yugabyte/yb-sample-apps) to perform this benchmark. 

To get the tool (``yb-sample-apps.jar`), run the following command.

```sh
$ wget https://github.com/yugabyte/yb-sample-apps/releases/download/1.3.1/yb-sample-apps.jar?raw=true -O yb-sample-apps.jar
```

To run the workload generator tool, you must have:

- Java runtime or JDK installed.
- Set the environment variable  $ENDPOINTS to the IP addresses (including hosts and ports) for the nodes of the cluster.

```
ENDPOINTS="X.X.X.X:9042,X.X.X.X:9042,X.X.X.X:9042"
```

## Run the write-heavy key-value workload

Run the key-value workload with higher number of write threads (representing write-heavy workload).  

Load 1B keys of 256 bytes each across 256 writer threads

```sh
$ java -jar ./yb-sample-apps.jar  \
      --workload CassandraKeyValue   \
      --nodes $ENDPOINTS             \
      --nouuid                       \
      --value_size 256               \
      --num_threads_read 0           \
      --num_threads_write  256       \
      --num_unique_keys 1000000000
```

### Expected results

Name    | Observation
--------|------
Write Ops/sec | ~90k
Write Latency | ~2.5-3.0 ms/op
CPU (User + Sys) | 60%

## Run the read-heavy key-value workload

Run the key-value workload with higher number of read threads (representing read-heavy workload).

Load 1M keys of 256 bytes and access them with 256 reader threads.

```sh
$ java -jar ./yb-sample-apps.jar  \
      --workload CassandraKeyValue   \
      --nodes $ENDPOINTS             \
      --nouuid                       \
      --value_size 256               \
      --num_threads_read 256         \
      --num_threads_write 0          \
      --num_unique_keys 1000000
```

### Expected results

Name    | Observation
--------|------
(Read) Ops/sec | ~150k
(Read) Latency | ~1.66 ms/op
CPU (User + Sys) | 60%

## Batch write-heavy KV workload

Run the key-value workload in batch mode and higher number of write threads (representing batched, write-heavy workload).  

Load 1B keys of 256 bytes each across 64 writer threads in batches of 25 each.

```sh
$ java -jar ./yb-sample-apps.jar      \
      --workload CassandraBatchKeyValue  \
      --nodes $ENDPOINTS                 \
      --nouuid                           \
      --batch_size 25                    \
      --value_size 256                   \
      --num_threads_read 0               \
      --num_threads_write 64             \
      --num_unique_keys 1000000000
```

### Expected results

Name    | Observation
--------|------
(Batch Write) Ops/sec | ~140k
(Batch Write) Latency | ~9.0 ms/op
CPU (User + Sys) | 80%
