---
title: Colocated tables
headerTitle: Colocated tables
linkTitle: Colocated tables
description: Learn about how colocated tables aggregate data into a single tablet.
aliases:
  - /stable/architecture/docdb/colocated_tables/
block_indexing: true
menu:
  stable:
    identifier: docdb-colocated-tables
    parent: architecture-docdb-sharding
    weight: 1144
isTocNested: false
showAsideToc: true
---

In workloads that need lower throughput and have a small data set, the bottleneck shifts from CPU/disk/network to the number of tablets that should be hosted per node. Since each table by default requires at least one tablet per node, a YugabyteDB cluster with 5000 relations (which includes tables and indexes) will result in 5000 tablets per node. There are practical limitations to the number of tablets that YugabyteDB can handle per node since each tablet adds some CPU, disk, and network overhead. If most or all of the tables in YugabyteDB cluster are small tables, then having separate tablets for each table unnecessarily adds pressure on CPU, network and disk.

To help accommodate such relational tables and workloads, YugabyteDB supports colocating SQL tables. Colocating tables puts all of their data into a single tablet, called the colocation tablet. This can dramatically increase the number of relations (tables, indexes, etc) that can be supported per node while keeping the number of tablets per node low. Note that all the data in the colocation tablet is still replicated across three nodes (or whatever the replication factor is). Large tablets can be dynamically split at a future date if there is need to serve more throughput over a larger data set.

## Motivation

This feature is desirable in a number of scenarios, some of which are described below.

### Small datasets needing HA or geo-distribution

Applications that have a smaller dataset may fall into the following pattern:

- They require large number of tables, indexes and other relations created in a single database.
- The size of the entire dataset is small. Typically, this entire database is less than 500 GB in size.
- Need high availability and/or geographic data distribution.
- Scaling the dataset or the number of IOPS is not an immediate concern.

In this scenario, it is undesirable to have the small dataset spread across multiple nodes because
this might affect performance of certain queries due to more network hops (for example, joins).

**Example:** User identity service for a global application. The user dataset size may not be too
large, but is accessed in a relational manner, requires high availability and might need to be
geo-distributed for low latency access.

### Large datasets - a few large tables with many small tables

Applications that have a large dataset may fall into the pattern where:

- They need a large number of tables and indexes.
- A handful of tables are expected to grow large, needing to be scaled out.
- The rest of the tables will continue to remain small.

In this scenario, only the few large tables would need to be sharded and scaled out. All other tables would benefit from colocation because queries involving all tables, except the larger ones, would not need network hops.

**Example:** An IoT use case, where one table records the data from the IoT devices while there are a number of other tables that store data pertaining to user identity, device profiles, privacy, etc.

### Scaling the number of databases, each database with a small dataset

There may be scenarios where the number of databases grows rapidly, while the dataset of each database is small.
This is characteristic of a microservices-oriented architecture, where each microservice needs its own database. These microservices are hosted in dev, test, staging, production and other environments. The net result is a lot of small databases, and the need to be able to scale the number of databases hosted. Colocated tables allow for the entire dataset in each database to be hosted in one tablet, enabling scalability of the number of databases in a cluster by simply adding more nodes.

**Example:** Multi-tenant SaaS services where one database is created per customer. As new customers are rapidly on-boarded, it becomes necessary to add more databases quickly while maintaining high-availability and fault-tolerance of each database.

## Tradeoffs

Fundamentally, colocated tables have the following tradeoffs:

- **Higher performance - no network reads for joins**.
All of the data across the various colocated tables is local, which means joins no longer have to
read data over the network. This improves the speed of joins.

- **Support higher number of tables - using fewer tablets**.
Because multiple tables and indexes can share one underlying tablet, a much higher number of tables
can be supported using colocated tables.

- **Lower scalability - until removal from colocation tablet**.
The assumptions behind tables that are colocated is that their data need not be automatically sharded and distributed across nodes. If it is known a priori that a table will get large, it can be opted out of the colocation tablet at creation time. If a table already present in the colocation tablet gets too large, it can dynamically be removed from the colocation tablet to enable splitting it into multiple tablets, allowing it to scale across nodes.

## Usage

To learn more about using this feature, see [Explore colocated tables](../../../explore/colocated-tables/).

## What's next?

For more information, see the architecture for [colocated tables](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/ysql-colocated-tables.md).
