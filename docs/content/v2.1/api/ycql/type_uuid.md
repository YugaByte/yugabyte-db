---
title: UUID and TIMEUUID data types [YCQL]
headerTitle: UUID and TIMEUUID
linkTitle: UUID and TIMEUUID
summary: UUID types
description: Use the UUID data type to specify columns for data of universally unique ids. TIMEUUID is a universal unique identifier variant that includes time information.
block_indexing: true
menu:
  v2.1:
    parent: api-cassandra
    weight: 1460
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `UUID` data type to specify columns for data of universally unique IDs. `TIMEUUID` is a universal unique identifier variant that includes time information.

Data type | Description |
----------|-----|
`UUID` | [UUID (all versions)](https://tools.ietf.org/html/rfc4122) |
`TIMEUUID` | [UUID (version 1)](https://tools.ietf.org/html/rfc4122#section-4.2.2) |

## Syntax

```
type_specification ::= { UUID | TIMEUUID }
uuid_literal ::= 4hex_block 4hex_block '-' 4hex_block '-' 4hex_block '-' 4hex_block '-' 4hex_block 4hex_block 4hex_block
4hex_block ::= hex_digit hex_digit hex_digit hex_digit
```

Where

- `hex_digit` is a hexadecimal digit (`[0-9a-fA-F]`).

## Semantics

- Columns of type `UUID` or `TIMEUUID` can be part of the `PRIMARY KEY`.
- Implicitly, values of type `UUID` and `TIMEUUID` data types are neither convertible nor comparable to other data types.
- `TIMEUUID`s are version 1 UUIDs: they include the date and time of their generation and a spatially unique node identifier.
- Comparison of `TIMEUUID` values first compares the time component and then (if time is equal) the node identifier.

## Examples

```sql
ycqlsh:example> CREATE TABLE devices(id UUID PRIMARY KEY, ordered_id TIMEUUID);
```

```sql
ycqlsh:example> INSERT INTO devices (id, ordered_id) 
               VALUES (123e4567-e89b-12d3-a456-426655440000, 123e4567-e89b-12d3-a456-426655440000);
```

```sql
ycqlsh:example> INSERT INTO devices (id, ordered_id) 
               VALUES (123e4567-e89b-42d3-a456-426655440000, 123e4567-e89b-12d3-a456-426655440000);
```

```sql
ycqlsh:example> UPDATE devices SET ordered_id = 00000000-0000-1000-0000-000000000000
               WHERE id = 123e4567-e89b-42d3-a456-426655440000; 
```

```sql
ycqlsh:example> SELECT * FROM devices;
```

```
id                                   | ordered_id
--------------------------------------+--------------------------------------
 123e4567-e89b-12d3-a456-426655440000 | 123e4567-e89b-12d3-a456-426655440000
 123e4567-e89b-42d3-a456-426655440000 | 00000000-0000-1000-0000-000000000000
```

## See also

- [`Date and time Functions`](../function_datetime)
- [Data types](..#data-types)
