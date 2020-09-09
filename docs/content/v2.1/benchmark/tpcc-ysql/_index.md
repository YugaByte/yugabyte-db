---
title: Benchmark YSQL performance using TPC-C
headerTitle: TPC-C
linkTitle: TPC-C
description: Benchmark YSQL performance using TPC-C
headcontent: Benchmark YugabyteDB using TPC-C
image: /images/section_icons/quick_start/explore_ysql.png
block_indexing: true
menu:
  v2.1:
    identifier: tpcc-ysql
    parent: benchmark
    weight: 4
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/benchmark/tpcc-ysql/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

</ul>

## Overview
Follow the steps below to run the open-source [oltpbench](https://github.com/oltpbenchmark/oltpbench) TPC-C workload against YugabyteDB YSQL. [TPC-C](http://www.tpc.org/tpcc/) is a popular online transaction processing benchmark that provides metrics you can use to evaluate the performance of YugabyteDB for concurrent transactions of different types and complexity that are either either executed online or queued for deferred execution.

## 1. Prerequisites

### Get TPC-C binaries

To download the TPC-C binaries, run the following commands.

```sh
$ cd $HOME
$ wget https://github.com/yugabyte/tpcc/releases/download/1.1/tpcc.tar.gz
$ tar -zxvf tpcc.tar.gz
$ cd tpcc
```

{{< note title="Note" >}}
The binaries are compiled with JAVA 13 and it is recommended to run these binaries with that version.
{{< /note >}}

### Start the Database

Start your YugabyteDB cluster by following the steps [here](../../deploy/manual-deployment/).

{{< tip title="Tip" >}}
You will need the IP addresses of the nodes in the cluster for the next step.
{{< /tip>}}


### Configure DB connection params (optional)

If not working with the defaults, we can change the username, password, port, etc. using the configuration file at `config/workload_all.xml`. We can also change the terminals or the physical connections being used by the benchmark using the configuration.

```sh
<dbtype>postgres</dbtype>
<driver>org.postgresql.Driver</driver>
<port>5433</5433>
<username>yugabyte</username>
<password></password>
<isolation>TRANSACTION_REPEATABLE_READ</isolation>

<terminals>100</terminals>
<numDBConnections>10</numDBConnections>
```

{{< note title="Note" >}}
By default the number of terminals is 10 times the number of warehouses which is the max that the TPC-C spec allows. The number of DB connections is the same as the number of warehouses.
{{< /note >}}

## 2. Run the TPC-C benchmark

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#10-wh" class="nav-link active" id="10-wh-tab" data-toggle="tab" role="tab" aria-controls="10-wh" aria-selected="true">
      10 Warehouses
    </a>
  </li>
  <li>
    <a href="#100-wh" class="nav-link" id="100-wh-tab" data-toggle="tab" role="tab" aria-controls="100-wh" aria-selected="false">
      100 Warehouses
    </a>
  </li>
  <li>
    <a href="#1000-wh" class="nav-link" id="docker-tab" data-toggle="tab" role="tab" aria-controls="docker" aria-selected="false">
      1000 Warehouses
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="10-wh" class="tab-pane fade show active" role="tabpanel" aria-labelledby="10-wh-tab">
    {{% includeMarkdown "10-wh/tpcc-ysql.md" /%}}
  </div>
  <div id="100-wh" class="tab-pane fade" role="tabpanel" aria-labelledby="100-wh-tab">
    {{% includeMarkdown "100-wh/tpcc-ysql.md" /%}}
  </div>
  <div id="1000-wh" class="tab-pane fade" role="tabpanel" aria-labelledby="1000-wh-tab">
    {{% includeMarkdown "1000-wh/tpcc-ysql.md" /%}}
  </div>
</div>
