---
title: Benchmark YSQL performance using sysbench
headerTitle: sysbench
linkTitle: sysbench
description: Benchmark YSQL performance using sysbench.
headcontent: Benchmark YSQL performance using sysbench.
menu:
  latest:
    identifier: sysbench-ysql
    parent: benchmark
    weight: 5
aliases:
  - /benchmark/sysbench/
showAsideToc: true
isTocNested: true
---
<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./sysbench-ysql.md" >}}" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

</ul>

## Overview
sysbench is a popular tool for benchmarking databases like Postgres and MySQL, as well as system capabilities like CPU, memory and I/O. Follow the steps below to run Sysbench against YugabyteDB.

The [YugabyteDB version of sysbench](https://github.com/yugabyte/sysbench) is forked from the [official](https://github.com/akopytov/sysbench) version with a few modifications to better reflect YugabyteDB's distributed nature.

{{< note title="Note" >}}
To ensure the recommended hardware requirements are met and the database is correctly configured before benchmarking, review the [deployment checklist](../../deploy/checklist/)  
{{< /note >}}

## Running the benchmark

### 1. Prerequisites

Install sysbench using the following steps.

```sh
$ cd $HOME
$ git clone https://github.com/yugabyte/sysbench.git
$ cd sysbench
$ ./autogen.sh && ./configure --with-pgsql && make -j && sudo make install
```

{{< note title="Note" >}}
The above steps will install the sysbench utility in '/usr/local/bin'
{{< /note >}}

Make sure you have the YSQL shell `ysqlsh` exported to the `PATH` variable. You can download [`ysqlsh`](https://download.yugabyte.com/) if you do not have it.
```sh
$ export PATH=$PATH:/path/to/ysqlsh
```

### 2. Start YugabyteDB
Start your YugabyteDB cluster by following the steps [here](../../deploy/manual-deployment/).

{{< tip title="Tip" >}}
You will need the IP addresses of the nodes in the cluster for the next step.
{{< /tip>}}

### 3. Run the benchmark
There is a handy shell script `run_sysbench.sh` that loads the data and runs the various workloads.
```sh
./run_sysbench.sh --ip <ip>
```
This script runs all the 8 workloads using 64 threads with the number of tables as 10 and the table size as 100k. If you want to run the benchmark with a different count of tables and tablesize:
```sh
./run_sysbench.sh --ip <ip> --numtables <number of tables> --tablesize <number of rows in each table>
```

### 4. Run individual workloads (optional)

This section outlines instructions in case you need to run workloads individually. Before starting the workload you need to load the data first.

```sh
$ sysbench oltp_point_select        \
      --tables=10                   \
      --table-size=100000           \
      --range_key_partitioning=true \
      --db-driver=pgsql             \
      --pgsql-host=127.0.0.1        \
      --pgsql-port=5433             \
      --pgsql-user=yugabyte         \
      --pgsql-db=yugabyte           \
      prepare
```

Then you can run the workload as follows

```sh
$ sysbench oltp_point_select        \
      --tables=10                   \
      --table-size=100000           \
      --range_key_partitioning=true \
      --db-driver=pgsql             \
      --pgsql-host=127.0.0.1        \
      --pgsql-port=5433             \
      --pgsql-user=yugabyte         \
      --pgsql-db=yugabyte           \
      --threads=64                  \
      --time=120                    \
      --warmup-time=120             \
      run
```

The choice of different workloads are:
* oltp_insert
* oltp_point_select
* oltp_write_only
* oltp_read_only
* oltp_read_write
* oltp_update_index
* oltp_update_non_index
* oltp_delete

## Expected results

### Setup

When run on a 3-node cluster with each node on a c5.4xlarge AWS instance (16 cores, 32 GB of RAM, and 2 EBS volumes), all belonging to the same AZ with the client VM running in the same AZ, you get the following results:

### 10 Tables Each with 100k Rows

| Workload   | Throughput (txns/sec) | Latency (ms)
-------------|-----------|----------|
OLTP_READ_ONLY | 3276 | 39
OLTP_READ_WRITE | 487 | 265
OLTP_WRITE_ONLY | 1818 | 70
OLTP_POINT_SELECT| 95695 | 1.3
OLTP_INSERT | 6348 | 20.1
OLTP_UPDATE_INDEX | 4052 | 31
OLTP_UPDATE_NON_INDEX | 11496 | 11
OLTP_DELETE | 67499 | 1.9
