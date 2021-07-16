---
title: Best practices
linkTitle: Best practices
description: Best practices when using YugabyteDB
aliases:
  - /latest/quick-start/best-practices-dev/
menu:
  latest:
    identifier: best-practices-dev
    parent: develop
    weight: 582
aliases:
  - /latest/develop/best-practices/
isTocNested: 4
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="" class="nav-link active">
      <i class="icon-" aria-hidden="true"></i>
      DocDB
    </a>
  </li>
  <li >
    <a href="{{< ref "best-practices-ysql.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="{{< ref "best-practices-ycql.md" >}}" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>


Developing applications using distributed databases requires more thought about 
schemas and queries to keep a well balanced cluster, low query latency and preventing 
data hotspots. Below is a list of best of practices that you can keep in mind
while using YugabyteDB.

## YSQL compared to YCQL
The first step is which API to use. In terms of language capabilities, YCQL is a subset of YSQL.
In a nutshell, YSQL is a clear choice when apps need advanced features like triggers,procedures,relations,subqueries,joins, range sharding, client controlled transactions etc.
While if app only has few tables (that are typically large) and point reads/writes, and uses TTL, then YCQL is a better option. 


## Hardware sizing
See [hardware sizing](/latest/deploy/checklist/) docs.


## Tablet leaders in multi-az and multi-region deployments
In multi-region deployments, we can hint the database to try and keep all tablet-leaders
in 1 region thus.

This will lower the latency and network hops during read/write transactions. If your app is running in only 1 region, tablet-leaders residing 
in the same region helps cut latency between app<>db.
This can be done using the `set_preferred_zones` command of [yb-admin](../../admin/yb-admin) cli.

## Settings for ci/cd/integration-tests:
Using YugabyteDB in (ci,cd,automated tests) scenarios we can set certain gflags to increase performance:

1. Point gflag `--fs_data_dirs` to a ramdisk directory. 
This will make DML,DDL and create,destroy a cluster faster because data is not written to disk
2. Set gflag `--yb_num_shards_per_tserver=1`.
Reducing the number of shards lowers overhead when creating,dropping YCQL tables and writing,reading small amounts of data
3. Set gflag `--ysql_num_shards_per_tserver=1`.
Reducing the number of shards lowers overhead when creating,dropping YSQL tables and writing,reading small amounts of data
4. Set gflag `--replication_factor=1`.
For these testing scenarios, perhaps the default of keeping the data 3-way replicated is not necessary. Reducing that down to 1 cuts space usage and increases perf.
5. Use `TRUNCATE table1,table2,table3..tablen;` instead of `CREATE TABLE` and `DROP TABLE` between test cases. 

## Single AZ deployments
In single AZ deployments, you need to set `--durable_wal_write=true` [gflag](../../reference/configuration/yb-tserver) in 
tserver to not lose data if the whole datacenter goes down (power failure etc).

## Use Read Replicas for low latency timeline consistent reads in multi-region deployments
In a YugabyteDB multi-region deployment, replication of data between nodes of your primary cluster runs synchronously and guarantees strong consistency. 

Optionally, you can [create a read replica cluster](../deploy/multi-dc/read-replica-clusters.md) that asynchronously replicates data from the primary cluster and guarantees timeline 
consistency (with bounded staleness). A synchronously replicated primary cluster can accept writes to the system. 
Using a read replica cluster allows applications to serve low latency reads in remote regions.

In a read replica cluster, read replicas are observer nodes that do not participate in writes, 
but get a timeline-consistent copy of the data through asynchronous replication from the primary cluster.

Reach out on slack/forum for help with your data-schemas and how to better integrate best practices in your project.

{{< note title="Note" >}}
Reading from followers does not yet work for either YSQL or transactional tables in YCQL.
{{< /note >}}
