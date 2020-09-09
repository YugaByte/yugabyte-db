---
title: Schema Migration
headerTitle: Schema Migration
linkTitle: Schema Migration
description: How to migrate PostgreSQL schema to YugabyteDB.
block_indexing: true
menu:
  stable:
    identifier: migrate-postgresql-schema
    parent: migrate-from-postgresql
    weight: 730
isTocNested: false
showAsideToc: true
---

To convert the PostgreSQL schema to YugabyteDB schema, the following changes need to be made. 

{{< tip title="Tip" >}}

Using `ysql_dump` tool can simplify some steps of your schema migration, [read more here](#using-ysql-dump).

{{< /tip >}}


## Specify `PRIMARY KEY` inline

YugabyteDB (as of v2.2) does not support the PostgreSQL syntax of first declaring a table, and subsequently running an ALTER TABLE command to add the primary key. This is because the data in YugabyteDB tables are index-organized according to the primary key specification. There is a significant performance difference in a distributed SQL database between a table that is organized by row id with an added primary key constraint, versus a table whose data layout is index-organized from the get go.

{{< note title="Note" >}}
Altering the primary key of a table after creation is a planned feature, the current status of this enhancement is tracked [here](https://github.com/yugabyte/yugabyte-db/issues/1104).
{{< /note >}}


## Use `HASH` sort order

In YugabyteDB, the sort order of the primary key (or index) of a table determines the data distribution strategy for that primary key (or index) table across the nodes of a cluster. Thus, the choice of the sort order is critical in determining the data distribution strategy.

Indexes using the `ASC` or `DESC` sort order can efficiently handle both point and range lookups. However, they will start off with a single tablet, and therefore all reads and writes to this table will be handled by a single tablet initially. The tablet would need to undergo dynamic splitting for the table to leverage multiple nodes. Creating `ASC` or `DESC` sort orders for large datasets when range queries are not required could result in a hot shard problem.

To overcome the above issues, YugabyteDB supports `HASH` ordering in addition to the standard `ASC` and `DESC` sort orders for indexes. With HASH ordering, a hash value is first computed by applying a hash function to the values of the corresponding columns, and the hash value is sorted. Because the sort order is effectively random, this results in a random distribution of data across the various nodes in the cluster. Random distribution of data has the following properties:

* It can eliminate hot spots in the cluster by evenly distributing data across all nodes

* The table can be pre-split to utilize all nodes of a cluster right from the get go

* Range queries **cannot** be efficiently supported on the index


## Optimize DBs with many objects

{{< tip title="Tip" >}}
Databases with over 500 objects (tables, indexes and unique constraints mainly) would benefit from the colocation optimization here. Colocation also improves join performance for smaller tables.
{{< /tip >}}

In many scenarios, there may be a large number of database objects (tables and indexes specifically) which hold a relatively small dataset. In such cases, creating a separate tablet for each table and index could drastically reduce performance. Colocating these tables and indexes into a single tablet can drastically improve performance.

Enabling the colocation property at a database level causes all tables created in this database to be colocated by default. Tables in this database that hold a large dataset or those that are expected to grow in size over time can be opted out of the colocation group, which would cause them to be split into multiple tablets.

{{< note title="Note" >}}
Making colocation the default for all databases is [work in progress](https://github.com/yugabyte/yugabyte-db/issues/5239).
{{< /note >}}


## Pre-split large tables

For larger tables/indexes that are hash sharded, specify the number of initial tablet splits desired as a part of the DDL statement of the table. This can be very beneficial to distribute the data of the table across multiple nodes right from the get go. An example of specifying the number of tablets at table creation time is shown [here](https://docs.yugabyte.com/stable/architecture/docdb-sharding/tablet-splitting/#hash-sharded-tables).

For larger tables/indexes that are range sharded and the value ranges of the primary key columns are known ahead of time, pre-split them at the time of creation. This is especially beneficial for range sharded tables/indexes. Pre-split an index using the syntax shown [here](https://docs.yugabyte.com/stable/architecture/docdb-sharding/tablet-splitting/#range-sharded-tables).

## Remove collation on columns 

YugabyteDB does not currently support any collation options using the COLLATE keyword (adding [collation support is in the roadmap](https://github.com/YugaByte/yugabyte-db/issues/1127)). Remove the COLLATE options in order move the schema over to YugabyteDB.

For example, consider the table definition below.

```
CREATE TABLE test1 (
    a text COLLATE "de_DE" PRIMARY KEY,
    b text COLLATE "es_ES"
);
```

Attempting to create this table would result in the following error.
```
ERROR:  0A000: COLLATE not supported yet
LINE 2:     a text COLLATE "de_DE" PRIMARY KEY,
                   ^
HINT:  See https://github.com/YugaByte/yugabyte-db/issues/1127. Click '+' on the description to raise its priority
LOCATION:  raise_feature_not_supported_signal, gram.y:17113
Time: 31.543 ms
```

The COLLATE options should be dropped as shown below.

```
CREATE TABLE test1 (
    a text PRIMARY KEY,
    b text
);
```

## Optimize sequences (`SERIAL`)

All sequences in your schema currently use a default `CACHE` value of 1. In a distributed DB, this will result in each `INSERT` performing extra RPC calls to generate new row ids, dramatically reducing write performance. 

Consider the following table as an example.

```
CREATE TABLE contacts (
  contact_id SERIAL,
  first_name VARCHAR NOT NULL,
  last_name VARCHAR NOT NULL,
  email VARCHAR NOT NULL,
  phone VARCHAR,
  PRIMARY KEY (contact_id)
);

```

One of the following techniques is recommended (in the order of preference) to improve performance when using sequences.


### Option 1. Larger `CACHE` value for `SERIAL`

In order to use the `SERIAL` data type and not incur a performance penalty on `INSERT` operations, setting the cache size to 1000 is recommended. This can be achieved in the example table above by running an `ALTER` command on the sequence in the following manner.

```
ALTER SEQUENCE contacts_contact_id_seq CACHE 1000;
```

You can find the name of the sequence as shown below.

```
yugabyte=# SELECT pg_get_serial_sequence('contacts', 'contact_id');
     pg_get_serial_sequence
--------------------------------
 public.contacts_contact_id_seq
(1 row)
```

### Option 2. Use `UUID`s instead of `SERIAL`

The recommended option is to use UUIDs instead of the SERIAL data type. UUIDs are globally unique identifiers that can be generated on any node without requiring any global inter-node coordination. 

Some systems refer to this data type as a globally unique identifier, or GUID, instead.

A UUID is a 128-bit quantity that is generated by an algorithm chosen to make it very unlikely that the same identifier will be generated by anyone else in the known universe using the same algorithm. Therefore, for distributed systems, these identifiers provide a better uniqueness guarantee than sequence generators, which are only unique within a single database.

The table shown in the example above should be changed as shown below.

```
CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE contacts (
  contact_id uuid DEFAULT gen_random_uuid(),
  first_name VARCHAR NOT NULL,
  last_name VARCHAR NOT NULL,
  email VARCHAR NOT NULL,
  phone VARCHAR,
  PRIMARY KEY (contact_id)
);
```

## Using `ysql_dump`

The PostgreSQL utility, pg_dump can be used to dump the schema of a database. However, this schema would need  to be slightly modified as described below. 

The `ysql_dump` tool (which is a YugabyteDB specific version of the pg_dump tool) can connect to an existing PostgreSQL database and export a YugabyteDB-friendly version of the schema (and therefore includes some of the schema modifications described below). The other changes mentioned below would need to be manually performed since they depend on the use-case.

{{< note title="Note" >}}
Note that `ysql_dump` has been tested upto PostgreSQL v11.2, it may not work on very new versions of PostgreSQL.
{{< /note >}}


