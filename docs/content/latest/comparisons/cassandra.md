---
title: Compare Apache Cassandra with YugabyteDB
headerTitle: Apache Cassandra
linkTitle: Apache Cassandra
description: Compare Apache Cassandra with YugabyteDB.
aliases:
  - /comparisons/cassandra/
menu:
  latest:
    parent: comparisons
    weight: 1120
isTocNested: false
showAsideToc: true
---

Application developers choosing Apache Cassandra as their default operational database understand well that their choice does not support multi-shard (aka distributed) ACID transactions. But they mistakenly believe that they can use Cassandra features such as quorum writes/reads, lightweight transactions and secondary indexes to achieve single-key ACID guarantees. This is because the Cassandra marketing and technical documentation over the years has promoted it as a “consistent-enough” database. This is far from the truth. The only good use of Apache Cassandra is in context of its original intent as a inexpensive, eventually consistent store for large volumes of data. Newer Cassandra compatible databases such as DataStax Enterprise and ScyllaDB suffer from the same problems as Apache Cassandra since they have not changed the design of the eventually consistent core.

For use cases that simultaneously need strong consistency, low latency and high density, the right path is to use a database that is not simply Cassandra compatible but is also transactional. This is exactly what YugabyteDB offers. Each of the problems highlighted here are solved by YugabyteDB at the core of its architecture.

- Single-key writes go through Raft (which also uses quorum) in YugabyteDB but reads are quorumless and hence can be served off a single node both for strongly consistent and timeline-consistent (aka bounded staleness) cases.

- Multi-key transactions are also supported through the use of a transaction manager that uses an enhanced 2-phase commit protocol.

- Lightweight transactions are obviated altogether in YugabyteDB since the Raft-based writes on a single key are automatically linearizable.

- Secondary indexes are global similar to the primary indexes so only the nodes storing the secondary indexes are queried.

