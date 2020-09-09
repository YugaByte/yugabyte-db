---
title: Integer and counter data types [YCQL]
headerTitle: Integer and counter data types
linkTitle: Integer and counter
description: There are several different data types for integers of different value ranges. Integers can be set, inserted, incremented, and decremented.
menu:
  latest:
    parent: api-cassandra
    weight: 1420
aliases:
  - /latest/api/cassandra/type_int
  - /latest/api/ycql/type_int
isTocNested: true
showAsideToc: true
---

## Synopsis

There are several different data types for integers of different value ranges. Integers can be set, inserted, incremented, and decremented while `COUNTER` can only be incremented or decremented. We've extend Apache Cassandra to support increment and decrement operators for integer data types.

Datatype | Min | Max |
---------|-----|-----|
`TINYINT` | -128 | 127 |
`SMALLINT` | -32,768 | 32,767 |
`INT` or `INTEGER` | -2,147,483,648 | 2,147,483,647 |
`BIGINT` | –9,223,372,036,854,775,808 | 9,223,372,036,854,775,807 |
`COUNTER` | –9,223,372,036,854,775,808 | 9,223,372,036,854,775,807 |
`VARINT` | unbounded | unbounded |

## Syntax

The following keywords are used to specify a column of type integer for different constraints including its value ranges.

```
type_specification ::= TINYINT | SMALLINT | INT | INTEGER | BIGINT | VARINT | COUNTER

integer_literal ::= [ + | - ] digit [ { digit | , } ... ]
```

## Semantics

- Columns of type `TINYINT`, `SMALLINT`, `INT`, `INTEGER`, `BIGINT` or `VARINT` can be part of the `PRIMARY KEY`.
- Values of different integer data types are comparable and convertible to one another.
- Values of integer data types are convertible but not comparable to floating point number.
- Values of floating point data types are not convertible to integers.

### Counter data type

`COUNTER` is an alias of `BIGINT` but has additional constraints.

- Columns of type `COUNTER` cannot be part of the`PRIMARY KEY`.
- If a column is of type `COUNTER`, all non-primary-key columns must also be of type `COUNTER`.
- Column of type `COUNTER` cannot be set or inserted. They must be incremented or decremented.
- If a column of type `COUNTER` is NULL, its value is replaced with zero when incrementing or decrementing.

## Examples

### Using integer data types

```sql
example> CREATE TABLE items(id INT PRIMARY KEY, item_count BIGINT);
```

```sql
example> INSERT INTO items(id, item_count) VALUES(1, 1);
```

```sql
example> INSERT INTO items(id, item_count) VALUES(2, 2);
```

```sql
example> UPDATE items SET item_count = 5 WHERE id = 1;
```

```sql
example> UPDATE items SET item_count = item_count + 1 WHERE id = 2;
```

```sql
example> SELECT * FROM items;
```

```
 id | item_count
----+------------
  2 |          3
  1 |          5
```

### Using `COUNTER` data type

```sql
example> CREATE TABLE item_counters(id INT PRIMARY KEY, item_counter COUNTER);
```

For counter type, null values are treated as 0.

```sql
example> UPDATE item_counters SET item_counter = item_counter + 1 WHERE id = 1;
```

```sql
example> SELECT * FROM item_counters;
```

```
 id | item_counter
----+--------------
  1 |            1
```

## See also

- [Data types](..#data-types)
