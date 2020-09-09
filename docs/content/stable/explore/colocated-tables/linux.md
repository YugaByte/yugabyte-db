---
title: Explore colocated tables on Linux
headerTitle: Colocated tables
linkTitle: Colocated tables
description: Create and use colocated tables in a local YugabyteDB cluster on Linux.
block_indexing: true
menu:
  stable:
    identifier: colocated-tables-2-linux
    parent: explore
    weight: 245
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/explore/colocated-tables/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/stable/explore/colocated-tables/linux" class="nav-link active">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

</ul>

In workloads that do very little IOPS and have a small data set, the bottleneck shifts from CPU/disk/network to the number of tablets one can host per node. Since each table by default requires at least one tablet per node, a YugabyteDB cluster with 5000 relations (tables, indexes) will result in 5000 tablets per node.There are practical limitations to the number of tablets that YugabyteDB can handle per node since each tablet adds some CPU, disk and network overhead. If most or all of the tables in YugabyteDB cluster are small tables, then having separate tablets for each table unnecessarily adds pressure on CPU, network and disk.

To help accommodate such relational tables and workloads, you can colocate SQL tables.
Colocating tables puts all of their data into a single tablet, called the _colocation tablet_.
This can dramatically increase the number of relations (tables, indexes, etc.) that can
be supported per node while keeping the number of tablets per node low. Note that all the data in the colocation tablet is still replicated across three nodes (or whatever the replication factor is).

This tutorial uses the [yb-ctl](../../../admin/yb-ctl) local cluster management utility.

## 1. Create a universe

```sh
$ ./bin/yb-ctl create
```

## 2. Create a colocated database

Connect to the cluster using `ysqlsh`.

```sh
$ ./bin/ysqlsh -h 127.0.0.1
```

Create database with `colocated = true` option.

```postgresql
yugabyte=# CREATE DATABASE northwind WITH colocated = true;
```

This will create a database `northwind` whose tables will be stored on a single tablet.

## 3. Create tables

Connect to `northwind` database and create tables using standard `CREATE TABLE` command.
The tables will be colocated on a single tablet since the database was created with `colocated = true` option.

```postgresql
yugabyte=# \c northwind
yugabyte=# CREATE TABLE customers (
               customer_id bpchar,
               company_name character varying(40) NOT NULL,
               contact_name character varying(30),
               contact_title character varying(30),
            PRIMARY KEY(customer_id ASC)
           );
yugabyte=# CREATE TABLE categories (
               category_id smallint,
               category_name character varying(15) NOT NULL,
               description text,
            PRIMARY KEY(category_id ASC)
           );
yugabyte=# CREATE TABLE suppliers (
               supplier_id smallint,
               company_name character varying(40) NOT NULL,
               contact_name character varying(30),
               contact_title character varying(30),
            PRIMARY KEY(supplier_id ASC)
           );
yugabyte=# CREATE TABLE products (
               product_id smallint,
               product_name character varying(40) NOT NULL,
               supplier_id smallint,
               category_id smallint,
               quantity_per_unit character varying(20),
               unit_price real,
            PRIMARY KEY(product_id ASC),
           	FOREIGN KEY (category_id) REFERENCES categories,
           	FOREIGN KEY (supplier_id) REFERENCES suppliers
           );
```

If you go to tables view in [master UI](http://localhost:7000/tables), you'll see that all tables have the same tablet.

![customers table](/images/ce/colocated-tables-customers.png)

![categories table](/images/ce/colocated-tables-categories.png)

## 4. Opt out table from colocation

YugabyteDB has the flexibility to opt a table out of colocation. In this case, the table will use its own set of tablets instead of using the same tablet as the colocated database. This is useful for scaling out tables that are likely to be large. You can do this by using `colocated = false` option while creating table.

```postgresql
yugabyte=# CREATE TABLE orders (
    order_id smallint NOT NULL PRIMARY KEY,
    customer_id bpchar,
    order_date date,
    ship_address character varying(60),
    ship_city character varying(15),
    ship_postal_code character varying(10),
    FOREIGN KEY (customer_id) REFERENCES customers
) WITH (colocated = false);
```

If you go to tables view in [master UI](http://localhost:7000/tables), you'll see that `orders` table has its own set of tablets.

![orders table](/images/ce/colocated-tables-opt-out.png)

## 5. Reading and writing data in colocated tables

You can use standard [YSQL DML statements](../../../api/ysql) to read and write data in colocated tables. YSQL's query planner and executor
will handle routing the data to the correct tablet.

## What's next?

For more information, see the architecture for [colocated tables](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/ysql-colocated-tables.md).
