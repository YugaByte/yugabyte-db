---
title: Best practices
linkTitle: Best practices
description: Best practices when using YugabyteDB
menu:
  latest:
    identifier: best-practices-ysql
    parent: develop
    weight: 582
aliases:
  - /latest/quick-start/best-practices-ysql/
isTocNested: 4
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="{{< ref "best-practices.md" >}}" class="nav-link">
      <i class="icon-" aria-hidden="true"></i>
      DocDB
    </a>
  </li>
  <li >
    <a href="" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="{{< ref "best-practices-ycql.md" >}}" class="nav-link"</a>
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>


## Core features

### Hash vs range sharding
Choosing the type of partitioning in primary-keys and indexes can be hard. 
There are differences how writes and reads are spread in a distributed database.
Here is a [guide](/latest/architecture/docdb-sharding/sharding) on which partitioning strategy to chose.

## Performance

### Connection pooling
YugabyteDB uses the upper half of Postgresql to implement it's YSQL layer.
Each connection in Postgresql forks a new process, has at least 10MB of overhead in RAM, 
has context-switching overhead and is generally slow to create. 

Because of each connections overhead, we have to be careful regarding the number
of opened connections that we keep for each tserver. Currently the maximum number 
per-tserver is `300` and is set by the [`--ysql_max_connections`](../reference/configuration/yb-tserver.md) gflag.

This overhead is usually mitigated by pooling connections in the client code or in an external connection pooler.
One connection pooler that we've tested is [pgbouncer](https://www.pgbouncer.org/).

But any connection pooler that works with Postgresql should work with YugabyteDB.
We're also working to lower the overhead of connections.

### Co-location
[Co-location](../explore/colocated-tables/linux.md)puts tables & indexes of a database inside 1 tablet. This increases performance and lowers latency on write transactions, joined queries and aggregations because they happen in 1 tablet/node.
If you have a small dataset (<500GB) that requires HA, or a DB with only few scale out tables, then colocation is a good option.


