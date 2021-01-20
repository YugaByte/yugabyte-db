---
title: CREATE TABLE statement [YCQL]
headerTitle: CREATE TABLE
linkTitle: CREATE TABLE
description: Use the CREATE TABLE statement to create a new table in a keyspace.
menu:
  latest:
    parent: api-cassandra
    weight: 1240
aliases:
  - /latest/api/cassandra/ddl_create_table
  - /latest/api/ycql/ddl_create_table
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE TABLE` statement to create a new table in a keyspace. It defines the table name, column names and types, primary key, and table properties.

## Syntax

### Diagram

#### create_table

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="806" height="50" viewbox="0 0 806 50"><path class="connector" d="M0 22h5m67 0h10m58 0h30m32 0h10m45 0h10m64 0h20m-196 0q5 0 5 5v8q0 5 5 5h171q5 0 5-5v-8q0-5 5-5m5 0h10m91 0h10m25 0h10m103 0h10m25 0h30m116 0h20m-151 0q5 0 5 5v8q0 5 5 5h126q5 0 5-5v-8q0-5 5-5m5 0h5"/><rect class="literal" x="5" y="5" width="67" height="25" rx="7"/><text class="text" x="15" y="22">CREATE</text><rect class="literal" x="82" y="5" width="58" height="25" rx="7"/><text class="text" x="92" y="22">TABLE</text><rect class="literal" x="170" y="5" width="32" height="25" rx="7"/><text class="text" x="180" y="22">IF</text><rect class="literal" x="212" y="5" width="45" height="25" rx="7"/><text class="text" x="222" y="22">NOT</text><rect class="literal" x="267" y="5" width="64" height="25" rx="7"/><text class="text" x="277" y="22">EXISTS</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="361" y="5" width="91" height="25"/><text class="text" x="371" y="22">table_name</text></a><rect class="literal" x="462" y="5" width="25" height="25" rx="7"/><text class="text" x="472" y="22">(</text><a xlink:href="#table-schema"><rect class="rule" x="497" y="5" width="103" height="25"/><text class="text" x="507" y="22">table_schema</text></a><rect class="literal" x="610" y="5" width="25" height="25" rx="7"/><text class="text" x="620" y="22">)</text><a xlink:href="#table-properties"><rect class="rule" x="665" y="5" width="116" height="25"/><text class="text" x="675" y="22">table_properties</text></a></svg>

#### table_schema

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="742" height="185" viewbox="0 0 742 185"><path class="connector" d="M0 67h25m-5 0q-5 0-5-5v-35q0-5 5-5h339m24 0h339q5 0 5 5v35q0 5-5 5m-697 0h20m106 0h10m98 0h30m-5 0q-5 0-5-5v-17q0-5 5-5h176q5 0 5 5v17q0 5-5 5m-171 0h20m73 0h10m43 0h20m-161 0q5 0 5 5v20q0 5 5 5h5m63 0h68q5 0 5-5v-20q0-5 5-5m5 0h262m-687 0q5 0 5 5v80q0 5 5 5h5m73 0h10m43 0h10m25 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h46q5 0 5 5v20q0 5-5 5m-5 0h30m25 0h50m-5 0q-5 0-5-5v-17q0-5 5-5h150q5 0 5 5v17q0 5-5 5m-121 0h10m106 0h40m-215 0q5 0 5 5v8q0 5 5 5h190q5 0 5-5v-8q0-5 5-5m5 0h10m25 0h5q5 0 5-5v-80q0-5 5-5m5 0h25"/><rect class="literal" x="359" y="5" width="24" height="25" rx="7"/><text class="text" x="369" y="22">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="45" y="50" width="106" height="25"/><text class="text" x="55" y="67">column_name</text></a><a xlink:href="../grammar_diagrams#column-type"><rect class="rule" x="161" y="50" width="98" height="25"/><text class="text" x="171" y="67">column_type</text></a><rect class="literal" x="309" y="50" width="73" height="25" rx="7"/><text class="text" x="319" y="67">PRIMARY</text><rect class="literal" x="392" y="50" width="43" height="25" rx="7"/><text class="text" x="402" y="67">KEY</text><rect class="literal" x="309" y="80" width="63" height="25" rx="7"/><text class="text" x="319" y="97">STATIC</text><rect class="literal" x="45" y="140" width="73" height="25" rx="7"/><text class="text" x="55" y="157">PRIMARY</text><rect class="literal" x="128" y="140" width="43" height="25" rx="7"/><text class="text" x="138" y="157">KEY</text><rect class="literal" x="181" y="140" width="25" height="25" rx="7"/><text class="text" x="191" y="157">(</text><rect class="literal" x="216" y="140" width="25" height="25" rx="7"/><text class="text" x="226" y="157">(</text><rect class="literal" x="312" y="110" width="24" height="25" rx="7"/><text class="text" x="322" y="127">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="271" y="140" width="106" height="25"/><text class="text" x="281" y="157">column_name</text></a><rect class="literal" x="407" y="140" width="25" height="25" rx="7"/><text class="text" x="417" y="157">)</text><rect class="literal" x="482" y="140" width="24" height="25" rx="7"/><text class="text" x="492" y="157">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="516" y="140" width="106" height="25"/><text class="text" x="526" y="157">column_name</text></a><rect class="literal" x="672" y="140" width="25" height="25" rx="7"/><text class="text" x="682" y="157">)</text></svg>

#### table_properties

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="697" height="190" viewbox="0 0 697 190"><path class="connector" d="M0 52h5m53 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h274m46 0h274q5 0 5 5v20q0 5-5 5m-589 0h20m112 0h10m30 0h10m111 0h291m-574 55q0 5 5 5h5m98 0h10m62 0h10m35 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h97m24 0h98q5 0 5 5v20q0 5-5 5m-108 0h30m44 0h29m-83 25q0 5 5 5h5m53 0h5q5 0 5-5m-78-25q5 0 5 5v33q0 5 5 5h63q5 0 5-5v-33q0-5 5-5m5 0h30m25 0h5q5 0 5-5m-569-55q5 0 5 5v115q0 5 5 5h5m77 0h10m77 0h385q5 0 5-5v-115q0-5 5-5m5 0h25"/><rect class="literal" x="5" y="35" width="53" height="25" rx="7"/><text class="text" x="15" y="52">WITH</text><rect class="literal" x="357" y="5" width="46" height="25" rx="7"/><text class="text" x="367" y="22">AND</text><a xlink:href="../grammar_diagrams#property-name"><rect class="rule" x="108" y="35" width="112" height="25"/><text class="text" x="118" y="52">property_name</text></a><a xlink:href="../grammar_diagrams#="><rect class="rule" x="230" y="35" width="30" height="25"/><text class="text" x="240" y="52">=</text></a><a xlink:href="../grammar_diagrams#property-literal"><rect class="rule" x="270" y="35" width="111" height="25"/><text class="text" x="280" y="52">property_literal</text></a><rect class="literal" x="108" y="95" width="98" height="25" rx="7"/><text class="text" x="118" y="112">CLUSTERING</text><rect class="literal" x="216" y="95" width="62" height="25" rx="7"/><text class="text" x="226" y="112">ORDER</text><rect class="literal" x="288" y="95" width="35" height="25" rx="7"/><text class="text" x="298" y="112">BY</text><rect class="literal" x="333" y="95" width="25" height="25" rx="7"/><text class="text" x="343" y="112">(</text><rect class="literal" x="480" y="65" width="24" height="25" rx="7"/><text class="text" x="490" y="82">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="388" y="95" width="106" height="25"/><text class="text" x="398" y="112">column_name</text></a><rect class="literal" x="524" y="95" width="44" height="25" rx="7"/><text class="text" x="534" y="112">ASC</text><rect class="literal" x="524" y="125" width="53" height="25" rx="7"/><text class="text" x="534" y="142">DESC</text><rect class="literal" x="627" y="95" width="25" height="25" rx="7"/><text class="text" x="637" y="112">)</text><rect class="literal" x="108" y="160" width="77" height="25" rx="7"/><text class="text" x="118" y="177">COMPACT</text><rect class="literal" x="195" y="160" width="77" height="25" rx="7"/><text class="text" x="205" y="177">STORAGE</text></svg>

### Grammar

```
create_table ::= CREATE TABLE [ IF NOT EXISTS ] table_name
                     '(' table_element [ ',' table_element ...] ')'
                     [WITH table_properties];

