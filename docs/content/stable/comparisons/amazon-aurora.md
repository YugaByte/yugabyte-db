---
title: Compare Amazon Aurora with YugabyteDB
headerTitle: Amazon Aurora
linkTitle: Amazon Aurora
description: Compare Amazon Aurora with YugabyteDB.
aliases:
  - /comparisons/amazon-aurora/
block_indexing: true
menu:
  stable:
    parent: comparisons
    weight: 1073
isTocNested: false
showAsideToc: true
---

Generally available since 2015, Amazon Aurora is built on a proprietary distributed storage engine that automatically replicates 6 copies of data across 3 availability zones for high availability. From an API standpoint, Aurora is wire compatible with both PostgreSQL and MySQL. As described in [“Amazon Aurora under the hood: quorums and correlated failure”](https://aws.amazon.com/blogs/database/amazon-aurora-under-the-hood-quorum-and-correlated-failure/), Aurora uses a quorum write approach based on 6 replicas. This allows for significantly better availability and durability than traditional master-slave replication. 

## Horizontal write scalability

By default, Aurora runs in a single-master configuration where only a single node can process write requests and all other nodes are read replicas. If the writer node becomes unavailable, a failover mechanism promotes one of the read-only nodes to be the new writer. 

[Multi-master](https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/aurora-multi-master.html) configuration is a recent addition to Aurora MySQL (not yet available on Aurora PostgreSQL) for scaling writes that involves a second writer node. However, since all of the data is now present in both the nodes, concurrent writes to the same data on the two nodes can lead to [write conflicts and deadlock errors](https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/aurora-multi-master.html#aurora-multi-master-deadlocks) that the application has to handle. A long list of [limitations](https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/aurora-multi-master.html#aurora-multi-master-limitations) include the inability to scale beyond the original two writer nodes as well as lack of geo-distributed writes across multiple regions.

## Multi-region active/active deployments with global consistency

Geo-distributed deployments in Aurora essentially involve a single primary region for handling writes and multiple other regions as read replicas. So globally-consistent multi-region deployments are not possible in Aurora.

## Fully open source

Aurora is Amazon's proprietary database that does not allow users to build cloud-neutral applications and run on cloud-neutral orchestration technologies like Kubernetes.

## Relevant blog posts

The following posts cover some more details around how YugabyteDB differs from Amazon Aurora.

- [What is Distributed SQL?](https://blog.yugabyte.com/what-is-distributed-sql/)
- [Comparing Distributed SQL Performance – YugabyteDB vs. Amazon Aurora PostgreSQL vs. CockroachDB](https://blog.yugabyte.com/comparing-distributed-sql-performance-yugabyte-db-vs-amazon-aurora-postgresql-vs-cockroachdb/)
- [Rise of Globally Distributed SQL Databases – Redefining Transactional Stores for Cloud Native Era](https://blog.yugabyte.com/rise-of-globally-distributed-sql-databases-redefining-transactional-stores-for-cloud-native-era/)