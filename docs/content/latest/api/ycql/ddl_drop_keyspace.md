---
title: DROP KEYSPACE statement [YCQL]
headerTitle: DROP KEYSPACE
linkTitle: DROP KEYSPACE
description: Use the DROP KEYSPACE statement to remove a keyspace from the system.
menu:
  latest:
    parent: api-cassandra
    weight: 1260
aliases:
  - /latest/api/cassandra/ddl_drop_keyspace
  - /latest/api/ycql/ddl_drop_keyspace
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP KEYSPACE` statement to remove a keyspace from the system.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="477" height="65" viewbox="0 0 477 65"><path class="connector" d="M0 22h5m53 0h30m82 0h20m-117 0q5 0 5 5v20q0 5 5 5h5m71 0h16q5 0 5-5v-20q0-5 5-5m5 0h30m32 0h10m64 0h20m-141 0q5 0 5 5v8q0 5 5 5h116q5 0 5-5v-8q0-5 5-5m5 0h10m116 0h5"/><rect class="literal" x="5" y="5" width="53" height="25" rx="7"/><text class="text" x="15" y="22">DROP</text><rect class="literal" x="88" y="5" width="82" height="25" rx="7"/><text class="text" x="98" y="22">KEYSPACE</text><rect class="literal" x="88" y="35" width="71" height="25" rx="7"/><text class="text" x="98" y="52">SCHEMA</text><rect class="literal" x="220" y="5" width="32" height="25" rx="7"/><text class="text" x="230" y="22">IF</text><rect class="literal" x="262" y="5" width="64" height="25" rx="7"/><text class="text" x="272" y="22">EXISTS</text><a xlink:href="../grammar_diagrams#keyspace-name"><rect class="rule" x="356" y="5" width="116" height="25"/><text class="text" x="366" y="22">keyspace_name</text></a></svg>

### Grammar

```
drop_keyspace ::= DROP { KEYSPACE | SCHEMA } [ IF EXISTS ] keyspace_name;
```

Where

- `keyspace_name` is an identifier.

## Semantics

- An error is raised if the specified `keyspace_name` does not exist unless `IF EXISTS` option is present.
- An error is raised if the specified keyspace is non-empty (contains tables or types).

## Examples

You can do this as shown below.

```sql
ycqlsh> CREATE KEYSPACE example;
```

```sql
ycqlsh> DROP KEYSPACE example;
```

```sql
ycqlsh> DROP KEYSPACE IF EXISTS example;
```

## See also

- [`ALTER KEYSPACE`](../ddl_alter_keyspace)
- [`CREATE KEYSPACE`](../ddl_create_keyspace)
- [`USE`](../ddl_use)
