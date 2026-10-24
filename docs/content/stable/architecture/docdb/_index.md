---
title: DocDB storage layer
headerTitle: DocDB storage layer
linkTitle: DocDB - Storage layer
description: Learn about the persistent storage layer of DocDB.
aliases:
  - /architecture/concepts/docdb/
  - /stable/docdb/persistence/
headcontent: The document store responsible for transactions, sharding, replication, and persistence
menu:
  stable:
    identifier: docdb
    parent: architecture
    weight: 600
type: indexpage
---

DocDB is the underlying document storage engine of YugabyteDB and is built on top of a highly customized and optimized version of [RocksDB](http://rocksdb.org/), a [log-structured merge tree (LSM)](./lsm-sst)-based key-value store. Several enhancements and customizations have been made on top of the vanilla RocksDB to make DocDB highly performant and scalable. DocDB in essence manages multiple RocksDB instances which are created one per tablet.

![DocDB Document Storage Layer](/images/architecture/docdb-rocksdb.png)

## Data model

DocDB is a persistent key-value store, which means that data is stored and retrieved using unique keys. It supports ordered data operations, allowing efficient range queries and iteration over keys. The keys are designed for fast lookups and efficient range scans.

{{<lead link="./data-model">}}
To understand more about how row data is stored as keys and values in DocDB, see [DocDB data model](./data-model).
{{</lead>}}

## Storage engine

DocDB is a log-structured merge-tree (LSM) based storage engine. This design is optimized for high write throughput and efficient storage utilization. Data is stored in multiple SSTs (Sorted String Tables) to store key-value data on disk. It is designed to be efficient for both sequential and random access patterns. DocDB periodically compacts data by merging and sorting multiple SST files into a smaller set of files. This process helps to maintain a consistent on-disk format and reclaim space from obsolete data.

{{<lead link="./lsm-sst">}}
To understand more about how LSM tree stores data in SSTs, see [LSM and SST](./lsm-sst).
{{</lead>}}

## Vector indexes with Vector LSM

YugabyteDB provides specialized support for vector indexes optimized for approximate nearest neighbor (ANN) search. Vector LSM is a customized log-structured merge tree designed specifically for managing vector data and maintaining vector indexes across distributed nodes.

{{<lead link="./vector-lsm">}}
To understand more about how Vector LSM manages vector indexes and handles compaction, see [Vector LSM](./vector-lsm).
{{</lead>}}

## Performance

DocDB is written in C++ and is designed to be highly performant on Linux.

{{<lead link="./performance">}}
To understand more about how DocDB enhances RocksDB, see [Performance](./performance).
{{</lead>}}
