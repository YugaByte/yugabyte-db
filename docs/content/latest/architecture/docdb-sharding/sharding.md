---
title: Hash & range sharding
headerTitle: Hash & range sharding
linkTitle: Hash & range sharding
description: Learn how YugabyteDB uses hash and range sharding for horizontal scaling.
aliases:
  - /latest/architecture/docdb/sharding/
  - /latest/architecture/concepts/sharding/
  - /latest/architecture/concepts/docdb/sharding/
menu:
  latest:
    identifier: docdb-sharding
    parent: architecture-docdb-sharding
    weight: 1142
isTocNested: true
showAsideToc: true
---

Sharding is the process of breaking up large tables into smaller chunks called shards that are spread across multiple servers. A shard is essentially a horizontal data partition that contains a subset of the total data set, and hence is responsible for serving a portion of the overall workload. The idea is to distribute data that can’t fit on a single node onto a cluster of database nodes. Sharding is also referred to as horizontal partitioning. The distinction between horizontal and vertical comes from the traditional tabular view of a database. A database can be split vertically — storing different table columns in a separate database, or horizontally — storing rows of the same table in multiple database nodes.


User tables are implicitly managed as multiple shards by DocDB. These shards are referred to as **tablets**. The primary key for each row in the table uniquely determines the tablet the row lives in. This is shown in the figure below.

![Sharding a table into tablets](/images/architecture/partitioning-table-into-tablets.png)

{{< note title="Note" >}}
For every given key, there is exactly one tablet that owns it.
{{< /note >}}

YugabyteDB currently supports two ways of sharding data - hash (aka consistent hash) sharding and range sharding.

## Hash sharding

With (consistent) hash sharding, data is evenly and randomly distributed across shards using a partitioning algorithm. Each row of the table is placed into a shard determined by computing a consistent hash on the partition column values of that row. This is shown in the figure below.

![tablet_hash_1](/images/architecture/tablet_hash_1.png)

The hash space for hash-sharded YugabyteDB tables is the 2-byte range from 0x0000 to 0xFFFF. Such
a table may therefore have at most 64K tablets. We expect this to be sufficient in practice even for
very large data sets or cluster sizes. As an example, for a table with 16 tablets the overall hash space [0x0000 to 0xFFFF) is divided into 16 sub-ranges, one for each tablet:  [0x0000, 0x1000), [0x1000, 0x2000), … , [0xF000, 0xFFFF]. Read and write operations are processed by converting the primary key into an internal key and its hash value, and determining what tablet the operation should be routed to. The figure below illustrates this.

![tablet_hash](/images/architecture/tablet_hash.png)

The insert/update/upsert by the end user is processed by serializing and hashing the primary key into byte-sequences and determining the tablet they belong to. Let us assume that the user is trying to insert a key k with a value v into a table T. The figure below illustrates how the tablet owning the key for the above table is determined.

![tablet_hash_2](/images/architecture/tablet_hash_2.png)

### Example

- YSQL table created with hash sharding.

```postgres
CREATE TABLE customers (
    customer_id bpchar NOT NULL,
    company_name character varying(40) NOT NULL,
    contact_name character varying(30),
    contact_title character varying(30),
    address character varying(60),
    city character varying(15),
    region character varying(15),
    postal_code character varying(10),
    country character varying(15),
    phone character varying(24),
    fax character varying(24),
    PRIMARY KEY (customer_id HASH)
);
```

- YCQL tables can be created with hash sharding only, hence an explict syntax for setting hash sharding is not necessary.

```postgres
CREATE TABLE items (
	supplier_id INT,
    item_id INT,
    supplier_name TEXT STATIC,
    item_name TEXT,
    PRIMARY KEY((supplier_id), item_id)
);
```

### Pros
This sharding strategy is ideal for massively scalable workloads because it distributes data evenly across all the nodes in the cluster, while retaining ease of adding nodes into the cluster. [Algorithmic hash sharding](https://blog.yugabyte.com/four-data-sharding-strategies-we-analyzed-in-building-a-distributed-sql-database/) is very effective also at distributing data across nodes, but the distribution strategy depends on the number of nodes. With consistent hash sharding, there are many more shards than the number of nodes and there is an explicit mapping table maintained tracking the assignment of shards to nodes. When adding new nodes, a subset of shards from existing nodes can be efficiently moved into the new nodes without requiring a massive data reassignment.

### Cons
Performing range queries could be inefficient. Examples of range queries are finding rows greater than a lower bound or less than an upper bound (as opposed to point lookups).

## Range sharding

Range sharding involves splitting the rows of a table into contiguous ranges that respect the sort order of the table based on the primary key column values. The tables that are range sharded usually start out with a single shard. As data is inserted into the table, it is dynamically split into multiple shards because it is not always possible to know the distribution of keys in the table ahead of time. The basic idea behind range sharding is shown in the figure below.

![tablet_range_1](/images/architecture/tablet_range_1.png)

### Example

- YSQL table created with range sharding.

```postgres
CREATE TABLE order_details (
    order_id smallint NOT NULL,
    product_id smallint NOT NULL,
    unit_price real NOT NULL,
    quantity smallint NOT NULL,
    discount real NOT NULL,
    PRIMARY KEY (order_id ASC, product_id),
    FOREIGN KEY (product_id) REFERENCES products,
    FOREIGN KEY (order_id) REFERENCES orders
);
```

- YCQL tables cannot be created with range sharding. They can be created with hash sharding only.

### Pros
This type of sharding allows efficiently querying a range of rows by the primary key values. Examples of such a query is to look up all keys that lie between a lower bound and an upper bound.

### Cons
Range sharding leads to a number of issues in practice at scale, some of which are similar to that of linear hash sharding.

Firstly, when starting out with a single shard implies only a single node is taking all the user queries. This often results in a database “warming” problem, where all queries are handled by a single node even if there are multiple nodes in the cluster. The user would have to wait for enough splits to happen and these shards to get redistributed before all nodes in the cluster are being utilized. This can be a big issue in production workloads. This can be mitigated in some cases where the distribution is keys is known ahead of time by presplitting the table into multiple shards, however this is hard in practice.

Secondly, globally ordering keys across all the shards often generates hot spots: some shards will get much more activity than others, and the node hosting those will be overloaded relative to others. While these can be mitigated to some extent with active load balancing, this does not always work well in practice because by the time hot shards are redistributed across nodes, the workload could change and introduce new hot spots.

## Additional reading

Following blogs highlight additional details related to sharding.

- [How Data Sharding Works in a Distributed SQL Database](https://blog.yugabyte.com/how-data-sharding-works-in-a-distributed-sql-database/)

- [Four Data Sharding Strategies We Analyzed in Building a Distributed SQL Database](https://blog.yugabyte.com/four-data-sharding-strategies-we-analyzed-in-building-a-distributed-sql-database/)

- [Overcoming MongoDB Sharding and Replication Limitations with YugabyteDB](https://blog.yugabyte.com/overcoming-mongodb-sharding-and-replication-limitations-with-yugabyte-db/)