table_element ::= table_column | table_constraints

table_column ::= column_name column_type [ column_constraint ...]

column_constraint ::= PRIMARY KEY | STATIC

table_constraints ::= PRIMARY KEY '(' partition_key_column_list clustering_key_column_list ')'

partition_key_column_list ::= '(' column_name [ ',' column_name ...] ')' | column_name

clustering_key_column_list ::= [ ',' column_name ...]

table_properties = [table_options]
                    [[AND] CLUSTERING ORDER BY '(' column_ordering_property [ ',' column_ordering_property ...] ')']
                    [[AND] COMPACT STORAGE]

table_options = property_name '=' property_literal [AND property_name '=' property_literal ...]

column_ordering_property ::= column_name [ ASC | DESC ]

```

Where

- `table_name`, `column_name`, and `property_name` are identifiers (`table_name` may be qualified with a keyspace name).
- `property_literal` is a literal of either [boolean](../type_bool), [text](../type_text), or [map](../type_collection) data type.

## Semantics

- An error is raised if `table_name` already exists in the associated keyspace unless the `IF NOT EXISTS` option is used.

### PRIMARY KEY

- Primary key must be defined in either `column_constraint` or `table_constraint` but not in both of them.
- Each row in a table is uniquely identified by its primary key.
- Primary key columns are either _partitioning_ columns or _clustering_ columns (described below).
- If primary key is set as a column constraint, then that column is the partition column and there are no clustering columns.
- If primary key is set as a table constraint then:
  - The partition columns are given by the first entry in the primary key list: the nested column list (if given), otherwise the first column.
  - The clustering columns are the rest of the columns in the primary key list (if any).

#### PARTITION KEY

- Partition key is required and defines a split of rows into _partitions_.
- Rows that share the same partition key form a partition and will be colocated on the same replica node.

#### CLUSTERING KEY

- Clustering key is optional and defines an ordering for rows within a partition.
- Default ordering is ascending (`ASC`) but can be set for each clustering column as ascending or descending using the `CLUSTERING ORDER BY` table property.

### STATIC COLUMNS

- Columns declared as `STATIC` will share the same value for all rows within a partition (i.e. rows having the same partition key).
- Columns in the primary key cannot be static.
- A table without clustering columns cannot have static columns (without clustering columns the primary key and the partition key are identical so static columns would be the same as regular columns).

### *table_properties*

- The `CLUSTERING ORDER BY` property can be used to set the ordering for each clustering column individually (default is `ASC`).
- The `default_time_to_live` property sets the default expiration time (TTL) in seconds for a table. The expiration time can be overridden by setting TTL for individual rows. The default value is `0` and means rows do not expire.
- The `transactions` property specifies if distributed transactions are enabled in the table. To enable distributed transactions, use `transactions = { 'enabled' : true }`.
- Use the `AND` operator to use multiple table properties.
- The other YCQL table properties are allowed in the syntax but are currently ignored internally (have no effect).
- The `TABLETS = <num>` property specifies the number of tablets to be used for the specified YCQL table. Setting this property overrides the value from the [`--yb_num_shards_per_tserver`](../../../reference/configuration/yb-tserver/#yb-num-shards-per-tserver) option. For an example, see [Create a table specifying the number of tablets](#create-a-table-specifying-the-number-of-tablets).
- `COMPACT STORAGE` is only for syntax compatibility with Cassandra. It doesn't affect the underlying storage.

## Examples

### Use column constraint to define primary key

 'user_id' is the partitioning column and there are no clustering columns.

```sql
ycqlsh:example> CREATE TABLE users(user_id INT PRIMARY KEY, full_name TEXT);
```

### Use table constraint to define primary key

'supplier_id' and 'device_id' are the partitioning columns and 'model_year' is the clustering column.

```sql
ycqlsh:example> CREATE TABLE devices(supplier_id INT,
                                    device_id INT,
                                    model_year INT,
                                    device_name TEXT,
                                    PRIMARY KEY((supplier_id, device_id), model_year));
