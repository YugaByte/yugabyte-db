---
title: CREATE INDEX statement [YSQL]
headerTitle: CREATE INDEX
linkTitle: CREATE INDEX
description: Use the CREATE INDEX statement to create an index on the specified columns of the specified table.
block_indexing: true
menu:
  stable:
    identifier: api-ysql-commands-create-index
    parent: api-ysql-commands
aliases:
  - /stable/api/ysql/commands/ddl_create_index
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE INDEX` statement to create an index on the specified columns of the specified table. Indexes are primarily used to improve query performance.

## Syntax

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link active" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <i class="fas fa-file-alt" aria-hidden="true"></i>
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <i class="fas fa-project-diagram" aria-hidden="true"></i>
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="grammar" class="tab-pane fade show active" role="tabpanel" aria-labelledby="grammar-tab">
    {{% includeMarkdown "../syntax_resources/commands/create_index,index_elem.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/create_index,index_elem.diagram.md" /%}}
  </div>
</div>

## Semantics

`CONCURRENTLY`, `USING method`, `COLLATE`, and `TABLESPACE` options are not yet supported.

{{< note title="Note" >}}

When an index is created on an existing table, YugabyteDB automatically backfills existing data into the index. Currently, this is not done in an online manner. To online backfill an index, you can set the `ysql_disable_index_backfill` flag to `false` when starting yb-tservers. Note that we don't recommend setting this flag in a production cluster yet. For details on how online index backfill works, see [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md).

{{< /note >}}

### UNIQUE

Enforce that duplicate values in a table are not allowed.

### INCLUDE clause

Specify a list of columns which will be included in the index as non-key columns.

### WHERE clause

A [partial index](#partial-indexes) is an index that is built on a subset of a table and includes only rows that satisfy the condition specified in the `WHERE` clause. 
It can be used to exclude NULL or common values from the index, or include just the rows of interest.
This will speed up any writes to the table since rows containing the common column values don't need to be indexed. 
It will also reduce the size of the index, thereby improving the speed for read queries that use the index.

#### *name*

 Specify the name of the index to be created.

#### *table_name*

Specify the name of the table to be indexed.

### *index_elem*

#### *column_name*

Specify the name of a column of the table.

#### *expression*

Specify one or more columns of the table and must be surrounded by parentheses.

- `HASH` - Use hash of the column. This is the default option for the first column and is used to hash partition the index table.
- `ASC` — Sort in ascending order. This is the default option for second and subsequent columns of the index.
- `DESC` — Sort in descending order.
- `NULLS FIRST` - Specifies that nulls sort before non-nulls. This is the default when DESC is specified.
- `NULLS LAST` - Specifies that nulls sort after non-nulls. This is the default when DESC is not specified.

### SPLIT INTO

For hash-sharded indexes, you can use the `SPLIT INTO` clause to specify the number of tablets to be created for the index. The hash range is then evenly split across those tablets.
Presplitting indexes, using `SPLIT INTO`, distributes index workloads on a production cluster. For example, if you have 3 servers, splitting the index into 30 tablets can provide higher write throughput on the index. For an example, see [Create an index specifying the number of tablets](#create-an-index-specifying-the-number-of-tablets).

{{< note title="Note" >}}

By default, YugabyteDB presplits an index into `ysql_num_shards_per_tserver * num_of_tserver` tablets. The `SPLIT INTO` clause can be used to override that setting on a per-index basis.

{{< /note >}}

## Examples

### Unique index with HASH column ordering

Create a unique index with hash ordered columns.

```plpgsql
yugabyte=# CREATE TABLE products(id int PRIMARY KEY,
                                 name text,
                                 code text);
yugabyte=# CREATE UNIQUE INDEX ON products(code);
yugabyte=# \d products
              Table "public.products"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 id     | integer |           | not null |
 name   | text    |           |          |
 code   | text    |           |          |
Indexes:
    "products_pkey" PRIMARY KEY, lsm (id HASH)
    "products_code_idx" UNIQUE, lsm (code HASH)
```

### ASC ordered index

Create an index with ascending ordered key.

```plpgsql
yugabyte=# CREATE INDEX products_name ON products(name ASC);
yugabyte=# \d products_name
   Index "public.products_name"
 Column | Type | Key? | Definition
--------+------+------+------------
 name   | text | yes  | name
lsm, for table "public.products
```

### INCLUDE columns

Create an index with ascending ordered key and include other columns as non-key columns

```plpgsql
yugabyte=# CREATE INDEX products_name_code ON products(name) INCLUDE (code);
yugabyte=# \d products_name_code;
 Index "public.products_name_code"
 Column | Type | Key? | Definition
--------+------+------+------------
 name   | text | yes  | name
 code   | text | no   | code
lsm, for table "public.products"
```

### Create an index specifying the number of tablets

To specify the number of tablets for an index, you can use the `CREATE INDEX` statement with the [`SPLIT INTO`](#split-into) clause.

```plpgsql
CREATE TABLE employees (id int PRIMARY KEY, first_name TEXT, last_name TEXT) SPLIT INTO 10 TABLETS;
CREATE INDEX ON employees(first_name, last_name) SPLIT INTO 10 TABLETS;
```

### Partial indexes

Consider an application maintaining shipments information. It has a `shipments` table with a column for `delivery_status`. If the application needs to access in-flight shipments frequently, then it can use a partial index to exclude rows whose shipment status is `delivered`.

```plpgsql
yugabyte=# create table shipments(id int, delivery_status text, address text, delivery_date date);
yugabyte=# create index shipment_delivery on shipments(delivery_status, address, delivery_date) where delivery_status != 'delivered';
```
