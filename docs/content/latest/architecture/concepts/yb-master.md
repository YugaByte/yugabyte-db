---
title: YB-Master Service
headerTitle: YB-Master Service
linkTitle: YB-Master Service
description: Learn how the YB-Master service manages tablet (aka shard) metadata and coordinates cluster configuration changes.
aliases:
  - /latest/architecture/concepts/yb-master/
menu:
  latest:
    identifier: architecture-concepts-yb-master
    parent: key-concepts
    weight: 1126
isTocNested: true
showAsideToc: true
---

The YB-Master Service is the keeper of system metadata and records such as what tables exist in the system, where their tablets live, what users and roles exist, the permissions associated with them, and so on.

It is also responsible for coordinating background operations (such as load-balancing or initiating re-replication of under-replicated data) and performing a variety of administrative operations such as creating, altering, and dropping tables.

Note that the YB-Master is highly available as it forms a Raft group with its peers, and it is not in the critical path of IO against user tables.

![master_overview](/images/architecture/master_overview.png)

## Functions of YB-Master

### Coordination of universe-wide admin operations

Examples of such operations are user-issued `CREATE TABLE`, `ALTER TABLE`, and `DROP TABLE` requests, as well as a creating a backup of a table. The YB-Master performs these operations with a guarantee that the operation is propagated to all tablets irrespective of the state of the YB-TServers hosting these tablets. This is essential because a YB-TServer failure while one of these universe-wide operations is in progress cannot affect the outcome of the operation by failing to apply it on some tablets.

### Storage of system metadata

Each YB-Master stores system metadata, including information about namespaces, tables, roles, permissions, and assignments of tablets to YB-TServers. These system records are replicated across the YB-Masters for redundancy using Raft as well. The system metadata is also stored as a DocDB table by the YB-Masters.

### Authoritative source of tablet assignments to YB-TServers

The YB-Master stores all tablets and the corresponding YB-TServers that currently host them. This map of tablets to the hosting YB-TServers is queried by clients (such as the YugabyteDB query layer). Applications using the YugabyteDB smart clients for the YCQL and YSQL APIs are efficient in retrieving data. The smart clients query the YB-Master for the tablet to YB-TServer map and cache it. By doing so, the smart clients can talk directly to the correct YB-TServer to serve various queries without incurring additional network hops.

### Background operations

These operations are performed throughout the lifetime of the universe, in the background, without impacting foreground read and write performance.

#### Data placement and load balancing

The YB-Master leader does the initial placement (at `CREATE TABLE` time) of tablets across YB-TServers to enforce any user-defined data placement constraints and ensure uniform load. In addition, during the lifetime of the universe, as nodes are added, fail or decommissioned, it continues to balance the load and enforce data placement constraints automatically.

#### Leader balancing

Aside from ensuring that the number of tablets served by each YB-TServer is balanced across the universe, the YB-Masters also ensures that each node has a symmetric number of tablet-peer leaders across eligible nodes.

#### Re-replication of data on extended YB-TServer failure

The YB-Master receives heartbeats from all the YB-TServers, and tracks their liveness. It detects if any YB-TServers has failed, and keeps track of the time interval for which the YB-TServer remains in a failed state. If the time duration of the failure extends beyond a threshold, it finds replacement YB-TServers to which the tablet data of the failed YB-TServer is re-replicated. Re-replication is initiated in a throttled fashion by the YB-Master leader so as to not impact the foreground operations of the universe.
