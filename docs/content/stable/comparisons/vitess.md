---
title: Compare Vitess with YugabyteDB
headerTitle: Vitess
linkTitle: Vitess
description: Compare Vitess with YugabyteDB.
aliases:
  - /comparisons/vitess/
block_indexing: true
menu:
  stable:
    parent: comparisons
    weight: 1077
isTocNested: false
showAsideToc: true
---

Vitess is an automated sharding solution for MySQL. Each MySQL instance acts as a shard of the overall database. It uses etcd, a separate strongly consistent key-value store, to store shard location metadata such as which shard is located in which instance. Vitess uses a pool of stateless servers to route queries to the appropriate shard based on the mapping stored in etcd. Each of the instances use standard MySQL master-slave replication to account for high availability.

## No single logical SQL database

SQL features that access multiple rows of data spread across multiple shards are not allowed. Examples are distributed ACID transactions and JOINs. This means a Vitess cluster loses the notion of a single logical SQL database. Application developers have to be acutely aware of their sharding mechanism and account for those while designing their schema as well as while executing queries.

## Lack of continuous availability

Vitess does not make any enhancements to the asynchronous master-slave replication architecture of MySQL. For every shard in the Vitess cluster, another slave instance has to be created and replication has to be maintained. End result is that Vitess cannot guarantee cannot continuous availability during failures. Spanner-inspired distributed SQL databases like YugabyteDB solve this replication problem at the core using Raft distributed consensus at a per-shard level for both data replication and leader election.