Following are the details of the key differences between YugabyteDB and [Apache Cassandra](http://cassandra.apache.org/).

## Data consistency

YugabyteDB is a strongly consistent system that avoids several pitfalls of an eventually consistent database.

1. Dirty reads: Systems with an eventually consistent core offer weaker semantics than even async
replication. To work around this, many Apache Cassandra deployments use quorum read/write modes, but
even those do NOT offer [clean rollback semantics on write failures](https://stackoverflow.com/questions/12156517/whats-the-difference-between-paxos-and-wr-n-in-cassandra) and hence can still lead to dirty reads.

2. Deletes resurfacing: Another problem due to an eventually consistent core is [deleted values resurfacing](https://stackoverflow.com/questions/35392430/cassandra-delete-not-working).

YugabyteDB avoids these pitfalls by using a theoretically sound replication model based on RAFT, with
strong-consistency on writes and tunable consistency options for reads.

## High read latency of eventual consistency

In eventually consistent systems, anti-entropy, read-repairs, etc. hurt performance and increase cost. But even in steady state, read operations read from a quorum to serve closer to correct responses and fix inconsistencies. For a replication factor of the 3, such a system’s read throughput is essentially ⅓.

With YugabyteDB, strongly consistent reads (from leaders), as well as timeline consistent/async reads
from followers perform the read operation on only 1 node (and not 3).

## Lightweight transactions

In Apache Cassandra, simple read-modify-write operations (aka compare-and-swap) such as “increment”, conditional updates, “INSERT …  IF NOT EXISTS” or “UPDATE ... IF EXISTS” use a scheme known as lightweight transactions [which incurs a 4-round-trip cost](https://teddyma.gitbooks.io/learncassandra/content/concurrent/concurrency_control.html) between replicas. With YugabyteDB, these operations only involve 1-round trip between the quorum members.

## Secondary indexes

Local secondary indexes in Apache Cassandra ([see blog](https://pantheon.io/blog/cassandra-scale-problem-secondary-indexes)) require a fan-out read to all nodes, i.e. index performance keeps dropping as cluster size increases. With YugabyteDB’s distributed transactions, secondary indexes are both strongly-consistent and point-reads rather than a read from all nodes/shards in the cluster.

## Data modeling

Apache Cassandra is a flexi-schema database that supports single key data modeling. On the other hand, YugabyteDB is a multi-model and multi-API database that supports multiple different types of data modeling including flexi-schema and document data (with the native JSON data type support in the Cassandra-compatible YCQL API). Additionally, YugabyteDB supports key-value (with the Redis-compatible YEDIS API) and relational (with the PostgreSQL-compatible YSQL API) data modeling.

Some further details on the document data modeling of both databases is warranted. Apache Cassandra's JSON support can be misleading for many developers. YCQL allows SELECT and INSERT statements to include the JSON keyword. The SELECT output will now be available in the JSON format and the INSERT inputs can now be specified in the JSON format. However, this “JSON” support is simply an ease-of-use abstraction in the CQL layer that the underlying database engine is unaware of. Since there is no native JSON data type in CQL, the schema doesn’t have any knowledge of the JSON provided by the user. This means the schema definition doesn’t change nor does the schema enforcement. Cassandra developers needing native JSON support previously had no choice but to add a new document database such as MongoDB or Couchbase into their data tier.

With YugabyteDB’s native JSON support, developers can now benefit from the structured query language of Cassandra and the document data modeling of MongoDB in a single database.

## Operational stability / Add node challenges

1. Apache Cassandra’s eventually consistent core implies that a new node trying to join a cluster cannot simply copy data files from current “leader”. A logical read (quorum read) across multiple surviving peers is needed with Apache Cassandra to build the correct copy of the data to bootstrap a new node with. These reads need to uncompress/recompress the data back too. With Yugabyte, a new node can be bootstrapped by simply copying already compressed data files from the leader of the corresponding shard.

2. Apache Cassandra is implemented in Java. GC pauses plus additional problems, especially when running with large heap sizes (RAM). YugabyteDB is implemented in C++.

3. Operations like anti-entropy and read-pair hurt steady-state stability of cluster as they consume additional system resources. With Yugabyte, which does replication using distributed consensus, these operations are not needed since the replicas stay in sync using RAFT or catch up the deltas cleanly from the transaction log of the current leader.

4. Apache Cassandra needs constant tuning of compactions (because of Java implementation, non-scalable, non-partitioned bloom filters and index-metadata, lack of scan-resistant caches, and so on.).

5. Adding a node when the cluster is at risk of running out of disk space can give relief in steps over time. The new server will 
start serving requests as soon as 1 tablet has been transferred. While in Cassandra the old nodes get no benefit until after adding node and then doing `nodetool cleanup` which could take many hours each.


## Operational flexibility

At times, you may need to move your database infrastructure to new hardware or you may want to add a sync/async replica in another region or in public cloud. With YugabyteDB, these operations are simple 1-click intent based operations that are handled seamlessly by the system in a completely online manner. Yugabyte’s core data fabric and its consensus based replication model enables the "data tier” to be very agile/recomposable much like containers/VMs have done for the application or stateless tier.

## Need to scale beyond single or 2-DC deployments

The current RDBMS solutions (using async replication) and NoSQL solutions work up to 2 data center deployments; but in a very restrictive manner. With Apache Cassandra’s 2-DC deployments, one has to chose between write unavailability on a partition between the DCs, or cope with async replication model where on a DC failure some inflight data is lost.

YugabyteDB’s distributed consensus based replication design, in 3-DC deployments for example, enables enterprise to “have their cake and eat it too”. It gives use cases the choice to be highly available for reads and writes even on a complete DC outage, without having to take a down time or resort to an older/asynchronous copy of the data (which may not have all the changes to the system).

## Relevant blog posts

A few blog posts that highlight how YugabyteDB differs from Apache Cassandra are below.

- [Apache Cassandra: The Truth Behind Tunable Consistency, Lightweight Transactions & Secondary Indexes](https://blog.yugabyte.com/apache-cassandra-lightweight-transactions-secondary-indexes-tunable-consistency/)
- [Building a Strongly Consistent Cassandra with Better Performance](https://blog.yugabyte.com/building-a-strongly-consistent-cassandra-with-better-performance)
- [DynamoDB vs MongoDB vs Cassandra for Fast Growing Geo-Distributed Apps](https://blog.yugabyte.com/dynamodb-vs-mongodb-vs-cassandra-for-fast-growing-geo-distributed-apps/)
- [YugabyteDB 1.1 New Feature: Document Data Modeling with the JSON Data Type](https://blog.yugabyte.com/yugabyte-db-1-1-new-feature-document-data-modeling-with-json-data-type/)
- [YugabyteDB 1.1 New Feature: Speeding Up Queries with Secondary Indexes](https://blog.yugabyte.com/yugabyte-db-1-1-new-feature-speeding-up-queries-with-secondary-indexes/)