```

### Use column constraint to define a static column

You can do this as shown below.

```sql
ycqlsh:example> CREATE TABLE items(supplier_id INT,
                                  item_id INT,
                                  supplier_name TEXT STATIC,
                                  item_name TEXT,
                                  PRIMARY KEY((supplier_id), item_id));
```

```sql
ycqlsh:example> INSERT INTO items(supplier_id, item_id, supplier_name, item_name)
               VALUES (1, 1, 'Unknown', 'Wrought Anvil');
```

```sql
ycqlsh:example> INSERT INTO items(supplier_id, item_id, supplier_name, item_name)
               VALUES (1, 2, 'Acme Corporation', 'Giant Rubber Band');
```

```sql
ycqlsh:example> SELECT * FROM devices;
```

```
 supplier_id | item_id | supplier_name    | item_name
-------------+---------+------------------+-------------------
           1 |       1 | Acme Corporation |     Wrought Anvil
           1 |       2 | Acme Corporation | Giant Rubber Band
```

### Use table property to define the order (ascending or descending) for clustering columns

Timestamp column 'ts' will be stored in descending order (latest values first).

```sql
ycqlsh:example> CREATE TABLE user_actions(user_id INT,
                                         ts TIMESTAMP,
                                         action TEXT,
                                         PRIMARY KEY((user_id), ts))
                                         WITH CLUSTERING ORDER BY (ts DESC);
