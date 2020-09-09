---
title: Benchmark YSQL performance with YCSB
headerTitle: YCSB
linkTitle: YCSB
description: Benchmark YSQL performance with YCSB using the new YSQL binding.
headcontent: Benchmark YSQL performance using YCSB.
menu:
  latest:
    identifier: ycsb-2-ysql
    parent: benchmark
    weight: 5
aliases:
  - /benchmark/ycsb/
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./ycsb-jdbc.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      JDBC Binding
    </a>
  </li>

  <li >
    <a href="{{< relref "./ycsb-ysql.md" >}}" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL Binding
    </a>
  </li>

  <li >
    <a href="{{< relref "./ycsb-ycql.md" >}}" class="nav-link">
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

This uses a new YSQL-specific binding to test the YSQL API using the YCSB benchmark.

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
Make sure you have the YSQL shell `ysqlsh` exported to the `PATH` variable. You can download [`ysqlsh`](https://download.yugabyte.com/) if you do not have it.
```sh
$ export PATH=$PATH:/path/to/ysqlsh
```

### 2. Start YugabyteDB

Start your YugabyteDB cluster by following the steps [here](../../deploy/manual-deployment/).

{{< tip title="Tip" >}}
You will need the IP addresses of the nodes in the cluster for the next step.
{{< /tip>}}

### 3. Configure `db.properties`

Update the file `db.properties` in the YCSB directory with the following contents. Remember to put the correct values for the IP addresses in the `db.url` field.

```sh
db.driver=org.postgresql.Driver
db.url=jdbc:postgresql://<ip1>:5433/ycsb;jdbc:postgresql://<ip2>:5433/ycsb;jdbc:postgresql://<ip3>:5433/ycsb;
db.user=yugabyte
db.passwd=
```

Details about other configuration parameters are described in [Core Properties](https://github.com/brianfrankcooper/YCSB/wiki/Core-Properties) on the [YCSB](https://github.com/brianfrankcooper/YCSB/) repository.

{{< note title="Note" >}}
The db.url field should be populated with the IPs of all the nodes that are part of the cluster.
{{< /note >}}

### 4. Run the benchmark
There is a handy script `run_ysql.sh` that loads and runs all the workloads.

```sh
$ ./run_ysql.sh --ip <ip>
```

The above command workload will run the workload on a table with 1 million rows. If you want to run the benchmark on a table with a different row count:
```sh
$ ./run_ysql.sh --ip <ip> --recordcount <number of rows>
```

{{< note title="Note" >}}
To get the maximum performance out of the system, you would have to tune the threadcount parameter in the script. As a reference, for a c5.4xlarge instance with 16 cores and 32GB RAM, you used a threadcount of 32 for the loading phase and 256 for the execution phase.
{{< /note >}}

### 5. Verify results

The script creates 2 result files per workload, one for the loading and one for the execution phase with the details of throughput and latency.
For example for workloada it creates `workloada-ysql-load.dat` and `workloada-ysql-transaction.dat`

### 6. Run individual workloads (optional)

Connect to the database using `ysqlsh`.
```sh
$ ./bin/ysqlsh -h <ip>
```

Create the `ycsb` database.
```postgres
yugabyte=# CREATE DATABASE ycsb;
```

Connect to the created database.
```postgres
yugabyte=# \c ycsb
```

Create the table.
```postgres
ycsb=# CREATE TABLE usertable (
           YCSB_KEY VARCHAR(255) PRIMARY KEY,
           FIELD0 TEXT, FIELD1 TEXT, FIELD2 TEXT, FIELD3 TEXT,
           FIELD4 TEXT, FIELD5 TEXT, FIELD6 TEXT, FIELD7 TEXT,
           FIELD8 TEXT, FIELD9 TEXT);
```

Before starting the `yugabyteSQL` workload, you will need to load the data first.

```sh
$ ./bin/ycsb load yugabyteSQL -s \
      -P db.properties           \
      -P workloads/workloada     \
      -p recordcount=1000000     \
      -p operationcount=10000000 \
      -p threadcount=32          \
      -p maxexecutiontime=180
```

Then, you can run the workload:

```sh
$ ./bin/ycsb run yugabyteSQL -s  \
      -P db.properties           \
      -P workloads/workloada     \
      -p recordcount=1000000     \
      -p operationcount=10000000 \
      -p threadcount=256         \
      -p maxexecutiontime=180
```

To run the other workloads (for example, `workloadb`), all you need to do is change that argument in the above command.

```sh
$ ./bin/ycsb run yugabyteSQL -s  \
      -P db.properties           \
      -P workloads/workloadb     \
      -p recordcount=1000000     \
      -p operationcount=10000000 \
      -p threadcount=256         \
      -p maxexecutiontime=180
```

## Expected results

### Setup

When run on a 3-node cluster with each node on a c5.4xlarge AWS instance (16 cores, 32 GB of RAM, and 2 EBS volumes), all belonging to the same AZ with the client VM running in the same AZ, you get the following results:

### 1 Million Rows

| Workload | Throughput (ops/sec) | Read Latency | Write Latency
-------------|-----------|------------|------------|
Workload A | 37,377 | 1.5ms | 12 ms update
Workload B | 66,875 | 4ms | 7.6ms update
Workload C | 77,068 | 3.5ms read | Not applicable
Workload D | 63,676 | 4ms | 7ms insert
Workload E | 63,686 | 3.8ms scan | Not applicable
Workload F | 29,500 | 2ms | 15ms read-modify-write
