---
title: FAQs about YugabyteDB
headerTitle: General FAQ
linkTitle: General FAQ
description: Answers to common questions about YugabyteDB.
aliases:
  - /latest/faq/product/
  - /latest/introduction/overview/
  - /latest/introduction/benefits/
  - /latest/introduction/core-features/
  - /latest/introduction/
  - /latest/faq/architecture/
menu:
  latest:
    identifier: faq-general
    parent: faq
    weight: 2710
isTocNested: false
showAsideToc: true
---

## What is YugabyteDB?

<!--
<div class="video-wrapper">
{{< vimeo 305074082 >}}
</div>
-->

YugabyteDB is a high-performance distributed SQL database for powering global, internet-scale applications. Built using a unique combination of high-performance document store, per-shard distributed consensus replication and multi-shard ACID transactions (inspired by Google Spanner), YugabyteDB serves both scale-out RDBMS and internet-scale OLTP workloads with low query latency, extreme resilience against failures and global data distribution. As a cloud native database, it can be deployed across public and private clouds as well as in Kubernetes environments with ease.

YugabyteDB is developed and distributed as an [Apache 2.0 open source project](https://github.com/yugabyte/yugabyte-db/).

## What makes YugabyteDB unique?

YugabyteDB is a transactional database that brings together 4 must-have needs of cloud native apps, namely SQL as a flexible query language, low-latency performance, continuous availability and globally-distributed scalability. Other databases do not serve all 4 of these needs simultaneously.

- Monolithic SQL databases offer SQL and low-latency reads but neither have ability to tolerate failures nor can scale writes across multiple nodes, zones, regions and clouds.

- Distributed NoSQL databases offer read performance, high availability and write scalability but give up on SQL features such as relational data modeling and ACID transactions.

YugabyteDB feature highlights are listed below.

### SQL and ACID transactions

- SQL [JOINs](../../quick-start/explore-ysql/#3-joins) and [distributed transactions](../../quick-start/explore-ysql/#4-distributed-transactions) that allow multi-row access across any number of shards at any scale.

- Transactional [document store](../../architecture/concepts/docdb/) backed by self-healing, strongly-consistent, synchronous [replication](../../architecture/concepts/docdb/replication/).

### High performance and massive scalability

- Low latency for geo-distributed applications with multiple [read consistency levels](../../architecture/concepts/docdb/replication/#tunable-read-consistency) and [read replicas](../../architecture/concepts/docdb/replication/#read-replicas).

- Linearly scalable throughput for ingesting and serving ever-growing datasets.

### Global data consistency

- [Global data distribution](../../explore/global-distribution/) that brings consistent data close to users through multi-region and multi-cloud deployments. Optional two-region multi-master and master-follower configurations powered by CDC-driven asynchronous replication.

- [Auto-sharding and auto-rebalancing](../../explore/auto-sharding/) to ensure uniform load across all nodes even for very large clusters.

### Cloud native

- Built for the container era with [highly elastic scaling](../../explore/linear-scalability/) and infrastructure portability, including [Kubernetes-driven orchestration](../../quick-start/install/kubernetes).

- [Self-healing database](../../explore/fault-tolerance/) that automatically tolerates any failures common in the inherently unreliable modern cloud infrastructure.

### Open source

- Fully functional distributed database available under [Apache 2.0 open source license](https://github.com/yugabyte/yugabyte-db/). 

### Built-in enterprise features

- Starting in [v1.3](https://blog.yugabyte.com/announcing-yugabyte-db-v1-3-with-enterprise-features-as-open-source/), YugabyteDB is the only open-source distributed SQL database to have built-in enterprise features such as Distributed Backups, Data Encryption, and Read Replicas. New features such as [Change Data Capture (CDC)](../../architecture/cdc-architecture/) and [2 Data Center Deployments](../../architecture/2dc-deployments/) are also included in open source.

## What client APIs are supported by YugabyteDB?

YugabyteDB supports two flavors of distributed SQL.

### Yugabyte SQL (YSQL)

[YSQL](../../api/ysql/) is a fully-relational SQL API that is wire compatible with the SQL language in PostgreSQL. It is best fit for RDBMS workloads that need horizontal write scalability and global data distribution while also using relational modeling features such as JOINs, distributed transactions and referential integrity (such as foreign keys). Get started by [exploring YSQL features](../../quick-start/explore-ysql/).

### Yugabyte Cloud QL (YCQL)

[YCQL](../../api/ycql/) is a semi-relational SQL API that is best fit for internet-scale OLTP and HTAP applications needing massive data ingestion and blazing-fast queries. It supports distributed transactions, strongly consistent secondary indexes and a native JSON column type. YCQL has its roots in the Cassandra Query Language. Get started by [exploring YCQL features](../../api/ycql/quick-start/).

{{< note title="Note" >}}

The YugabyteDB APIs are isolated and independent from one another today. This means that the data inserted or managed by one API cannot be queried by the other API. Additionally, there is no common way to access the data across the APIs (external frameworks such as [Presto](../../develop/ecosystem-integrations/presto/) can help for simple cases). 

<b>The net impact is that application developers have to select an API first before undertaking detailed database schema/query design and implementation.</b>

{{< /note >}}

## When should I pick YCQL over YSQL?

You should pick YCQL over YSQL if your application:

- Does not require fully-relational data modeling constructs, such as foreign keys and JOINs. Note that strongly-consistent secondary indexes and unique constraints are supported by YCQL.
- Requires storing large amounts of data (for example, 10TB or more).
- Needs to serve low-latency (sub-millisecond) queries.
- Needs TTL-driven automatic data expiration.
- Needs to integrate with stream processors, such as Apache Spark and KSQL.

If you have a specific use case in mind, share it in our [Slack community](https://www.yugabyte.com/slack) and the community can help you decide the best approach.

## How does YugabyteDB's common document store work?

[DocDB](../../architecture/concepts/docdb/), YugabyteDB's distributed document store common across all APIs, is built using a custom integration of Raft replication, distributed ACID transactions and the RocksDB storage engine. Specifically, DocDB enhances RocksDB by transforming it from a key-value store (with only primitive data types) to a document store (with complex data types). **Every key is stored as a separate document in DocDB, irrespective of the API responsible for managing the key.** DocDB’s [sharding](../../architecture/concepts/docdb/sharding/), [replication/fault-tolerance](../../architecture/concepts/docdb/replication/) and [distributed ACID transactions](../../architecture/transactions/distributed-txns/) architecture are all based on the [Google Spanner design](https://research.google.com/archive/spanner-osdi2012.pdf) first published in 2012. [How We Built a High Performance Document Store on RocksDB?](https://blog.yugabyte.com/how-we-built-a-high-performance-document-store-on-rocksdb/) provides an in-depth look into DocDB.

## What are the trade-offs involved in using YugabyteDB?

Trade-offs depend on the type of database used as baseline for comparison.

### Distributed SQL

Examples: Amazon Aurora, Google Cloud Spanner, CockroachDB, TiDB

**Benefits of YugabyteDB**

- Low-latency reads and high-throughput writes. 
- Cloud-neutral deployments with a Kubernetes-native database.
- 100% Apache 2.0 open source even for enterprise features.

**Trade-offs**

- None

Learn more: [What is Distributed SQL?](https://blog.yugabyte.com/what-is-distributed-sql/)

### Monolithic SQL

Examples: PostgreSQL, MySQL, Oracle, Amazon Aurora.

**Benefits of YugabyteDB**

- Scale write throughput linearly across multiple nodes and/or geographic regions. 
- Automatic failover and native repair.
- 100% Apache 2.0 open source even for enterprise features.

**Trade-offs**

- Transactions and JOINs can now span multiple nodes, thereby increasing latency.

Learn more: [Distributed PostgreSQL on a Google Spanner Architecture – Query Layer](https://blog.yugabyte.com/distributed-postgresql-on-a-google-spanner-architecture-query-layer/)

### Traditional NewSQL

Examples: Vitess, Citus

**Benefits of YugabyteDB**

- Distributed transactions across any number of nodes.
- No single point of failure given all nodes are equal.
- 100% Apache 2.0 open source even for enterprise features.

**Trade-offs**

- None

Learn more: [Rise of Globally Distributed SQL Databases – Redefining Transactional Stores for Cloud Native Era](https://blog.yugabyte.com/rise-of-globally-distributed-sql-databases-redefining-transactional-stores-for-cloud-native-era/)

### Transactional NoSQL 

Examples: MongoDB, Amazon DynamoDB, FoundationDB, Azure Cosmos DB.

**Benefits of YugabyteDB**

- Flexibility of SQL as query needs change in response to business changes.
- Distributed transactions across any number of nodes.
- Low latency, strongly consistent reads given that read-time quorum is avoided altogether.
- 100% Apache 2.0 open source even for enterprise features.

**Trade-offs**

- None

Learn more: [Why are NoSQL Databases Becoming Transactional?](https://blog.yugabyte.com/nosql-databases-becoming-transactional-mongodb-dynamodb-faunadb-cosmosdb/)

### Eventually Consistent NoSQL

Examples: Apache Cassandra, Couchbase.

**Benefits of YugabyteDB**

- Flexibility of SQL as query needs change in response to business changes.
- Strongly consistent, zero data loss writes.
- Strongly consistent as well as timeline-consistent reads without resorting to eventual consistency-related penalties such as read repairs and anti-entropy.
- 100% Apache 2.0 open source even for enterprise features.

**Trade-offs**

- Extremely short unavailability during the leader election time for all shard leaders lost during a node failure or network partition. 

Learn more: [Apache Cassandra: The Truth Behind Tunable Consistency, Lightweight Transactions & Secondary Indexes](https://blog.yugabyte.com/apache-cassandra-lightweight-transactions-secondary-indexes-tunable-consistency/)

## When is YugabyteDB a good fit?

YugabyteDB is a good fit for fast-growing, cloud native applications that need to serve business-critical data reliably, with zero data loss, high availability and low latency. Common use cases include:

- Distributed Online Transaction Processing (OLTP) applications needing multi-region scalability without compromising strong consistency and low latency. E.g. User identity, Retail product catalog, Financial data service.

- Hybrid Transactional/Analytical Processing (HTAP), also known as Translytical, applications needing real-time analytics on transactional data. E.g User personalization, fraud detection, machine learning.

- Streaming applications needing to efficiently ingest, analyze and store ever-growing data. E.g. IoT sensor analytics, time series metrics, real-time monitoring.

A few such use cases are detailed [here](https://www.yugabyte.com/).

## When is YugabyteDB not a good fit?

YugabyteDB is not a good fit for traditional Online Analytical Processing (OLAP) use cases that need complete ad-hoc analytics. Use an OLAP store such as [Druid](http://druid.io/druid.html) or a data warehouse such as [Snowflake](https://www.snowflake.net/).

## How can YugabyteDB be both CP and ensure high availability (HA) at the same time?

In terms of the [CAP theorem](https://blog.yugabyte.com/a-for-apple-b-for-ball-c-for-cap-theorem-8e9b78600e6d), YugabyteDB is a consistent and partition-tolerant (CP) database. It ensures high availability (HA) for most practical situations even while remaining strongly consistent. While this may seem to be a violation of the CAP theorem, that is not the case. CAP treats availability as a binary option whereas YugabyteDB treats availability as a percentage that can be tuned to achieve high write availability (reads are always available as long as a single node is available).

- During network partitions or node failures, the replicas of the impacted tablets (whose leaders got partitioned out or lost) form two groups: a majority partition that can still establish a Raft consensus and a minority partition that cannot establish such a consensus (given the lack of quorum). The replicas in the majority partition elect a new leader among themselves in a matter of seconds and are ready to accept new writes after the leader election completes. For these few seconds till the new leader is elected, the DB is unable to accept new writes given the design choice of prioritizing consistency over availability. All the leader replicas in the minority partition lose their leadership during these few seconds and hence become followers.

- Majority partitions are available for both reads and writes. Minority partitions are available for reads only (even if the data may get stale as time passes), but not available for writes. **Multi-active availability** refers to YugabyteDB's ability to serve writes on any node of a non-partitioned cluster and reads on any node of a partitioned cluster.

- The approach above obviates the need for any unpredictable background anti-entropy operations as well as need to establish quorum at read time. As shown in the [YCSB benchmarks against Apache Cassandra](https://forum.yugabyte.com/t/ycsb-benchmark-results-for-yugabyte-and-apache-cassandra-again-with-p99-latencies/99), YugabyteDB delivers predictable p99 latencies as well as 3x read throughput that is also timeline-consistent (given no quorum is needed at read time).

On one hand, the YugabyteDB storage and replication architecture is similar to that of [Google Cloud Spanner](https://cloudplatform.googleblog.com/2017/02/inside-Cloud-Spanner-and-the-CAP-Theorem.html), which is also a CP database with high write availability. While Google Cloud Spanner leverages Google's proprietary network infrastructure, YugabyteDB is designed work on commodity infrastructure used by most enterprise users. On the other hand, YugabyteDB's multi-model, multi-API, and tunable read latency approach is similar to that of [Azure Cosmos DB](https://azure.microsoft.com/en-us/blog/a-technical-overview-of-azure-cosmos-db/).

A post on our blog titled [Practical Tradeoffs in Google Cloud Spanner, Azure Cosmos DB and YugabyteDB](https://blog.yugabyte.com/practical-tradeoffs-in-google-cloud-spanner-azure-cosmos-db-and-yugabyte-db/) goes through the above tradeoffs in more detail.

## How many major releases YugabyteDB has had so far?

YugabyteDB has had 8 major releases.

- [v2.2](https://blog.yugabyte.com/) in July 2020.
- [v2.1](https://blog.yugabyte.com/yugabytedb-2-1-is-ga-scaling-new-heights-with-distributed-sql/) in February 2020.
- [v2.0](https://blog.yugabyte.com/announcing-yugabyte-db-2-0-ga:-jepsen-tested,-high-performance-distributed-sql/ ) in September 2019.
- [v1.3](https://blog.yugabyte.com/announcing-yugabyte-db-v1-3-with-enterprise-features-as-open-source/) in July 2019.
- [v1.2](https://blog.yugabyte.com/announcing-yugabyte-db-1-2-company-update-jepsen-distributed-sql/) in March 2019.
- [v1.1](https://blog.yugabyte.com/announcing-yugabyte-db-1-1-and-company-update/) in September 2018.
- [v1.0](https://blog.yugabyte.com/announcing-yugabyte-db-1-0-%F0%9F%8D%BE-%F0%9F%8E%89/) in May 2018.
- [v0.9 Beta](https://blog.yugabyte.com/yugabyte-has-arrived/) in November 2017.

The next major release is the v2.5 release in Fall 2020. Roadmap for this release can be found on [GitHub](https://github.com/yugabyte/yugabyte-db#whats-being-worked-on).

## Can I deploy YugabyteDB to production?

Yes, both YugabyteDB APIs are production ready. [YCQL](https://blog.yugabyte.com/yugabyte-db-1-0-a-peek-under-the-hood/) achieved this status starting with v1.0 in May 2018 while [YSQL](https://blog.yugabyte.com/announcing-yugabyte-db-2-0-ga:-jepsen-tested,-high-performance-distributed-sql/) became production ready starting v2.0 in September 2019.

## Which companies are currently using YugabyteDB in production?

Reference deployments are listed [here](https://www.yugabyte.com/all-resources/resource-parent/case-studies/).

## What is the definition of the "Beta" feature tag?

Some features are marked Beta in every release. Following are the points to consider:

- Code is well tested. Enabling the feature is considered safe. Some of these features enabled by default.

- Support for the overall feature will not be dropped, though details may change in incompatible ways in a subsequent beta or GA release. 

- Recommended only for non-production use.

Please do try our beta features and give feedback on them on our [Slack community](https://www.yugabyte.com/slack) or by filing a [GitHub issue](https://github.com/yugabyte/yugabyte-db/issues).

## Any performance benchmarks available?

[Yahoo Cloud Serving Benchmark (YCSB)](https://github.com/brianfrankcooper/YCSB/wiki) is a popular benchmarking framework for NoSQL databases. We benchmarked the Yugabyte Cloud QL (YCQL) API against standard Apache Cassandra using YCSB. YugabyteDB outperformed Apache Cassandra by increasing margins as the number of keys (data density) increased across all the 6 YCSB workload configurations. 

[Netflix Data Benchmark (NDBench)](https://github.com/Netflix/ndbench) is another publicly available, cloud-enabled benchmark tool for data store systems. We ran NDBench against YugabyteDB for 7 days and observed P99 and P995 latencies that were orders of magnitude less than that of Apache Cassandra. 

Details for both the above benchhmarks are published in [Building a Strongly Consistent Cassandra with Better Performance](https://blog.yugabyte.com/building-a-strongly-consistent-cassandra-with-better-performance-aa96b1ab51d6).

## What about correctness testing?

[Jepsen](https://jepsen.io/) is a widely used framework to evaluate the behavior of databases under different failure scenarios. It allows for a database to be run across multiple nodes, and create artificial failure scenarios, as well as verify the correctness of the system under these scenarios. YugabyteDB 1.2 passes [formal Jepsen testing](https://blog.yugabyte.com/yugabyte-db-1-2-passes-jepsen-testing/). 

## Is YugabyteDB open source?

Starting with [v1.3](https://blog.yugabyte.com/announcing-yugabyte-db-v1-3-with-enterprise-features-as-open-source/), YugabyteDB is 100% open source. It is licensed under Apache 2.0 and the source is available on [GitHub](https://github.com/yugabyte/yugabyte-db).

## How do YugabyteDB, Yugabyte Platform and Yugabyte Cloud differ from each other?

[YugabyteDB](../../quick-start/) is the 100% open source core database. It is the best choice for the startup organizations with strong technical operations expertise looking to deploy to production with traditional DevOps tools.

[Yugabyte Platform](../../yugabyte-platform/) is commercial software for running a self-managed YugabyteDB-as-a-Service. It has built-in cloud native operations, enterprise-grade deployment options and world-class support. It is the simplest way to run YugabyteDB in mission-critical production environments with one or more regions (across both public cloud and on-premises data centers).

[Yugabyte Cloud](../../yugabyte-cloud/) is Yugabyte's fully-managed cloud service on Amazon Web Services (AWS) and Google Cloud Platform (GCP). You can [sign up](https://www.yugabyte.com/cloud/) for early access now.

For a more detailed comparison between the above, see [Adopt YugabyteDB Your Way
](https://www.yugabyte.com/platform/#compare-editions).

## How does YugabyteDB compare to other SQL and NoSQL databases?

See [Compare YugabyteDB to other databases](../../comparisons/)

- [Amazon Aurora](../../comparisons/amazon-aurora/)
- [Google Cloud Spanner](../../comparisons/google-spanner/)
- [MongoDB](../../comparisons/mongodb/)
- [CockroachDB](https://www.yugabyte.com/yugabyte-db-vs-cockroachdb/)

## Why is a group of YugabyteDB nodes called a universe instead of the more commonly used term clusters?

A YugabyteDB universe packs a lot more functionality than what people think of when referring to a cluster. In fact, in certain deployment choices, the universe subsumes the equivalent of multiple clusters and some of the operational work needed to run these. Here are just a few concrete differences, which made us feel like giving it a different name would help earmark the differences and avoid confusion.

- A YugabyteDB universe can move into new machines, availability zones (AZs), regions, and data centers in an online fashion, while these primitives are not associated with a traditional cluster.

- It is very easy to set up multiple asynchronous replicas with just a few clicks (in the Yugabyte Platform). This is built into the universe as a first-class operation with bootstrapping of the remote replica and all the operational aspects of running async replicas being supported natively. In the case of traditional clusters, the source and the async replicas are independent clusters. The user is responsible for maintaining these separate clusters as well as operating the replication logic.

- Failover to asynchronous replicas as the primary data and failback once the original is up and running are both natively supported within a universe.

## What is the difference between `ysqlsh` and `psql`?

The YSQL shell (`ysqlsh`) is functionally similar to PostgreSQL's `psql` , but uses different default values for some variables (for example, the default user, default database, and the path to TLS certificates). This is done for the user's convenience. In the Yugabyte `bin` directory, the deprecated `psql` alias opens the `ysqlsh` CLI. For more details, see [ysqlsh](../../admin/ysqlsh).

## What is the status of the YEDIS API?

In the near-term, Yugabyte is not actively working on new feature or driver enhancements to the [YEDIS](../../yedis/) API other than bug fixes and stability improvements. Current focus is on [YSQL](../../api/ysql) and [YCQL](../../api/ycql).

For key-value workloads that need persistence, elasticity and fault-tolerance, YCQL (with notion of keyspaces, tables, role-based access control and more) is often a great fit, especially if the application new rather than an existing one already written in Redis. The YCQL drivers are also more clustering aware, and hence YCQL is expected to perform better than YEDIS for equivalent scenarios. In general, our new feature development (support for data types, built-ins, TLS, backups and more), correctness testing (using Jepsen) and performance optimization is in the YSQL and YCQL areas.

## Why is consistent hash sharding the default sharding strategy?

Users primarily turn to YugabyteDB for scalability reasons. Consistent hash sharding is ideal for massively scalable workloads because it distributes data evenly across all the nodes in the cluster, while retaining ease of adding nodes into the cluster. Most use cases that require scalability do not need to perform range lookups on the primary key, so consistent hash sharding is the default sharding strategy for YugabyteDB. Common applications that do not need hash sharding include user identity (user IDs do not need ordering), product catalog (product IDs are not related to one another), and stock ticker data (one stock symbol is independent of all other stock symbols). For applications that benefit from range sharding, YugabyteDB lets you select that option.

To learn more about sharding strategies and lessons learned, see Karthik's blog on ["Four Data Sharding Strategies We Analyzed in Building a Distributed SQL Database"](https://blog.yugabyte.com/four-data-sharding-strategies-we-analyzed-in-building-a-distributed-sql-database/).