```

```sql
ycqlsh:example> INSERT INTO user_actions(user_id, ts, action) VALUES (1, '2000-12-2 12:30:15', 'log in');
```

```sql
ycqlsh:example> INSERT INTO user_actions(user_id, ts, action) VALUES (1, '2000-12-2 12:30:25', 'change password');
```

```sql
ycqlsh:example> INSERT INTO user_actions(user_id, ts, action) VALUES (1, '2000-12-2 12:30:35', 'log out');
```

```sql
ycqlsh:example> SELECT * FROM user_actions;
```

```
 user_id | ts                              | action
---------+---------------------------------+-----------------
       1 | 2000-12-02 19:30:35.000000+0000 |         log out
       1 | 2000-12-02 19:30:25.000000+0000 | change password
       1 | 2000-12-02 19:30:15.000000+0000 |          log in
```

### Use table property to define the default expiration time for rows

You can do this as shown below.

```sql
ycqlsh:example> CREATE TABLE sensor_data(sensor_id INT,
                                        ts TIMESTAMP,
                                        value DOUBLE,
                                        PRIMARY KEY((sensor_id), ts))
                                        WITH default_time_to_live = 5;
```

First insert at time T (row expires at T + 5).

```sql
ycqlsh:example> INSERT INTO sensor_data(sensor_id, ts, value) VALUES (1, '2017-10-1 11:22:31', 3.1);
```

Second insert 3 seconds later (row expires at T + 8).

```sql
ycqlsh:example> INSERT INTO sensor_data(sensor_id, ts, value) VALUES (2, '2017-10-1 11:22:34', 3.4);
```

First select 3 seconds later (at time T + 6).

```sql
ycqlsh:example> SELECT * FROM sensor_data;
```

```
 sensor_id | ts                              | value
-----------+---------------------------------+-------
         2 | 2017-10-01 18:22:34.000000+0000 |   3.4
```

Second select 3 seconds later (at time T + 9).

```sql
ycqlsh:example> SELECT * FROM sensor_data;
```

```
 sensor_id | ts | value
-----------+----+-------

```

### Create a table specifying the number of tablets

You can use the `CREATE TABLE` statement with the `WITH tablets = <num>` clause to specify the number of tablets for a table. This is useful to scale the table up or down based on requirements. For example, for smaller static tables, it may be wasteful to have a large number of shards (tablets). In that case, you can use this to reduce the number of tablets created for the table. Similarly, for a very large table, you can use this statement to presplit the table into a large number of shards to get improved performance.

Note that YugabyteDB, by default, presplits a table in `yb_num_shards_per_tserver * num_of_tserver` shards. This clause can be used to override that setting on per-table basis.

```sql
ycqlsh:example> CREATE TABLE tracking (id int PRIMARY KEY) WITH tablets = 10;
```

If you create an index for these tables, you can also specify the number of tablets for the index.

You can also use `AND` to add other table properties, like in this example.

```sql
ycqlsh:example> CREATE TABLE tracking (id int PRIMARY KEY) WITH tablets = 10 AND transactions = { 'enabled' : true };
```

## See also

- [`ALTER TABLE`](../ddl_alter_table)
- [`DELETE`](../dml_delete)
- [`DROP TABLE`](../ddl_drop_table)
- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
- [`UPDATE`](../dml_update)
