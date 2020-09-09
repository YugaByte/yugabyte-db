---
title: Compare Apache HBase with YugabyteDB
headerTitle: Apache HBase
linkTitle: Apache HBase
description: Compare Apache HBase with YugabyteDB.
aliases:
  - /comparisons/hbase/
block_indexing: true
menu:
  stable:
    parent: comparisons
    weight: 1130
isTocNested: false
showAsideToc: true
---

Following are the key areas of differences between YugabyteDB and [Apache HBase](http://hbase.apache.org/).

## Simpler software stack

Apache HBase relies on HDFS (another complex piece of infrastructure for data replication) and on Apache Zookeeper for leader election, failure detection, and so on. Running HBase smoothly requires a lot of on-going operational overheads.

## High availability and fast failover

In HBase, on a region-server death, the unavailability window of shards/regions
on the server can be in the order of 60 seconds or more. This is because the HBase master first
needs to wait for the server’s ephemeral node in Zookeeper to expire, followed by time take to split
the transaction logs into per-shard recovery logs, and the time taken to replay the edits from the
transaction log by a new server before the shard is available to take IO. In contrast, in Yugabyte,
the tablet-peers are hot standbys, and within a matter of few heartbeats (a few seconds) detect
failure of the leader, and initiate leader election.

## C++ implementation

Avoids GC tuning; can run better on large memory machines.
Richer data model: YugabyteDB offers a multi-model/multi-API using YSQL, YCQL, and YEDIS.
Rather than deal with just byte keys and values, YugabyteDB offers a rich set of scalar (int, text,
decimal, binary, timestamp, etc.) and composite types (such as collections, UDTs, etc.).

## Multi-data center deployment

Flexible deployment choices across multiple DCs or availability zones. HBase provides
strong-consistency only within a single data center and offers only async replication alternative for
cross-DC deployments.
