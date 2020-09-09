---
title: CREATE INDEX statement [YCQL]
headerTitle: CREATE INDEX
linkTitle: CREATE INDEX
summary: Create a new index on a table
description: Use the CREATE INDEX statement to create a new index on a table.
menu:
  latest:
    parent: api-cassandra
    weight: 1225
aliases:
  - /latest/api/cassandra/ddl_create_index
  - /latest/api/ycql/ddl_create_index
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE INDEX` statement to create a new index on a table. It defines the index name, index columns, and additional columns to include.

## Syntax

### Diagram

#### create_index

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="587" height="150" viewbox="0 0 587 150"><path class="connector" d="M0 22h15m68 0h10m59 0h30m32 0h10m46 0h10m64 0h20m-197 0q5 0 5 5v8q0 5 5 5h172q5 0 5-5v-8q0-5 5-5m5 0h10m97 0h10m39 0h7m2 0h2m2 0h2m-535 50h2m2 0h2m2 0h7m95 0h10m25 0h10m162 0h30m170 0h20m-205 0q5 0 5 5v8q0 5 5 5h180q5 0 5-5v-8q0-5 5-5m5 0h10m25 0h7m2 0h2m2 0h2m-587 50h2m2 0h2m2 0h27m224 0h20m-259 0q5 0 5 5v8q0 5 5 5h234q5 0 5-5v-8q0-5 5-5m5 0h30m132 0h20m-167 0q5 0 5 5v8q0 5 5 5h142q5 0 5-5v-8q0-5 5-5m5 0h15"/><polygon points="0,29 5,22 0,15" style="fill:black;stroke-width:0"/><rect class="literal" x="15" y="5" width="68" height="25" rx="7"/><text class="text" x="25" y="22">CREATE</text><rect class="literal" x="93" y="5" width="59" height="25" rx="7"/><text class="text" x="103" y="22">INDEX</text><rect class="literal" x="182" y="5" width="32" height="25" rx="7"/><text class="text" x="192" y="22">IF</text><rect class="literal" x="224" y="5" width="46" height="25" rx="7"/><text class="text" x="234" y="22">NOT</text><rect class="literal" x="280" y="5" width="64" height="25" rx="7"/><text class="text" x="290" y="22">EXISTS</text><a xlink:href="../grammar_diagrams#index-name"><rect class="rule" x="374" y="5" width="97" height="25"/><text class="text" x="384" y="22">index_name</text></a><rect class="literal" x="481" y="5" width="39" height="25" rx="7"/><text class="text" x="491" y="22">ON</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="15" y="55" width="95" height="25"/><text class="text" x="25" y="72">table_name</text></a><rect class="literal" x="120" y="55" width="25" height="25" rx="7"/><text class="text" x="130" y="72">(</text><a xlink:href="../grammar_diagrams#partition-key-columns"><rect class="rule" x="155" y="55" width="162" height="25"/><text class="text" x="165" y="72">partition_key_columns</text></a><a xlink:href="../grammar_diagrams#clustering-key-columns"><rect class="rule" x="347" y="55" width="170" height="25"/><text class="text" x="357" y="72">clustering_key_columns</text></a><rect class="literal" x="547" y="55" width="25" height="25" rx="7"/><text class="text" x="557" y="72">)</text><a xlink:href="../grammar_diagrams#clustering-key-column-ordering"><rect class="rule" x="35" y="105" width="224" height="25"/><text class="text" x="45" y="122">clustering_key_column_ordering</text></a><a xlink:href="../grammar_diagrams#covering-columns"><rect class="rule" x="309" y="105" width="132" height="25"/><text class="text" x="319" y="122">covering_columns</text></a><polygon points="472,129 476,129 476,115 472,115" style="fill:black;stroke-width:0"/></svg>

#### partition_key_columns

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="286" height="95" viewbox="0 0 286 95"><path class="connector" d="M0 22h35m106 0h130m-251 0q5 0 5 5v50q0 5 5 5h5m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h46q5 0 5 5v20q0 5-5 5m-5 0h30m25 0h5q5 0 5-5v-50q0-5 5-5m5 0h15"/><polygon points="0,29 5,22 0,15" style="fill:black;stroke-width:0"/><a xlink:href="../grammar_diagrams#index-column"><rect class="rule" x="35" y="5" width="106" height="25"/><text class="text" x="45" y="22">index_column</text></a><rect class="literal" x="35" y="65" width="25" height="25" rx="7"/><text class="text" x="45" y="82">(</text><rect class="literal" x="131" y="35" width="24" height="25" rx="7"/><text class="text" x="141" y="52">,</text><a xlink:href="../grammar_diagrams#index-column"><rect class="rule" x="90" y="65" width="106" height="25"/><text class="text" x="100" y="82">index_column</text></a><rect class="literal" x="226" y="65" width="25" height="25" rx="7"/><text class="text" x="236" y="82">)</text><polygon points="282,29 286,29 286,15 282,15" style="fill:black;stroke-width:0"/></svg>

#### clustering_key_columns

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="176" height="65" viewbox="0 0 176 65"><path class="connector" d="M0 52h35m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h46q5 0 5 5v20q0 5-5 5m-5 0h35"/><polygon points="0,59 5,52 0,45" style="fill:black;stroke-width:0"/><rect class="literal" x="76" y="5" width="24" height="25" rx="7"/><text class="text" x="86" y="22">,</text><a xlink:href="../grammar_diagrams#index-column"><rect class="rule" x="35" y="35" width="106" height="25"/><text class="text" x="45" y="52">index_column</text></a><polygon points="172,59 176,59 176,45 172,45" style="fill:black;stroke-width:0"/></svg>

#### clustering_key_column_ordering

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="641" height="100" viewbox="0 0 641 100"><path class="connector" d="M0 52h15m54 0h10m99 0h10m63 0h10m35 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h98m24 0h98q5 0 5 5v20q0 5-5 5m-109 0h30m45 0h29m-84 25q0 5 5 5h5m54 0h5q5 0 5-5m-79-25q5 0 5 5v33q0 5 5 5h64q5 0 5-5v-33q0-5 5-5m5 0h30m25 0h15"/><polygon points="0,59 5,52 0,45" style="fill:black;stroke-width:0"/><rect class="literal" x="15" y="35" width="54" height="25" rx="7"/><text class="text" x="25" y="52">WITH</text><rect class="literal" x="79" y="35" width="99" height="25" rx="7"/><text class="text" x="89" y="52">CLUSTERING</text><rect class="literal" x="188" y="35" width="63" height="25" rx="7"/><text class="text" x="198" y="52">ORDER</text><rect class="literal" x="261" y="35" width="35" height="25" rx="7"/><text class="text" x="271" y="52">BY</text><rect class="literal" x="306" y="35" width="25" height="25" rx="7"/><text class="text" x="316" y="52">(</text><rect class="literal" x="454" y="5" width="24" height="25" rx="7"/><text class="text" x="464" y="22">,</text><a xlink:href="../grammar_diagrams#index-column"><rect class="rule" x="361" y="35" width="106" height="25"/><text class="text" x="371" y="52">index_column</text></a><rect class="literal" x="497" y="35" width="45" height="25" rx="7"/><text class="text" x="507" y="52">ASC</text><rect class="literal" x="497" y="65" width="54" height="25" rx="7"/><text class="text" x="507" y="82">DESC</text><rect class="literal" x="601" y="35" width="25" height="25" rx="7"/><text class="text" x="611" y="52">)</text><polygon points="637,59 641,59 641,45 637,45" style="fill:black;stroke-width:0"/></svg>

#### index_column

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="188" height="65" viewbox="0 0 188 65"><path class="connector" d="M0 22h35m107 0h31m-153 0q5 0 5 5v20q0 5 5 5h5m118 0h5q5 0 5-5v-20q0-5 5-5m5 0h15"/><polygon points="0,29 5,22 0,15" style="fill:black;stroke-width:0"/><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="35" y="5" width="107" height="25"/><text class="text" x="45" y="22">column_name</text></a><a xlink:href="../grammar_diagrams#jsonb-attribute"><rect class="rule" x="35" y="35" width="118" height="25"/><text class="text" x="45" y="52">jsonb_attribute</text></a><polygon points="184,29 188,29 188,15 184,15" style="fill:black;stroke-width:0"/></svg>

#### jsonb_attribute

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="582" height="65" viewbox="0 0 582 65"><path class="connector" d="M0 37h15m107 0h50m-5 0q-5 0-5-5v-17q0-5 5-5h178q5 0 5 5v17q0 5-5 5m-139 0h10m124 0h40m-243 0q5 0 5 5v8q0 5 5 5h218q5 0 5-5v-8q0-5 5-5m5 0h10m43 0h10m124 0h15"/><polygon points="0,44 5,37 0,30" style="fill:black;stroke-width:0"/><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="15" y="20" width="107" height="25"/><text class="text" x="25" y="37">column_name</text></a><rect class="literal" x="172" y="20" width="34" height="25" rx="7"/><text class="text" x="182" y="37">-&gt;</text><rect class="literal" x="216" y="20" width="124" height="25" rx="7"/><text class="text" x="226" y="37">&apos;attribute_name&apos;</text><rect class="literal" x="390" y="20" width="43" height="25" rx="7"/><text class="text" x="400" y="37">-&gt;&gt;</text><rect class="literal" x="443" y="20" width="124" height="25" rx="7"/><text class="text" x="453" y="37">&apos;attribute_name&apos;</text><polygon points="578,44 582,44 582,30 578,30" style="fill:black;stroke-width:0"/></svg>

#### covering_columns

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="383" height="95" viewbox="0 0 383 95"><path class="connector" d="M0 52h35m86 0h20m-121 0q5 0 5 5v20q0 5 5 5h5m76 0h15q5 0 5-5v-20q0-5 5-5m5 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h47q5 0 5 5v20q0 5-5 5m-5 0h30m25 0h15"/><polygon points="0,59 5,52 0,45" style="fill:black;stroke-width:0"/><rect class="literal" x="35" y="35" width="86" height="25" rx="7"/><text class="text" x="45" y="52">COVERING</text><rect class="literal" x="35" y="65" width="76" height="25" rx="7"/><text class="text" x="45" y="82">INCLUDE</text><rect class="literal" x="151" y="35" width="25" height="25" rx="7"/><text class="text" x="161" y="52">(</text><rect class="literal" x="247" y="5" width="24" height="25" rx="7"/><text class="text" x="257" y="22">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="206" y="35" width="107" height="25"/><text class="text" x="216" y="52">column_name</text></a><rect class="literal" x="343" y="35" width="25" height="25" rx="7"/><text class="text" x="353" y="52">)</text><polygon points="379,59 383,59 383,45 379,45" style="fill:black;stroke-width:0"/></svg>

### Grammar

```
create_index ::= CREATE INDEX [ IF NOT EXISTS ] index_name
                     ON table_name ( partition_key_columns [ clustering_key_columns ] )  
                     [ clustering_key_column_ordering ] [ covering_columns ]

