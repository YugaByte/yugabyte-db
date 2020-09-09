---
title: Compare TiDB with YugabyteDB
headerTitle: TiDB
linkTitle: TiDB
description: Compare TiDB with YugabyteDB.
aliases:
  - /comparisons/tidb/
block_indexing: true
menu:
  stable:
    parent: comparisons
    weight: 1076
---

PingCap’s TiDB, a MySQL-compatible distributed database built on TiKV, takes design inspiration from Google Spanner and Apache HBase. While its sharding and replication architecture are similar to that of Spanner, it follows a very different design for multi-shard transactions. TiDB uses Google Percolator as the inspiration for its multi-shard transaction design. This choice essentially makes TiDB unfit for deployments with geo-distributed writes since the majority of transactions in a random-access OLTP workload will now experience high WAN latency when acquiring a timestamp from the global timestamp oracle running in a different region. Additionally, TiDB lacks support for critical relational data modeling constructs such as foreign key constraints and Serializable isolation level.

## Relevant blog posts

The following posts cover some more details around how YugabyteDB differs from TiDB.

- [What is Distributed SQL?](https://blog.yugabyte.com/what-is-distributed-sql/)
- [Implementing Distributed Transactions the Google Way: Percolator vs. Spanner](https://blog.yugabyte.com/implementing-distributed-transactions-the-google-way-percolator-vs-spanner/)
