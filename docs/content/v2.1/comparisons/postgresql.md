---
title: Compare PostgreSQL with YugabyteDB
headerTitle: PostgreSQL
linkTitle: PostgreSQL
description: Compare PostgreSQL with YugabyteDB.
block_indexing: true
menu:
  v2.1:
    parent: comparisons
    weight: 1128
isTocNested: false
showAsideToc: true
---

## The basics

PostgreSQL is a single-node relational database (also known as RDBMS). Tables are not horizontally partitioned/sharded into smaller components since all data is served from the single node. On the other hand, YugabyteDB is an auto-sharded distributed database where shards are automatically placed on different nodes altogether. The benefit is that no single node can become the performance or availability bottleneck while serving data across different shards. This concept is key to how YugabyteDB achieves horizontal write scalability that native PostgreSQL lacks. Additional nodes lead to the shards getting rebalanced across all the nodes so that the additional compute capacity can be more thoroughly utilized.

There is a concept of “partitioned tables” in PostgreSQL that can make horizontal data partitioning/sharding confusing to PostgreSQL developers. First introduced in PostgreSQL 10, partitioned tables enable a single table to be broken into multiple child tables so that these child tables can be stored on separate disks (tablespaces). Serving of the data however is still performed by a single node. Hence this approach does not offer any of the benefits of an auto-sharded distributed database such as YugabyteDB.

## Continuous availability

The most common replication mechanism in PostgreSQL is that of asynchronous replication. Two completely independent database instances are deployed in a master-slave configuration in such a way that the slave instances periodically receive committed data from the master instance. The slave instance does not participate in the original writes to the master, thus making the latency of write operations low from an application client standpoint. However, the true cost is loss of availability (until manual failover to slave) as well as inability to serve recently committed data when the master instance fails (given the data lag on the slave). The less common mechanism of synchronous replication involves committing to two independent instances simultaneously. It is less common because of the complete loss of availability when one of the instances fail. Thus, irrespective of the replication mechanism used, it is impossible to guarantee always-on, strongly-consistent reads in PostgreSQL.

YugabyteDB is designed to solve the high availability need that monolithic databases such as PostgreSQL were never designed for. This inherently means committing the updates at 1 more independent failure domain than compared to PostgreSQL. There is no overall “leader” node in YugabyteDB that is responsible for handing updates for all the data in the database. There are multiple shards and those shards are distributed among the multiple nodes in the cluster. Each node has some shard leaders and some shard followers. Serving writes is the responsibility of a shard leader which then uses Raft replication protocol to commit the write at at least 1 more follower replica before acknowledging the write as successful back to the application client. When a node fails, some shard leaders will be lost but the remaining two follower replicas (on still available nodes) will elect a new leader automatically in a few seconds. Note that the replica that had the latest data gets the priority in such an election. This leads to extremely low write unavailability and essentially a self-healing system with auto-failover characteristics.

Most distributed databases do a quorum read to serve more consistent data but increase the latency of the operation since a majority of replicas located in potentially geo-distributed nodes now have to coordinate. YugabyteDB does not face this problem since it can serve strongly consistent reads directly from the shard leader without any quorum. It achieves this by implementing the notion of leader-leases to ensure that even in case a new leader is elected for a shard, the old leader doesn’t continue to serve potentially stale data. Serving low latency reads for internet-scale OLTP workloads thus becomes really easy.

## Distributed ACID transactions

PostgreSQL can be thought of as a single-shard database which means it supports for single row (e.g. an INSERT statement) and single shard transactions (e.g. database operations bounded by BEGIN TRANSACTION and END TRANSACTION). The notion of multiple shards is not applicable to PostgreSQL and as a result, multi-shard transactions too are not applicable. On the other hand, YugabyteDB takes inspiration from Google Spanner, Google’s globally distributed database, and supports all the 3 flavors of transactions. As described in “Yes We Can! Distributed ACID Transactions with High Performance”, it is designed to ensure the single row/shard transactions can be served with lowest latency possible while the distributed transactions can be served with absolute correctness.

## Relevant blog posts

The following posts cover some more details around how YugabyteDB differs from PostgreSQL.

- [Mapping YugabyteDB Concepts to PostgreSQL and MongoDB](https://blog.yugabyte.com/mapping-yugabyte-db-concepts-to-postgresql-and-mongodb/)
- [YugabyteDB High Availability & Transactions for PostgreSQL & MongoDB Developers](https://blog.yugabyte.com/mapping-yugabyte-db-concepts-to-postgresql-and-mongodb/)
- [Distributed PostgreSQL on a Google Spanner Architecture – Query Layer](https://blog.yugabyte.com/distributed-postgresql-on-a-google-spanner-architecture-query-layer/)
- [Distributed PostgreSQL on a Google Spanner Architecture – Storage Layer](https://blog.yugabyte.com/distributed-postgresql-on-a-google-spanner-architecture-storage-layer/)
