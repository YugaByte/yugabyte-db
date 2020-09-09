---
title: USE statement [YCQL]
headerTitle: USE
linkTitle: USE
description: Use the USE statement to specify a default keyspace for the current client session.
menu:
  latest:
    parent: api-cassandra
    weight: 1290
aliases:
  - /latest/api/cassandra/ddl_use
  - /latest/api/ycql/ddl_use
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `USE` statement to specify a default keyspace for the current client session. When a database object (such as [table](../ddl_create_table) or [type](../ddl_create_type)) name does not identify a keyspace, this default keyspace is used.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="181" height="35" viewbox="0 0 181 35"><path class="connector" d="M0 22h5m45 0h10m116 0h5"/><rect class="literal" x="5" y="5" width="45" height="25" rx="7"/><text class="text" x="15" y="22">USE</text><a xlink:href="../grammar_diagrams#keyspace-name"><rect class="rule" x="60" y="5" width="116" height="25"/><text class="text" x="70" y="22">keyspace_name</text></a></svg>

### Grammar

```
use_keyspace ::= USE keyspace_name;
```

Where

- `keyspace_name` must be an identifier that cannot be any reserved keyword and cannot contains whitespaces, or it has to be double-quoted.

## Semantics

- If the specified keyspace does not exist, an error is raised.
- Any unqualified table or type name will use the current default keyspace (or raise an error if no keyspace is set).

## Examples

### Create and use keyspaces

```sql
ycqlsh> CREATE KEYSPACE example;
```

```sql
ycqlsh> CREATE KEYSPACE other_keyspace;
```

```sql
ycqlsh> USE example;
```

### Create a table in the current keyspace

``` sql
ycqlsh:example> CREATE TABLE test(id INT PRIMARY KEY);
ycqlsh:example> INSERT INTO test(id) VALUES (1);
ycqlsh:example> SELECT * FROM test;
```

```
 id
----
  1
```

### Create a table in another keyspace

``` sql
ycqlsh:example> CREATE TABLE other_keyspace.test(id INT PRIMARY KEY);
ycqlsh:example> INSERT INTO other_keyspace.test(id) VALUES (2);
ycqlsh:example> SELECT * FROM other_keyspace.test;
```

```
 id
----
  2
```

## See also

- [`ALTER KEYSPACE`](../ddl_alter_keyspace)
- [`CREATE KEYSPACE`](../ddl_create_keyspace)
- [`DROP KEYSPACE`](../ddl_drop_keyspace)
