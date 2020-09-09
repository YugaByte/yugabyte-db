---
title: Subscripted expressions [YCQL]
headerTitle: Subscripted expressions
linkTitle: Subscripted expressions
summary: Subscripted columns
description: Use subscripted expressions to access elements in a multi-element value, such as a map collection by using the [] operator.
block_indexing: true
menu:
  v2.1:
    parent: api-cassandra
    weight: 1340
isTocNested: true
showAsideToc: true
---

Use subscripted expressions to access elements in a multi-element value, such as a map collection by using operator `[]`. Subscripted column expressions can be used when writing the same way as a [column expression](../expr_simple##Column). For example, if `ids` refers to a column of type `LIST`, `ids[7]` refers to the third element of the list `ids`, which can be set in an [UPDATE](../dml_update) statement.

<li>Subscripted expression can only be applied to columns of type `LIST`, `MAP`, or user-defined data types.</li>
<li>Subscripting a `LIST` value with a non-positive index will yield NULL.</li>
<li>Subscripting a `MAP` value with a non-existing key will yield NULL. Otherwise, it returns the element value that is associated with the given key.</li>
<li>Apache Cassandra does not allow subscripted expression in the select list of the SELECT statement.</li>

## Examples

```sql
ycqlsh:yugaspace> CREATE TABLE t(id INT PRIMARY KEY,yugamap MAP<TEXT, TEXT>);
```

```sql
ycqlsh:yugaspace> UPDATE yugatab SET map_value['key_value'] = 'yuga_string' WHERE id = 7;
```

## See also

- [All Expressions](..##expressions)
