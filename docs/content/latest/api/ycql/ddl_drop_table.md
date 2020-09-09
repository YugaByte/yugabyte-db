---
title: DROP TABLE statement [YCQL]
headerTitle: DROP TABLE
linkTitle: DROP TABLE
description: Use the DROP TABLE statement to remove a table and all of its data from the database.
menu:
  latest:
    parent: api-cassandra
    weight: 1270
aliases:
  - /latest/api/cassandra/ddl_drop_table
  - /latest/api/ycql/ddl_drop_table
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP TABLE` statement to remove a table and all of its data from the database.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="388" height="50" viewbox="0 0 388 50"><path class="connector" d="M0 22h5m53 0h10m58 0h30m32 0h10m64 0h20m-141 0q5 0 5 5v8q0 5 5 5h116q5 0 5-5v-8q0-5 5-5m5 0h10m91 0h5"/><rect class="literal" x="5" y="5" width="53" height="25" rx="7"/><text class="text" x="15" y="22">DROP</text><rect class="literal" x="68" y="5" width="58" height="25" rx="7"/><text class="text" x="78" y="22">TABLE</text><rect class="literal" x="156" y="5" width="32" height="25" rx="7"/><text class="text" x="166" y="22">IF</text><rect class="literal" x="198" y="5" width="64" height="25" rx="7"/><text class="text" x="208" y="22">EXISTS</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="292" y="5" width="91" height="25"/><text class="text" x="302" y="22">table_name</text></a></svg>

### Grammar

```
drop_table ::= DROP TABLE [ IF EXISTS ] table_name;
```

Where

- `table_name` is an identifier (possibly qualified with a keyspace name).

## Semantics

- An error is raised if the specified `table_name` does not exist unless `IF EXISTS` option is present.
- Associated objects to `table_name` such as prepared statements will be eventually invalidated after the drop statement is completed.

## Examples

```sql
ycqlsh:example> CREATE TABLE users(id INT PRIMARY KEY, name TEXT);
```

```sql
ycqlsh:example> DROP TABLE users;
```

## See also

- [`ALTER TABLE`](../ddl_alter_table)
- [`CREATE TABLE`](../ddl_create_table)
- [`DELETE`](../dml_delete)
- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
- [`UPDATE`](../dml_update)
