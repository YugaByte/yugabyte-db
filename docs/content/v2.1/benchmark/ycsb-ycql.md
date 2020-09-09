---
title: Benchmark YCQL performance using YCSB
headerTitle: YCSB
linkTitle: YCSB
description: Benchmark YCQL performance with YCSB using the new YCQL binding.
headcontent: Benchmark YCQL performance using YCSB.
block_indexing: true
menu:
  v2.1:
    identifier: ycsb-3-ycql
    parent: benchmark
    weight: 5
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/benchmark/ycsb-jdbc/" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      JDBC Binding
    </a>
  </li>

  <li >
    <a href="/latest/benchmark/ycsb-ysql/" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL Binding
    </a>
  </li>

  <li >
    <a href="/latest/benchmark/ycsb-ycql/" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL Binding
    </a>
  </li>

</ul>

{{< note title="Note" >}}

For more information about YCSB, see:

* YCSB Wiki: https://github.com/brianfrankcooper/YCSB/wiki
* Workload info: https://github.com/brianfrankcooper/YCSB/wiki/Core-Workloads

{{< /note >}}

## Overview

This uses a new YCQL-specific binding to test the YCQL API using the YCSB benchmark.

## Running the benchmark

### 1. Prerequisites

{{< note title="Note" >}}
The binaries are compiled with JAVA 13 and it is recommended to run these binaries with that version.
{{< /note >}}

Download the YCSB binaries. You can do this by running the following commands.

```sh
$ cd $HOME
$ wget https://github.com/yugabyte/YCSB/releases/download/1.0/ycsb.tar.gz
$ tar -zxvf ycsb.tar.gz
$ cd YCSB
```

Make sure you have the YCQL shell `ycqlsh` exported to the `PATH` variable. You can download [`ycqlsh`](https://download.yugabyte.com/) if you do not have it.

```sh
$ export PATH=$PATH:/path/to/ycqlsh
```

### 2. Start YugabyteDB

Start your YugabyteDB cluster by following the steps [here](../../deploy/manual-deployment/).

{{< tip title="Tip" >}}

You will need the IP addresses of the nodes in the cluster for the next step.

{{< /tip>}}

### 3. Configure `db.properties`

Update the file `db.properties` in the YCSB directory with the following contents. Remember to put the correct value for the IP address in the `hosts` field.

```sh
hosts=<ip>
port=9042
cassandra.username=yugabyte
```

For details on other configuration parameters, like username, password, connection parameters, etc., see [YugabyteCQL binding](https://github.com/yugabyte/YCSB/tree/master/yugabyteCQL).

### 4. Run the benchmark

There is a handy script `run_ycql.sh` that loads and runs all the workloads.

```sh
$ ./run_ycql.sh --ip <ip>
```

The above command workload will run the workload on a table with 1 million rows. If you want to run the benchmark on a table with a different row count:

```sh
$ ./run_ycql.sh --ip <ip> --recordcount <number of rows>
```

{{< note title="Note" >}}

To get the maximum performance out of the system, you would have to tune the `threadcount` parameter in the script. As a reference, for a c5.4xlarge instance with 16 cores and 32 GB RAM, we used a `threadcount` of `32` for the loading phase and `256` for the execution phase.

{{< /note >}}

### 5. Verify results

The script creates 2 result files per workload, one for the loading and one for the execution phase with the details of throughput and latency.
For example, for workloada, it creates `workloada-ycql-load.dat` and `workloada-ycql-transaction.dat`

## 6. Run individual workloads (optional)

Connect to the database using `ycqlsh`.

```sh
$ ./bin/ycqlsh <ip>
```

Create the `ycsb` keyspace.

```postgres
ycqlsh> CREATE KEYSPACE ycsb;
```

Connect to the created keyspace.

```postgres
ycqlsh> USE ycsb;
```

Create the table.

```postgres
ycqlsh:ycsb> create table usertable (
                y_id varchar primary key,
                field0 varchar, field1 varchar, field2 varchar, field3 varchar,
                field4 varchar, field5 varchar, field6 varchar, field7 varchar,
                field8 varchar, field9 varchar);
```

Before starting the `yugabyteCQL` workload, you first need to load the data.

```sh
$ ./bin/ycsb load yugabyteCQL -s \
      -P db.properties           \
      -P workloads/workloada     \
      -p recordcount=1000000     \
      -p operationcount=10000000 \
      -p threadcount=256         \
      -p maxexecutiontime=180
```

Then, you can run the workload:

```sh
$ ./bin/ycsb run yugabyteCQL -s  \
      -P db.properties           \
      -P workloads/workloada     \
      -p recordcount=1000000     \
      -p operationcount=10000000 \
      -p threadcount=256         \
      -p maxexecutiontime=180
```

To run the other workloads (for example, `workloadb`), all you need to do is change that argument in the above command.

```sh
$ ./bin/ycsb run yugabyteCQL -s  \
      -P db.properties           \
      -P workloads/workloadb     \
      -p recordcount=1000000     \
      -p operationcount=10000000 \
      -p threadcount=256         \
      -p maxexecutiontime=180
```

## Expected results

### Setup

When run on a 3-node cluster with each a c5.4xlarge AWS instance (16 cores, 32 GB of RAM and 2 EBS volumes) all belonging to the same AZ with the client VM running in the same AZ we get the following results:

### 1 Million Rows

| Workload | Throughput (ops/sec) | Read Latency | Write Latency
-------------|-----------|------------|------------|
Workload A | 1,08,249 | 1ms | 3.5 ms update
Workload B | 1,41,061 | 1.6ms | 4ms update
Workload C | 1,88,111 | 1.3ms | Not applicable
Workload D | 1,53,165 | 1.5ms | 4.5ms insert
Workload E | 23,489 | 10ms scan | Not applicable
Workload F | 80,451 | 1ms | 5ms read-modify-write
