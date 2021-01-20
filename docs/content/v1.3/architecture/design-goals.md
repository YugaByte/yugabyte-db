---
title: Design goals
linkTitle: Design goals
description: Design goals
block_indexing: true
menu:
  v1.3:
    identifier: architecture-design-goals
    parent: architecture
    weight: 1105
isTocNested: true
showAsideToc: true
---

This page outlines the design goals with which YugabyteDB has been built.

## Consistency

YugabyteDB offers strong consistency guarantees in the face of a variety of failures. It  supports distributed transactions.

### CAP theorem

In terms of the [CAP theorem](https://en.wikipedia.org/wiki/CAP_theorem), YugabyteDB is a CP database (consistent and partition tolerant), but achieves very high availability. The architectural design of Yugabyte is similar to Google Cloud Spanner, which is also a CP system. The description about [Spanner](https://cloudplatform.googleblog.com/2017/02/inside-Cloud-Spanner-and-the-CAP-Theorem.html) is just as valid for YugabyteDB. The key takeaway is that no system provides 100% availability, so the pragmatic question is whether or not the system delivers availability that is so high that most users no longer have to be concerned about outages. For example, given there are many sources of outages for an application, if YugabyteDB is an insignificant contributor to its downtime, then users are correct to not worry about it.

### Single-key linearizability

YugabyteDB supports single-key linearizable writes. Linearizability is one of the strongest single-key consistency models, and implies that every operation appears to take place atomically and in some total linear order that is consistent with the real-time ordering of those operations. In other words, the following should be true of operations on a single key: 

* Operations can execute concurrently, but the state of the database at any point in time must appear to be the result of some totally ordered, sequential execution of operations.
* If operation A completes before operation B begins, then B should logically take effect after A.

### Multi-key ACID transactions

YugabyteDB supports multi-key transactions with both Serializable and Snapshot Isolation.

* The [YSQL](../../api/ysql/) API supports both Serializable and Snapshot Isolation using the PostgreSQL isolation level syntax of `SERIALIZABLE` and `REPEATABLE READS` respectively. Note that YSQL Serializable support was added in [v1.2.6](../../releases/v1.2.6/).
* The [YCQL](../../api/ycql/dml_transaction/) API supports only Snapshot Isolation using the `BEGIN TRANSACTION` syntax.

{{< tip title="Read More about Consistency" >}}
* Achieving [consistency with Raft consensus](../docdb/replication/).
* How [fault tolerance and high availability](../core-functions/high-availability/) are achieved.
* [Single-key linearizable transactions](../transactions/single-row-transactions/) in YugabyteDB.
* The architecture of [distributed transactions](../transactions/single-row-transactions/).
{{< /tip >}}

## Query APIs

YugabyteDB does not reinvent storage APIs. It is wire-compatible with existing APIs and extends functionality. It supports the following APIs:

* **YSQL** which is being built to be ANSI-SQL compliant and is wire-compatible with PostgreSQL
* **YCQL** (or the *Yugabyte Cloud Query Language*) which is a semi-relational API with Cassandra roots

### Distributed SQL

The YSQL API is PostgreSQL-compatible as noted before. It reuses PostgreSQL code base.

* New changes do not break existing PostgreSQL functionality

* Designed with migrations to newer PostgreSQL versions over time as an explicit goal. This means that new features are implemented in a modular fashion in the YugabyteDB codebase to enable rapidly integrating with new PostgreSQL features in an on-going fashion.

* Support wide SQL functionality:
  * All data types
  * Built-in functions and expressions
  * Various kinds of joins
  * Constraints (primary key, foreign key, unique, not null, check)
  * Secondary indexes (incl. multi-column & covering columns)
  * Distributed transactions (Serializable and Snapshot Isolation)
  * Views
  * Stored Procedures
  * Triggers

{{< tip title="Read More" >}}
Understanding [the design of the query layer](../query-layer/overview/).
{{< /tip >}}

## Performance

Written in C++ to ensure high performance and the ability to leverage large memory heaps (RAM) as an internal database cache. It is optimized primarily to run on SSDs and NVMe drives. It is designed with the following workloads in mind:

* High write throughput
* High client concurrency
* High data density (total data set size per node)
* Ability to handle ever growing event data use-cases well

{{< tip title="Read More" >}}
Achieving [high performance in YugabyteDB](../docdb/performance/).
{{< /tip >}}

## Geo-distributed

### Multi-region deployments

YugabyteDB should work well in deployments where the nodes of the cluster span:

* single zone
* multiple zones
* multiple regions that are geographically replicated
* multiple clouds (both public and private clouds)

In order to achieve this, a number of features would be required. For example, client drivers across the various languages should be:

* Cluster-aware, with ability to handle node failures seamlessly
* Topology-aware, with ability to route traffic seamlessly

## Cloud native

YugabyteDB is a cloud-native database. It has been designed with the following cloud-native principles in mind:

### Run on commodity hardware

* Run on any public cloud or on-premise datacenter. This means YugabyteDB should be able to run on commodity hardware on bare metal machines, VMs or containers.

* No hard external dependencies. For example, YugabyteDB should not rely on atomic clocks, but should be able to utilize one if available.

### Kubernetes ready

The database should work natively in Kubernetes and other containerized environments as a stateful application.

### Open source

YugabyteDB is open source under the very permissive Apache 2.0 license.

## What's next?

You can now read about the following:

{{< note title="" >}}

* [Overview of the architectural layers in YugabyteDB](../layered-architecture/)
* [Architecture of DocDB](../docdb/)
* [Transactions in DocDB](../transactions/)
* [Design of the query layer](../query-layer/)
* [How various functions work, like the read and write IO paths](../core-functions/)

{{< /note >}}