partition_key_columns ::= index_column | ( index_column [ , ... ] )

clustering_key_columns ::= index_column [ , ... ]

clustering_key_column_ordering ::= WITH CLUSTERING ORDER BY ( { index_column [ ASC | DESC ] } [ , ... ] )

index_column ::= column_name | jsonb_attribute

jsonb_attribute ::= column_name [ -> 'attribute_name' [ ... ] ] ->> 'attribute_name'

covering_columns ::= { COVERING | INCLUDE } ( column_name [ , ... ] )

```

Where

- `index_name`, `table_name`, and `column_name` are identifiers. `table_name` may be qualified with a keyspace name. `index_name` cannot be qualified with a keyspace name because an index must be created in the table's keyspace.

## Semantics

- An error is raised if transactions have not be enabled using the `WITH transactions = { 'enabled' : true }` clause on the table to be indexed. This is because secondary indexes internally use distributed transactions to ensure ACID guarantees in the updates to the secondary index and the associated primary key. More details [here](https://blog.yugabyte.com/yugabyte-db-1-1-new-feature-speeding-up-queries-with-secondary-indexes/).
- An error is raised if `index_name` already exists in the associated keyspace unless the `IF NOT EXISTS` option is used.
- Indexes do not support TTL. An error is raised if data is inserted with TTL into a table with indexes.


{{< note title="Note" >}}

When an index is created on an existing table, YugabyteDB will automatically backfill existing data into the index in an online manner (that is, while continuing to serve other concurrent writes and traffic). For more details on how this is done, see [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md).

{{< /note >}}

### PARTITION KEY

- Partition key is required and defines a split of the index into _partitions_.

### CLUSTERING KEY

- Clustering key is optional and defines an ordering for index rows within a partition.
- Default ordering is ascending (`ASC`) but can be set for each clustering column as ascending or descending using the `CLUSTERING ORDER BY` property.
- Any primary key column of the table not indexed explicitly in `index_columns` is added as a clustering column to the index implicitly. This is necessary so that the whole primary key of the table is indexed.

### INCLUDED COLUMNS

- Included columns are optional table columns whose values are copied into the index in addition to storing them in the table. When additional columns are included in the index, they can be used to respond to queries directly from the index without querying the table.

### UNIQUE INDEX

- A unique index disallows duplicate values from being inserted into the indexed columns. It can be used to ensure uniqueness of index column values.

## Examples

### Create a table to be indexed

'customer_id' is the partitioning column and 'order_date' is the clustering column.

```sql
ycqlsh:example> CREATE TABLE orders (customer_id INT,
                                    order_date TIMESTAMP,
                                    product JSONB,
                                    warehouse_id INT,
                                    amount DOUBLE,
                                    PRIMARY KEY ((customer_id), order_date))
               WITH transactions = { 'enabled' : true };
