---
title: Compare Redis in-memory store with YugabyteDB
linkTitle: Redis in-memory store
description: Compare Redis in-memory store with YugabyteDB.
block_indexing: true
menu:
  v2.1:
    parent: comparisons
    weight: 1129
isTocNested: false
showAsideToc: true
---

Following are the key areas of differences between YugabyteDB and [Redis in-memory store](https://redis.io/).

## Persistent DB vs. in-memory cache

YugabyteDB’s Redis is a persistent database rather than an in-memory cache. [While Redis has a
check-pointing feature for persistence, it is a highly inefficient operation that does a process
fork. It is also not an incremental operation; the entire memory state is written to disk causing
serious overall performance impact.]

## Auto-sharded and clustered

YugabyteDB’s Redis is an auto-sharded, clustered with built-in support for strongly consistent
replication and multi-DC deployment flexibility. Operations such as add node, remove node are
simple, throttled and intent-based and leverage Yugabyte’s core engine (YBase) and associated
architectural benefits.

## No explicit memory management

Unlike the normal Redis, the entire data set does not need to fit in memory. In Yugabyte, the hot
data lives in RAM, and colder data is automatically tiered to storage and on-demand paged in at
block granularity from storage much like traditional database.

## Consistent and transparent caching

Applications that use Redis only as a cache and use a separate backing database as the main system
of record, and need to deal with dev pain points around keeping the cache and DB consistent and
operational pain points at two levels of infrastructure (sharding, load-balancing, geo-redundancy)
etc. can leverage YugabyteDB’s Redis as a unified cache + database offering.

Scan resistant block cache design ensures long scan (e.g., of older data) do not impact reads for
recent data.
