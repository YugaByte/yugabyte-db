---
title: Tablet splitting
headerTitle: Tablet splitting
linkTitle: Tablet splitting
description: Learn how YugabyteDB splits tablets.
block_indexing: true
menu:
  v2.1:
    identifier: docdb-tablet-splitting
    parent: architecture-docdb-sharding
    weight: 1143
isTocNested: true
showAsideToc: true
---

Tablet splitting enables changing the number of tablets at runtime. This enables *re-shrading* the existing data in the cluster.

## Overview

There are a number of scenarios where this is useful.

### Range scans
In use-cases that scan a range of data, the data is stored in the natural sort order (also known as range-sharding). In these usage patterns, it is often impossible to predict a good split boundary ahead of time. For example:

```
CREATE TABLE census_stats (
    age INTEGER,
    user_id INTEGER,
    ...
    );
```    
In the table above, it is not possible for the database to infer the range of values for age (typically in the 1 to 100 range). It is also impossible to predict the distribution of rows in the table, meaning how many user_id rows will be inserted for each value of age to make an evenly distributed data split. This makes it hard to pick good split points ahead of time.

### Low-cardinality primary keys
In use-cases with a low-cardinality of the primary keys (or the secondary index), hashing is not very effective. For example, if we had a table where the primary key (or index) was the column gender which has only two values Male and Female, hash sharding would not be very effective. However, it is still desirable to use the entire cluster of machines to maximize serving throughput.

### Small tables that become very large
This feature is also useful for use-cases where tables begin small, and thereby start with a few shards. If these tables grow very large, then nodes continuously get added to the cluster. We may reach a scenario where the number of nodes exceeds the number of tablets. Such cases require tablet splitting to effectively re-balance the cluster.


## Approaches to tablet splitting

DocDB allows the following mechanisms to re-shard data by splitting tablets:

* **Pre-splitting** All tables created in DocDB can be split into the desired number of tablets at creation time.

* **Manual splitting** The tablets in a running cluster can be split manually at runtime by the user.

* **Dynamic splitting** The tablets in a running cluster are automatically split according to some policy by the database.

The sections below go into the details of how to pre-split tablets.


## Pre-splitting

As the name suggests, the table is split at creation time into a desired number of tablets. Pre-splitting of tablets is supported for both *range* and *hash* sharded tables. The number of tablets can be spicified in one of two ways:

* **Desired number of tablets** In this case, the table is created with the desired number of tablets.
* **Desired number of tablets per node** In this case, the total number of tablets the table is split into is computed as follows:
    ```
    num_tablets_in_table = num_tablets_per_node * num_nodes_at_table_creation_time
    ```

{{< note title="Note" >}}

In order to pre-split a table into a desired number of tablets, we need the start and end key for each tablet. This makes pre-splitting slightly different for hash-sharded and range-sharded tables.

{{</note >}}


### Hash-sharded tables

 Since hash sharding works by applying a hash function on a subset (or all) of the primary key columns, the byte space of the hash sharded keys is known ahead of time. For example, if we take a 2-byte hash, the byte space would be `[0x0000, 0xFFFF]`. 

![tablet_hash_1](/images/architecture/tablet_hash_1.png)

In the diagram above, the table is split into 16 shards. This can be achieved by using the `SPLIT INTO 16 TABLETS` clause as a part of the `CREATE TABLE` statement as shown below.

```
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
) SPLIT INTO 16 TABLETS;
```

The above example shows how a table can be pre-split in YSQL, see the [YSQL reference section](../../../api/ysql/commands/ddl_create_table/#create-a-table-specifying-the-number-of-tablets) for more detail. For the YCQL API, this can be achieved as described in the [YCQL `CREATE TABLE` API reference section](../../../api/ycql/ddl_create_table/#create-a-table-specifying-the-number-of-tablets).

### Range-sharded tables

In range-sharded tables, the start and end key for each tablet is not immediately known since this depends on the column type and the intended usage. For example, if the primary key is a `percentage NUMBER` column where the range of values is in the `[0, 100]` range, pre-splitting on the entire `NUMBER` space would not be effective.

For this reason, in order to pre-split range sharded tables, it is necessary to explicitly specify the split points. These explicit split points can be specified as shown below.

```
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
    PRIMARY KEY (customer_id ASC)
) SPLIT AT ((1000), (2000), (3000), ... );
```

## Manual splitting

Imagine there is a table with pre-exsiting data spread across a certain number of tablets. It is possible to split some or all of the tablets in this table manually. This is shown in the example below.

#### Create sample YSQL table

First let us create a three node local cluster.

```bash
$ bin/yb-ctl --rf=3 create --num_shards_per_tserver=1
```

Let us create a sample table and insert some data.

```postgres
yugabyte=# CREATE TABLE t (k VARCHAR, v TEXT, PRIMARY KEY (k)) SPLIT INTO 1 TABLETS;
```

Next, we insert some sample data (100K rows) into this table.

```postgres
yugabyte=# INSERT INTO t (k, v) 
             SELECT i::text, left(md5(random()::text), 4)
             FROM generate_series(1, 100000) s(i);
```

```
yugabyte=# select count(*) from t;
 count
--------
 100000
(1 row)

```

#### Verify table has one tablet

In order to verify that the table `t` has only one tablet, we just list all the tablets for table `t`. The set of tablets in any table can be queried using the `yb-admin` tool as shown below.

```bash
$ bin/yb-admin --master_addresses 127.0.0.1:7100 list_tablets ysql.yugabyte t
```

This would produce the following output. Note the tablet UUID for use later on to split this tablet.

```
Tablet UUID                       Range                         Leader
9991368c4b85456988303cd65a3c6503  key_start: "" key_end: ""     127.0.0.1:9100
```


#### Manually split the table

The tablet of this table can be manually split into two tablets by running the following command.

```
bin/yb-admin --master_addresses <MASTER_ADDRESSES> split_tablet <TABLET_ID_TO_SPLIT>
```

In our scenario, this command would look as follows.

```bash
$ bin/yb-admin \
    --master_addresses 127.0.0.1:7100,127.0.0.2:7100,127.0.0.3:7100 \
    split_tablet 9991368c4b85456988303cd65a3c6503
```

After the split, we should see two tablets for the table `t`.

```bash
$ bin/yb-admin --master_addresses 127.0.0.1:7100 list_tablets ysql.yugabyte t
```

```
Tablet UUID                       Range                                 Leader
20998f68c3fa4d299e8af7c04410e230  key_start: "" key_end: "\177\377"     127.0.0.1:9100
a89ecb84ad1b488b893b6e7762a6ca2a  key_start: "\177\377" key_end: ""     127.0.0.3:9100
```

{{< note title="Note" >}}

There are a few important things to note here. 
* The original tablet `9991368c4b85456988303cd65a3c6503` is no longer present in the system. It has been replaced with two new tablets. 
* The tablet leaders are now spread across two nodes, in order to evenly balance the tablets for a table across the nodes in the cluster.

{{</note >}}

## Dynamic splitting

Dynamic splitting is currently a [work-in-progress](https://github.com/yugabyte/yugabyte-db/issues/1004) feature and is not yet available. Design for this feature can be reviewed on [GitHub](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md).