```

### Create an index for query by the `order_date` column

```sql
ycqlsh:example> CREATE INDEX orders_by_date ON orders (order_date) INCLUDE (amount);
```

### Create an index for query by the JSONB attribute `product->>'name'`

```sql
ycqlsh:example> CREATE INDEX product_name ON orders (product->>'name') INCLUDE (amount);
```

### Create an index for query by the `warehouse_id` column

```sql
ycqlsh:example> CREATE INDEX orders_by_warehouse ON orders (warehouse_id, order_date) INCLUDE (amount);
```

### Insert some data

```sql
ycqlsh:example> INSERT INTO orders (customer_id, order_date, product, warehouse_id, amount)
               VALUES (1001, '2018-01-10', '{ "name":"desk" }', 107, 100.30);
ycqlsh:example> INSERT INTO orders (customer_id, order_date, product, warehouse_id, amount)
               VALUES (1002, '2018-01-11', '{ "name":"chair" }', 102, 50.45);
ycqlsh:example> INSERT INTO orders (customer_id, order_date, product, warehouse_id, amount)
               VALUES (1001, '2018-04-09', '{ "name":"pen" }', 102, 20.25);
ycqlsh:example> INSERT INTO orders (customer_id, order_date, product, warehouse_id, amount)
               VALUES (1003, '2018-04-09', '{ "name":"pencil" }', 108, 200.80);
