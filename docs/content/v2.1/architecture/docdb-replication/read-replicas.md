---
title: Read replicas
headerTitle: Read replicas
linkTitle: Read replicas
description: Learn about read replicas in YugabyteDB.
block_indexing: true
menu:
  v2.1:
    identifier: architecture-docdb-replication-read-replicas
    parent: architecture-docdb-replication
    weight: 1155
isTocNested: true
showAsideToc: true
---

In addition to the core distributed consensus based replication, DocDB extends Raft to add read replicas (aka observer nodes) that do not participate in writes but get a timeline consistent copy of the data in an asynchronous manner.

Read Replicas are a read-only extension to the primary data in the cluster. With read replicas, the primary data of the cluster is deployed across multiple zones in one region, or across nearby regions. Read replicas do not add to the write latencies since the write does not synchronously replicate data to them - the data gets replicated to read replicas asynchronously.

Nodes in remote data centers can thus be added in "read-only" mode. This is primarily for cases where latency of doing a distributed consensus-based write is not
tolerable for some workloads.

## Replication factor

Every universe contains a primary data cluster, and one or more read replica clusters. Thus, each read replica cluster can independently have its own replication factor.

{{< note title="Note" >}}

The replication factor of a read replica cluster can be an even number as well. For example, a read replica cluster with a replication factor of 2 is perfectly valid. This is the case because read replicas do not participate in Raft consensus operation, and therefore an odd number of replicas is not required for correctness.

{{</note >}}


## Writing to read replicas

An application can send write requests to read replicas, but these write requests get internally redirected to the source of truth. This is because the read replicas are aware of the topology of the cluster.

## Schema changes

Since read replicas are a Raft replication level extension, the schema changes will transparently apply to these replicas. There is no need to 

## Read replicas vs eventual consistency

This read-only node (or timeline-consistent node) is still strictly better than eventual consistency, because with the latter the application's view of the data can move back and forth in time and is hard to program to.
