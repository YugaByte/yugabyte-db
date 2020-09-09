---
title: Universe 
headerTitle: Universe
linkTitle: Universe
description: Learn about the YugabyteDB universe (or cluster).
block_indexing: true
menu:
  v2.1:
    identifier: architecture-concepts-universe
    parent: key-concepts
    weight: 1122
isTocNested: true
showAsideToc: true
---

A YugabyteDB universe is a group of nodes (VMs, physical machines, or containers) that collectively function as a resilient and scalable distributed database.

{{< note title="Note" >}}

In most of the docs, the term `cluster` and `universe` are used interchangeably. However, the two are not always equivalent. The difference is described in a section below.

{{< /note >}}

The universe can be deployed in a variety of configurations depending on business requirements, and latency considerations. Some examples:

- Single availability zone (AZ/rack/failure domain)
- Multiple availability zones (AZs) in a region
- Multiple regions (with synchronous and asynchronous replication choices)

## Organization of user data

A YugabyteDB *universe* can consist of one or more namespaces. Each of these namespaces can contain one or more user tables.

YugabyteDB automatically shards, replicates and load balances these tables across the nodes in the universe, while respecting user intent such as cross-AZ or region placement requirements, desired replication factor, and so on. YugabyteDB automatically handles failures (such as node, disk, AZ or region failures), and re-distributes and re-replicates data back to desired levels across the remaining available nodes while still respecting any replica placement requirements.

### YSQL

Namespaces in YSQL are referred to as **databases** and are logically the same as in other RDBMS (such as PostgreSQL).

### YCQL

A namespace in YCQL is referred to as a **keyspace** and is logically the same as a keyspace in Apache Cassandra's CQL.

## Component services

A universe comprises of two sets of servers, **YB-TServer** and **YB-Master**. These sets of YB-TServer and YB-Master servers form two respective distributed services using [Raft](https://raft.github.io/) as a building block. High availability (HA) of both these services is achieved by the failure detection, leader election and data replication mechanisms in the Raft implementation.

{{< note title="Note" >}}

YugabyteDB is architected to not have any single point of failure.

{{< /note >}}

These serve different purposes as described below.

### YB-TServer

The **YB-TServer** (aka the *YugabyteDB Tablet Server*) service is responsible for hosting/serving user data (for example, tables). They deal with all the user queries.

For details, see [YB-TServer](../yb-tserver).

### YB-Master

The **YB-Master** (aka the *YugabyteDB Master Server*) service is responsible for keeping system metadata, coordinating system-wide operations, such as create/alter/drop tables, and initiating maintenance operations such as load balancing.

For details, see [YB-Master](../yb-master).

Below is an illustration of a simple 4-node YugabyteDB universe:

![4 node cluster](/images/architecture/4_node_cluster.png)

## Universe vs cluster

A YugabyteDB universe comprises of exactly one primary cluster and zero or more read replica clusters. 

- A primary cluster can perform both writes and reads. Replication between nodes in a primary cluster is performed synchronously.

- Read replica clusters can perform only reads. Writes sent to read replica clusters get automatically rerouted to the primary cluster for the universe. These clusters help in powering reads in regions that are far away from the primary cluster with timeline-consistent data. This ensures low latency reads for geo-distributed applications. Data is brought into the read replica clusters through asynchronous replication from the primary cluster. In other words, nodes in a read replica cluster act as Raft observers that do not participate in the write path involing the Raft leader and Raft followers present in the primary cluster.

For more information about read replica clusters, see [read replicas](../../docdb/replication/#read-only-replicas).