```

### Query by the partition column `customer_id` in the table

```sql
ycqlsh:example> SELECT SUM(amount) FROM orders WHERE customer_id = 1001 AND order_date >= '2018-01-01';
```

```
  sum(amount)
-------------
      120.55
```

### Query by the partition column `order_date` in the index `orders_by_date`

```sql
ycqlsh:example> SELECT SUM(amount) FROM orders WHERE order_date = '2018-04-09';
```

```
 sum(amount)
-------------
      221.05
```

### Query by the partition column `product->>'name'` in the index `product_name`

```sql
ycqlsh:example> SELECT SUM(amount) FROM orders WHERE product->>'name' = 'desk';
```

```
 sum(amount)
-------------
      100.30
```

### Query by the partition column `warehouse_id` column in the index `orders_by_warehouse`

```sql
ycqlsh:example> SELECT SUM(amount) FROM orders WHERE warehouse_id = 102 AND order_date >= '2018-01-01';
```

```
 sum(amount)
-------------
        70.7
```

### Create a table with a unique index

You can do this as shown below.

```sql
ycqlsh:example> CREATE TABLE emp (enum INT primary key,
                                 lastname VARCHAR,
                                 firstname VARCHAR,
                                 userid VARCHAR)
               WITH transactions = { 'enabled' : true };
ycqlsh:example> CREATE UNIQUE INDEX emp_by_userid ON emp (userid);
```

### Insert values into the table and verify no duplicate `userid` is inserted

```sql
ycqlsh:example> INSERT INTO emp (enum, lastname, firstname, userid)
               VALUES (1001, 'Smith', 'John', 'jsmith');
ycqlsh:example> INSERT INTO emp (enum, lastname, firstname, userid)
               VALUES (1002, 'Smith', 'Jason', 'jsmith');
InvalidRequest: Error from server: code=2200 [Invalid query] message="SQL error: Execution Error. Duplicate value disallowed by unique index emp_by_userid
INSERT INTO emp (enum, lastname, firstname, userid)
       ^^^^
VALUES (1002, 'Smith', 'Jason', 'jsmith');
 (error -300)"
ycqlsh:example> INSERT INTO emp (enum, lastname, firstname, userid)
               VALUES (1002, 'Smith', 'Jason', 'jasmith');
```

```sql
ycqlsh:example> SELECT * FROM emp;
```

```
 enum | lastname | firstname | userid
------+----------+-----------+---------
 1002 |    Smith |     Jason | jasmith
 1001 |    Smith |      John |  jsmith
```

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`DROP INDEX`](../ddl_drop_index)
