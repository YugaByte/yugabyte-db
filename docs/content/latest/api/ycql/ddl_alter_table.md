---
title: ALTER TABLE statement [YCQL]
headerTitle: ALTER TABLE
linkTitle: ALTER TABLE
description: Use the ALTER TABLE statement to change the schema or definition of an existing table.
menu:
  latest:
    parent: api-cassandra
    weight: 1220
aliases:
  - /latest/api/cassandra/ddl_alter_table
  - /latest/api/ycql/ddl_alter_table
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER TABLE` statement to change the schema or definition of an existing table.
It allows adding, dropping, or renaming a column as well as updating a table property.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="716" height="260" viewbox="0 0 716 260"><path class="connector" d="M0 67h5m58 0h10m58 0h10m91 0h30m-5 0q-5 0-5-5v-47q0-5 5-5h439q5 0 5 5v47q0 5-5 5m-434 0h20m46 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h100m24 0h100q5 0 5 5v20q0 5-5 5m-113 0h10m98 0h119m-419 55q0 5 5 5h5m53 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h46q5 0 5 5v20q0 5-5 5m-5 0h205q5 0 5-5m-409 60q0 5 5 5h5m71 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h127m24 0h127q5 0 5 5v20q0 5-5 5m-167 0h10m36 0h10m106 0h25q5 0 5-5m-414-115q5 0 5 5v170q0 5 5 5h5m53 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h118m46 0h119q5 0 5 5v20q0 5-5 5m-166 0h10m30 0h10m111 0h38q5 0 5-5v-170q0-5 5-5m5 0h25"/><rect class="literal" x="5" y="50" width="58" height="25" rx="7"/><text class="text" x="15" y="67">ALTER</text><rect class="literal" x="73" y="50" width="58" height="25" rx="7"/><text class="text" x="83" y="67">TABLE</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="141" y="50" width="91" height="25"/><text class="text" x="151" y="67">table_name</text></a><rect class="literal" x="282" y="50" width="46" height="25" rx="7"/><text class="text" x="292" y="67">ADD</text><rect class="literal" x="453" y="20" width="24" height="25" rx="7"/><text class="text" x="463" y="37">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="358" y="50" width="106" height="25"/><text class="text" x="368" y="67">column_name</text></a><a xlink:href="../grammar_diagrams#column-type"><rect class="rule" x="474" y="50" width="98" height="25"/><text class="text" x="484" y="67">column_type</text></a><rect class="literal" x="282" y="110" width="53" height="25" rx="7"/><text class="text" x="292" y="127">DROP</text><rect class="literal" x="406" y="80" width="24" height="25" rx="7"/><text class="text" x="416" y="97">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="365" y="110" width="106" height="25"/><text class="text" x="375" y="127">column_name</text></a><rect class="literal" x="282" y="170" width="71" height="25" rx="7"/><text class="text" x="292" y="187">RENAME</text><rect class="literal" x="505" y="140" width="24" height="25" rx="7"/><text class="text" x="515" y="157">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="383" y="170" width="106" height="25"/><text class="text" x="393" y="187">column_name</text></a><rect class="literal" x="499" y="170" width="36" height="25" rx="7"/><text class="text" x="509" y="187">TO</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="545" y="170" width="106" height="25"/><text class="text" x="555" y="187">column_name</text></a><rect class="literal" x="282" y="230" width="53" height="25" rx="7"/><text class="text" x="292" y="247">WITH</text><rect class="literal" x="478" y="200" width="46" height="25" rx="7"/><text class="text" x="488" y="217">AND</text><a xlink:href="../grammar_diagrams#property-name"><rect class="rule" x="365" y="230" width="112" height="25"/><text class="text" x="375" y="247">property_name</text></a><rect class="literal" x="487" y="230" width="30" height="25" rx="7"/><text class="text" x="497" y="247">=</text><a xlink:href="../grammar_diagrams#property-literal"><rect class="rule" x="527" y="230" width="111" height="25"/><text class="text" x="537" y="247">property_literal</text></a></svg>

### Grammar

```
alter_table ::= ALTER TABLE table_name alter_operator [ alter_operator ...]

alter_operator ::= add_op | drop_op | rename_op | property_op

add_op ::= ADD column_name column_type [ ',' column_name column_type ...]

drop_op ::= DROP column_name [ ',' column_name ...]

rename_op ::= RENAME column_name TO column_name [ ',' column_name TO column_name ...]

property_op ::= WITH property_name '=' property_literal [ AND property_name '=' property_literal ...]
```

Where

- `table_name`, `column_name`, and `property_name` are identifiers (`table_name` may be qualified with a keyspace name).
- `property_literal` is a literal of either [boolean](../type_bool), [text](../type_text), or [map](../type_collection) data type.

## Semantics

- An error is raised if `table_name` does not exist in the associated keyspace.
- Columns that are part of `PRIMARY KEY` cannot be be altered.
- When adding a column, its value for all existing rows in the table defaults to `null`.
- After dropping a column, all values currently stored for that column in the table are discarded (if any).

## Examples

### Add a column to a table

```sql
ycqlsh:example> CREATE TABLE employees (id INT, name TEXT, salary FLOAT, PRIMARY KEY((id), name));
```

```sql
ycqlsh:example> ALTER TABLE employees ADD title TEXT;
```

```sql
ycqlsh:example> DESCRIBE TABLE employees;
```

Following result would be shown.

```
CREATE TABLE example.employees (
    id int,
    name text,
    salary float,
    title text,
    PRIMARY KEY (id, name)
) WITH CLUSTERING ORDER BY (name ASC);
```

### Remove a column from a table

```sql
ycqlsh:example> ALTER TABLE employees DROP salary;
```

```sql
ycqlsh:example> DESCRIBE TABLE employees;
```

Following result would be shown.

```
CREATE TABLE example.employees (
    id int,
    name text,
    title text,
    PRIMARY KEY (id, name)
) WITH CLUSTERING ORDER BY (name ASC);
```

### Rename a column in a table

```sql
ycqlsh:example> ALTER TABLE employees RENAME title TO job_title;
```

```sql
ycqlsh:example> DESCRIBE TABLE employees;
```

Following result would be shown.

```
CREATE TABLE example.employees (
    id int,
    name text,
    job_title text,
    PRIMARY KEY (id, name)
) WITH CLUSTERING ORDER BY (name ASC);
```

### Update a table property

You can do this as shown below.

```sql
ycqlsh:example> ALTER TABLE employees WITH default_time_to_live = 5;
```

```sql
ycqlsh:example> DESCRIBE TABLE employees;
```

Following result would be shown.

```
CREATE TABLE example.employees (
    id int,
    name text,
    job_title text,
    PRIMARY KEY (id, name)
) WITH CLUSTERING ORDER BY (name ASC)
    AND default_time_to_live = 5;
```

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`DELETE`](../dml_delete)
- [`DROP TABLE`](../ddl_drop_table)
- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
- [`UPDATE`](../dml_update)
