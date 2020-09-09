---
title: Layered architecture for queries and storage
headerTitle: Layered architecture
linkTitle: Layered architecture
description: Learn about the layered architecture of YugabyteDB in the query layer and the storage layer.
aliases:
  - /stable/architecture/layered-architecture/
block_indexing: true
menu:
  stable:
    identifier: architecture-layered-architecture
    parent: architecture
    weight: 1109
isTocNested: true
showAsideToc: true
---

YugabyteDB architecture follows a layered design. It is comprised of 2 logical layers as shown in the diagram below:

* **Yugabyte Query Layer**
* **DocDB** distributed document store

![YugabyteDB Logical Architecture](/images/architecture/yb-arch-new.png)

## Yugabyte Query Layer

The [Yugabyte Query Layer or YQL](../query-layer/) is the upper layer of YugabyteDB. Applications interact directly with YQL using client drivers. This layer deals with the API specific aspects such as query/command compilation and the run-time (data type representations, built-in operations and more). YQL is built with extensibility in mind, and allows for new APIs to be added.

Currently, YQL supports two flavors of distributed SQL APIs.

### Yugabyte SQL (YSQL)

YSQL is a distributed SQL API that is built by re-using the PostgreSQL language layer code. It is a stateless SQL query engine that is wire-format compatible with PostgreSQL.

### Yugabyte Cloud QL (YCQL)

YCQL is a semi-relational language that has its roots in Cassandra Query Language. It is a SQL-like language built specifically to be aware of clustering of data across nodes.

{{< tip title="Read More" >}}
Understanding [the design of the query layer](../query-layer/overview/).
{{< /tip >}}

## DocDB

[DocDB](../docdb/) is a distributed document store. It has the following properties:

* [Strong write consistency](../docdb/replication/#strong-write-consistency)
* Extremely resilient to failures
* Automatic sharding and load balancing
* Zone/region/cloud aware data placement policies
* [Tunable read consistency](../docdb/replication/#tunable-read-consistency)

Data in DocDB is stored in tables. Each table is composed of rows, each row contains a key and a document. Here are some key points:

### Sharding

Data is stored inside tables in DocDB. A DocDB table is often sharded into a number of **tablets**. This sharding of tables is transparent to users.

You can read more about [how sharding works in DocDB](../docdb/sharding/).

### Replication

Each tablet consisting of user data is replicated according to some replication factor using the Raft consensus algorithm. Replication is performed at a tablet level, and ensures single row linearizability even in the presence of failures.

You can read more about [how replication works in DocDB](../docdb/replication/).

### Persistence

In order to persist data, a log-structured row/document-oriented storage is used. It includes several optimizations for handling ever-growing datasets efficiently.

You can read more about [how persistence of data works in DocDB](../docdb/persistence/).

### Transactions

DocDB has support for both single-row and multi-row transactions. This means that DocDB allows modifying multiple keys while preserving ACID properties.

* Read about the [isolation levels supported in DocDB](../transactions/isolation-levels/).
* [Single-row transactions](../transactions/single-row-transactions/).
* [Multi-row transactions](../transactions/distributed-txns/).

## What's next

You can now read about the following:

{{< note title="" >}}

* [The design goals of YugabyteDB](../design-goals/)
* [Architecture of DocDB](../docdb/)
* [Transactions in DocDB](../transactions/)
* [Design of the query layer](../query-layer/)
* [How various functions work, like the read and write IO paths](../core-functions/)

{{< /note >}}
