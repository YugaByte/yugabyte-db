---
title: Design goals
headerTitle: Design goals
linkTitle: Design goals
description: Learn the design goals that drive the building of YugabyteDB.
aliases:
  - /latest/architecture/design-goals/
menu:
  latest:
    identifier: architecture-design-goals
    parent: architecture
    weight: 1105
isTocNested: true
showAsideToc: true
---

This page outlines the design goals with which YugabyteDB has been built.

## Consistency

YugabyteDB offers strong consistency guarantees in the face of a variety of failures. It supports distributed transactions.

### CAP theorem

In terms of the [CAP theorem](https://en.wikipedia.org/wiki/CAP_theorem), YugabyteDB is a CP database (consistent and partition tolerant), but achieves very high availability. The architectural design of YugabyteDB is similar to Google Cloud Spanner, which is also a CP system. The description about [Spanner](https://cloudplatform.googleblog.com/2017/02/inside-Cloud-Spanner-and-the-CAP-Theorem.html) is just as valid for YugabyteDB. The key takeaway is that no system provides 100% availability, so the pragmatic question is whether or not the system delivers availability that is so high that most users no longer have to be concerned about outages. For example, given there are many sources of outages for an application, if YugabyteDB is an insignificant contributor to its downtime, then users are correct to not worry about it.

### Single-row linearizability

YugabyteDB supports single-row linearizable writes. Linearizability is one of the strongest single-row consistency models, and implies that every operation appears to take place atomically and in some total linear order that is consistent with the real-time ordering of those operations. In other words, the following should be true of operations on a single row: 

- Operations can execute concurrently, but the state of the database at any point in time must appear to be the result of some totally ordered, sequential execution of operations.
- If operation A completes before operation B begins, then B should logically take effect after A.

### Multi-row ACID transactions

YugabyteDB supports multi-row transactions with both Serializable and Snapshot isolation.

- The [YSQL](../../api/ysql/) API supports both Serializable and Snapshot Isolation using the PostgreSQL isolation level syntax of `SERIALIZABLE` and `REPEATABLE READ` (default) respectively. 
- The [YCQL](../../api/ycql/dml_transaction/) API supports only Snapshot Isolation using the `BEGIN TRANSACTION` syntax.

{{< tip title="Read more about consistency" >}}

- Achieving [consistency with Raft consensus](../docdb/replication/).
- How [fault tolerance and high availability](../core-functions/high-availability/) are achieved.
- [Single-row linearizable transactions](../transactions/single-row-transactions/) in YugabyteDB.
- The architecture of [distributed transactions](../transactions/single-row-transactions/).

{{< /tip >}}

## Query APIs

YugabyteDB does not reinvent data client APIs. It is wire-compatible with existing APIs and extends functionality. The following APIs are supported.

### YSQL

[YSQL](../../api/ysql/) is a fully-relational SQL API that is wire compatible with the SQL language in PostgreSQL. It is best fit for RDBMS workloads that need horizontal write scalability and global data distribution while also using relational modeling features such as JOINs, distributed transactions and referential integrity (such as foreign keys). Note that YSQL [reuses the native query layer](https://blog.yugabyte.com/why-we-built-yugabytedb-by-reusing-the-postgresql-query-layer/) of the PostgreSQL open source project.

- New changes do not break existing PostgreSQL functionality

- Designed with migrations to newer PostgreSQL versions over time as an explicit goal. This means that new features are implemented in a modular fashion in the YugabyteDB codebase to enable rapid integration with new PostgreSQL features as an ongoing process.

- Support wide SQL functionality:
  - All data types
  - Built-in functions and expressions
  - Joins (inner join, outer join, full outer join, cross join, natural join)
  - Constraints (primary key, foreign key, unique, not null, check)
  - Secondary indexes (including multi-column and covering columns)
  - Distributed transactions (Serializable and Snapshot Isolation)
  - Views
  - Stored procedures
  - Triggers

### YCQL

[YCQL](../../api/ycql/) is a semi-relational SQL API that is best fit for internet-scale OLTP and HTAP apps needing massive write scalability as well as blazing-fast queries. It supports distributed transactions, strongly consistent secondary indexes and a native JSON column type. YCQL has its roots in the Cassandra Query Language. 

{{< tip title="Read more" >}}

Understanding [the design of the query layer](../query-layer/overview/).

{{< /tip >}}

## Performance

Written in C++ to ensure high performance and the ability to leverage large memory heaps (RAM) as an internal database cache. It is optimized primarily to run on SSDs and NVMe drives. It is designed with the following workloads in mind:

- High write throughput
- High client concurrency
- High data density (total data set size per node)
- Ability to handle ever growing event data use-cases well

{{< tip title="Read More" >}}
Achieving [high performance in YugabyteDB](../docdb/performance/).
{{< /tip >}}

## Geo-distributed deployments

### Multi-region configurations

YugabyteDB should work well in deployments where the nodes of the cluster span:

- single zone
- multiple zones
- multiple regions that are geographically replicated
- multiple clouds (both public and private clouds)

In order to achieve this, a number of features would be required. For example, client drivers across the various languages should be:

- Cluster-aware, with ability to handle node failures seamlessly
- Topology-aware, with ability to route traffic seamlessly

## Cloud native architecture

YugabyteDB is a cloud-native database. It has been designed with the following cloud-native principles in mind:

### Run on commodity hardware

- Run on any public cloud or on-premises data center. This means YugabyteDB should be able to run on commodity hardware on bare metal machines, VMs or containers.
- No hard external dependencies. For example, YugabyteDB should not rely on atomic clocks, but should be able to utilize one if available.

### Kubernetes ready

The database should work natively in Kubernetes and other containerized environments as a stateful application.

### Open source

YugabyteDB is open source under the very permissive Apache 2.0 license.

## What's next?

You can now read about the following:

{{< note title="" >}}

- [Overview of the architectural layers in YugabyteDB](../layered-architecture/)
- [Architecture of DocDB](../docdb/)
- [Transactions in DocDB](../transactions/)
- [Design of the query layer](../query-layer/)
- [How various functions work, like the read and write IO paths](../core-functions/)

{{< /note >}}