### Use partial indexes
[A partial index](../api/ysql/commands/ddl_create_index.md#where-clause) is an index that is built on a subset of a table and includes only rows that satisfy the condition 
specified in the `WHERE` clause. This will speed up any writes to the table and reduce the size of the index, thereby improving the speed for read queries that use the index. 

### Use covering indexes
When querying by a secondary index, the original table is consulted to get the columns that aren't specified in the 
index. This can result in multiple random reads across the main table.

Sometimes a better way is to include the other columns that we're querying that aren't part of the index 
using the [`INCLUDE`](../api/ysql/commands/ddl_create_index.md#include-columns) clause.  
When additional columns are included in the index, they can be used to respond to queries directly using index only scans
that are faster.

This turns a (possible) random read from the main table to just a filter on the index.


### Column size limit
For consistent latency/performance, we suggest keeping columns in the `2MB` range 
or less even though we support an individual column being about 32MB.

### Row size limit
Big columns add up when selecting full rows or multiple of them. 
For consistent latency/performance, we suggest keeping the size in the `32MB` range
or less. This is a combination of [column sizing recommendations](#column-size-limit) for all columns.

### Use multi row inserts wherever possible
If you're inserting multiple rows, it's faster to batch them together whenever possible. You can start with 128 rows per-batch
and test different amounts to find the sweet spot.

Don't use multiple statements:
```postgresql
INSERT INTO users(name,surname) VALUES ('bill', 'jane');
INSERT INTO users(name,surname) VALUES ('billy', 'bob');
INSERT INTO users(name,surname) VALUES ('joey', 'does');
```
Group values into a single statement:
```postgresql
INSERT INTO users(name,surname) VALUES ('bill', 'jane'), ('billy', 'bob'), ('joe', 'does');
``` 

### UPSERT multiple rows wherever possible

PostgreSQL and YSQL enable you to do upserts using the `INSERT ON CONFLICT` clause. Similar to multi-row inserts, 
you can also batch multiple upserts in a single `INSERT ON CONFLICT` statement for better performance.

In case the row already exists, you can access the existing values using `EXCLUDED.<column_name>` in the query.

In the example below we have a table with where we are keeping counters of different products and incrementing rows in batches:

```postgresql
CREATE TABLE products
  (
     NAME     TEXT PRIMARY KEY,
     quantity BIGINT DEFAULT 0
  );  

---

INSERT INTO products(name, quantity) 
VALUES 
  ('apples', 1), 
  ('oranges', 5) ON CONFLICT(name) DO UPDATE 
SET 
  quantity = products.quantity + excluded.quantity;

---

INSERT INTO products(name, quantity) 
VALUES 
  ('apples', 1), 
  ('oranges', 5) ON CONFLICT(name) DO UPDATE 
SET 
  quantity = products.quantity + excluded.quantity;

---

SELECT *
FROM   products;

  name   | quantity
---------+----------
 oranges |   	10
 apples  |    2

(2 rows)
```


### YSQL proxy & load balancer 
Currently YugabyteDB uses the same drivers as Postgresql. Usually they are implemented on top of the
[libpq](https://www.postgresql.org/docs/current/libpq.html) c client. 
Postgresql drivers aren't created with assumptions of a 
distributed/sharded database and they're not cluster-aware to redirect & load balance reads/writes
to the cluster. 
For all scenarios, there are several solutions using proxies & load-balancers that 
you can use and are tested in production:

1. You can specify multiple hosts in the connection string for any driver that uses [`libpq`](https://www.postgresql.org/docs/11/libpq-connect.html)
2. [haproxy](http://www.haproxy.org/) load balancer. Can be used with [pgsql-check](http://cbonte.github.io/haproxy-dconv/2.2/configuration.html#option%20pgsql-check) 
plugin which periodically checks if servers are up and adds/removes them from the pool.
3. [consul](https://www.consul.io/) can be used for service discovery of the tserver nodes
On the client node the consul agent will provide a hostname which will point to a 
server that is up. You can use that hostname in the connection string to YugabyteDB.
This way you keep only 1 connection pool to 1 server. When the server goes down,
consul will return a new hostname which will be able to serve queries.
4. [nginx](http://nginx.org/) can be used as a tcp proxy & load balancer in front
of a cluster of YugabyteDB.
5. On kubernetes, YugabyteDB uses it's [load balancer](../deploy/kubernetes/_index.html) to proxy the tservers.
6. In GCP you can use [Cloud Load Balancer](https://cloud.google.com/load-balancing/) as a TCP proxy.
7. In AZURE you can use [Load Balancer](https://azure.microsoft.com/en-us/services/load-balancer/) as a TCP proxy. 

{{< note title="Note" >}}
- Make sure to set forwarding rules to port :5433.
- Configure health checks using HTTP port :7000 and path `/status`.
- The load balancer is only needed for yb-tservers. Clients don't need to connect to Masters.
{{< /note >}}


### Number of tables
Each table is split into tablets and each tablet has overhead. 
See [tablets per server](#tablets-per-server) for limits.

### Tablets per server
Each table consists of several tablets based on the `yb_num_shards_per_tserver`
gflag. Each `tablet` has overhead in the memory of the tserver that it resides.
Currently we recommend a maximum of around `500` tablets on each tserver.
You have to keep this number in mind depending on the number of tables and number 
of tablets per-server that you intend to create.
We're [actively working](https://github.com/yugabyte/yugabyte-db/issues/1317) to increase this limit.

There are different ways to reduce number of tablets:

1. Use [colocation](../explore/colocated-tables/linux.md) to group small tables into 1 tablet.
2. Reduce number of tablets per table using [`--ysql_num_shards_per_tserver`](../reference/configuration/yb-tserver.md) gflag.
3. Use [`SPLIT INTO`](../api/ysql/commands/ddl_create_table.md#split-into) clause when creating the table.

### Create Primary Key when creating the table
YugabyteDB YSQL layer requires the `Primary Key` to be specified in the `CREATE TABLE` statement upfront. 

### Create indexes and foreign keys when creating the table
It is better to create indexes and foreign keys at the time of table creation instead of adding them after loading data.

### Use UUIDs instead of auto-increment serial IDs
Auto-incremented IDs are not recommended in YugabyteDB because they can introduce a bottleneck and may lower write concurrency 
by creating hotspots. Since auto-increment IDs are generated by using a single row in a system table that maintains the ID, 
the tablet hosting the row becomes a hotspot. Furthermore, auto-incremented IDs are logically close together and can 
end up writing to a set of few tablets (if table uses range sharding), thereby increasing contention and load on those tablets. 

A better way to get similar results is to use [`uuid`](../api/ysql/datatypes/type_uuid.md) columns. They do not introduce a single point of bottleneck, 
can be generated in the server or client and are uniformly distributed across tablets when inserting new rows.

In the example below, we first create the extension [`pgcrypto`](../api/ysql/extensions.md#pgcrypto) which enables automatically generating `uuid` values 
with the `gen_random_uuid()` function and use it as the default value:

```postgresql
yb_demo=# CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION

yb_demo=# CREATE TABLE users(id uuid PRIMARY KEY DEFAULT gen_random_uuid(), name TEXT);
CREATE TABLE

yb_demo=# INSERT INTO users(name) VALUES ('ben'),('bill');
INSERT 0 2

yb_demo=# SELECT * FROM users;
              	id              	| name
--------------------------------------+------
 47c2e90f-6a27-4d49-b694-b8a392ac4f70 | bill
 a69c219c-ca6b-428e-b35e-ef04d29cb5f3 | ben
(2 rows)
```

However, if your application still requires auto-incrementing IDs, then you can create those using 
[`serial`](../api/ysql/datatypes/type_serial.md) (4 byte integer) or [`bigserial`](../api/ysql/datatypes/type_serial.md) (8 byte integer) columns.

Serial columns use [sequences](../api/ysql/commands/ddl_create_sequence.md) for generating the IDs. To alleviate the hotspot problem that sequences cause, 
we recommend configuring a [cache](../api/ysql/commands/ddl_create_sequence.md#cache) for sequences that will pre-allocate 
a set of auto-incrementing IDs and store them in memory for faster access.

```postgresql
yb_demo=# CREATE SEQUENCE user_id_sequence CACHE 10000;
CREATE SEQUENCE
yb_demo=# CREATE TABLE users(id BIGINT PRIMARY KEY DEFAULT nextval('user_id_sequence'), name TEXT);
CREATE TABLE
yb_demo=# INSERT INTO users(name) VALUES ('ben'),('bill');
INSERT 0 2
yb_demo=# SELECT * FROM users;
 id | name
----+------
  2 | bill
  1 | ben
(2 rows)
```


## Miscellaneous

### JSONB datatype
YugabyteDB has [`jsonb`](../api/ysql/datatypes/type_json.md) datatype that makes it easy to model json data which does not have a set schema and might change often. 
It is the same as Postgresql [`jsonb`](https://www.postgresql.org/docs/11/datatype-json.html) datatype.
You can use jsonb to group less interesting / lesser accessed columns of a table. 
YSQL also supports JSONB expression indexes that can be used to speed up data retrieval that would otherwise require scanning the json entries.

{{< note title="Use jsonb columns only when necessary" >}}
`jsonb` columns are slower to read/write compared to normal columns.
 
They also take more space because they need to store keys in strings and make keeping data consistency harder and 
needing complex queries to get/set jsonb values. 
A good schema design is to keep most columns as regular ones and only using `jsonb` for truly dynamic values. 
Don't create a `data jsonb` column where you put everything, but a `dynamic_data jsonb` column and other ones being 
primitive columns.

{{< /note >}}


### Multi tenant use cases
There are many cases where data is spread across many tenants. Usually each 
tenant is isolated with it's own data. In these cases users may be inclined to 
create per-tenant tables/databases.
Because each table/tablet has overhead, having a large number of them is not 
recommended.
Depending on the number of tenants, it may be better to have a `tenant_id` on 
primary-keys which can be used to filter tenants at write/query time.
This can be combined with moving large tenants to their own private tables.


### Use `TRUNCATE` to empty tables instead of `DELETE`
`TRUNCATE` deletes the database files that store the table and is very fast. 
While DELETE inserts a `delete marker` for each row  in transactions and they are removed from storage when a compaction 
runs.

