---
title: Explore auto sharding on Kubernetes
headerTitle: Auto sharding
linkTitle: Auto sharding
description: Follow this tutorial (on Kubernetes) to learn how YugabyteDB automatically splits tables into shards (tablets).
aliases:
  - /stable/explore/auto-sharding-kubernetes/
block_indexing: true
menu:
  stable:
    identifier: auto-sharding-4-kubernetes
    parent: explore
    weight: 225
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/explore/auto-sharding/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/stable/explore/auto-sharding/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>
<!--
  <li >
    <a href="/stable/explore/auto-sharding-docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>
-->
<!--
  <li >
    <a href="/stable/explore/auto-sharding-kubernetes" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
-->
</ul>

YugabyteDB automatically splits user tables into multiple shards, called *tablets*. The primary key for each row in the table uniquely determines the tablet the row lives in. For data distribution purposes, a hash based partitioning scheme is used. Read more about [how sharding works](../../../architecture/docdb/sharding/) in YugabyteDB.

By default, YugabyteDB creates eight tablets per node in the cluster for each table and automatically distributes the data across the various tablets, which in turn are distributed evenly across the nodes. In this tutorial, you will explore how automatic sharding is done internally for tables. The system Redis table works in an identical manner.

We will explore automatic sharding inside YugabyteDB by creating these tables:

- Use a replication factor (RF) of `1`. This will make it easier to understand how automatic sharding is achieved independent of data replication.
- Insert entries one by one, and examine which how the data gets distributed across the various nodes.

If you haven't installed YugabyteDB yet, do so first by following the [Quick start](../../../quick-start/install/) guide.

## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./yb-docker-ctl destroy
```

Start a new local universe with replication factor 1.

```sh
$ ./yb-docker-ctl create --rf 1 
```

The above command creates a universe with one node. Let us add 2 more nodes to this universe. You can do that by running the following:

```sh
$ ./yb-docker-ctl add_node
$ ./yb-docker-ctl add_node
```

Create a YCQL table. The keyspace and table name below must be named as shown below, since the sample application writes data to this table. We will use the sample application to write data to this table to understand sharding in a subsequent step.

```sh
$ ./bin/ycqlsh
```

```sql
ycqlsh> CREATE KEYSPACE ybdemo_keyspace;
```

```sql
ycqlsh> CREATE TABLE ybdemo_keyspace.cassandrakeyvalue (k text PRIMARY KEY, v blob);
```

## 2. Examine tablets

For each table, YugabyteDB creates 8 shards per node in the universe by default. In our example, since we have 3 nodes, we expect 24 tablets for each of the tables we created (the Redis and YCQL tables), or 48 tablets total.

You can see the number of tablets per node in the Tablet Servers page of the master Admin UI, by going to http://127.0.0.1:7000/tablet-servers. The page should look something like the image below:

You can also navigate to the table details for these two tables by going to <URL>. This page should look as follows.

Note here that the tablets balancing across nodes happens on a per-table basis, so that each table is scaled out to an appropriate number of nodes.

## 3. Insert/query the Redis table

## 4. Insert/query the Cassandra table

## 5. Clean up (optional)

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./yb-docker-ctl destroy
```
