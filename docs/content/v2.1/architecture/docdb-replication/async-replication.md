---
title: xCluster replication
headerTitle: xCluster replication
linkTitle: xCluster replication
description: Asynchronous replication between multiple YugabyteDB clusters.
block_indexing: true
menu:
  v2.1:
    identifier: architecture-docdb-async-replication
    parent: architecture-docdb-replication
    weight: 1150
isTocNested: true
showAsideToc: true
---

xCluster replication enables asynchronous replication between independent YugabyteDB clusters.

## Overview

YugabyteDB provides synchronous replication of data in clusters dispersed across multiple (three or more) data centers by leveraging the Raft consensus algorithm to achieve enhanced high availability and performance. However, many use cases do not require synchronous replication or justify the additional complexity and operation costs associated with managing three or more data centers. For these needs, YugabyteDB supports two data center (2DC) deployments that use asynchronous replication built on top of [change data capture (CDC)](../change-data-capture) in DocDB.

For details about configuring a 2DC deployment, see [Replicate between two data centers](../../../deploy/multi-dc/2dc-deployment).

{{< note title="Note" >}}

Asynchronous replication of data will work across all the APIs (YSQL, YCQL) since the replication across the clusters is done at the DocDB level.

{{</note >}}

## Supported deployment scenarios

### Active-Passive

The replication could be unidirectional from a source cluster (aka the master cluster) to one or more sink clusters (aka slave clusters). The sink clusters, typically located in data centers or regions that are different from the source cluster, are passive because they do not not take writes from the higher layer services. Such deployments are typically used for serving low latency reads from the slave clusters as well as for disaster recovery purposes.

The source-sink deployment architecture is shown in the diagram below:

<img src="https://github.com/yugabyte/yugabyte-db/raw/master/architecture/design/images/2DC-source-sink-deployment.png" style="max-width:750px;"/>

### Active-Active

The replication of data can be bi-directional between two clusters. In this case, both clusters can perform reads and writes. Writes to any cluster is asynchronously replicated to the other cluster with a timestamp for the update. If the same key is updated in both clusters at a similar time window, this will result in the write with the larger timestamp becoming the latest write. Thus, in this case, the clusters are all active and this deployment mode is called multi-master deployments or active-active deployments).

{{< note title="Note" >}}

The multi-master deployment is built internally using two master-slave unidirectional replication streams as a building block. Special care is taken to ensure that the timestamps are assigned to ensure last writer wins semantics and the data arriving from the replication stream is not re-replicated. 

{{</note >}}

The architecture diagram is shown below:

<img src="https://github.com/yugabyte/yugabyte-db/raw/master/architecture/design/images/2DC-multi-master-deployment.png" style="max-width:750px;"/>

## Features and limitations

### Features

- Updates will be timeline consistent. That is, target data center will receive updates for a row in the same order in which they occurred on the source.
- Active-active replication is supported, with both data centers accepting writes and replicating them to the other data center.
- It is possible to perform replication to multiple target clusters. Similarly, it will be possible to consume replicated data from multiple source clusters.

### Impact on application design

Since 2DC replication is done asynchronously and by replicating the WAL (and thereby bypassing the query layer), application design needs to follow these patterns:

- **Avoid UNIQUE indexes / constraints (only for active-active mode):** Since replication is done at the WAL level, we don’t have a way to check for unique constraints. It’s possible to have two conflicting writes on separate universes which will violate the unique constraint and will cause the main table to contain both rows but the index to contain just 1 row, resulting in an inconsistent state.

- **Avoid triggers:** Since we bypass the query layer for replicated records, DB triggers will not be fired for those and can result in unexpected behavior.

- **Avoid serial columns in primary key (only for active-active mode):** Since both universes will generate the same sequence numbers, this can result in conflicting rows. It is better to use UUIDs instead.

### Limitations

- Transaction atomicity
  - Transactions from the producer won't be applied atomically on the consumer. That is, some changes in a transaction may be visible before others.

- Automatically bootstrapping sink clusters
  - Currently: it is the responsibility of the end user to ensure that a sink cluster has sufficiently recent updates so that replication can safely resume.
  - Future: bootstrapping the sink cluster can be automated.

- Replicating DDL changes
  - Currently: DDL changes are not automatically replicated. Applying create table and alter table commands to the sync clusters is the responsibility of the user.
  - Future: Allow safe DDL changes to be propagated automatically.

- Safety of DDL and DML in active-active
  - Currently: Certain potentially unsafe combinations of DDL/DML are allowed. For example, in having a unique key constraint on a column in an active-active last writer wins mode is unsafe since a violation could easily be introduced by inserting different values on the two clusters - each of these operations is legal in itself. The ensuing replication can, however, violate the unique key constraint. This will cause the two clusters to permanently diverge and the replication to fail.
  - Future: Detect such unsafe combinations and warn the user. Such combinations should possibly be disallowed by default.

## Transactional guarantees

### Atomicity of transactions

This implies one can never read a partial result of a transaction on the sink cluster.

### Not globally ordered

The transactions (especially those that do not involve overlapping rows) may not be applied in the same order as they occur in the source cluster.

### Last writer wins

In case of active-active configurations, if there are conflicting writes to the same key, then the update with the larger timestamp is considered the latest update. Thus, the deployment is eventually consistent across the two data centers.
