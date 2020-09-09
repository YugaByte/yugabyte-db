---
title: FROZEN data type [YCQL]
headerTitle: FROZEN data type
linkTitle: FROZEN
description: Use the FROZEN data type to specify columns of binary strings that result from serializing collections, tuples, or user-defined types.
menu:
  latest:
    parent: api-cassandra
    weight: 1401
aliases:
  - /latest/api/cassandra/type_frozen
  - /latest/api/ycql/type_frozen
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `FROZEN` data type to specify columns of binary strings that result from serializing collections, tuples, or user-defined types.

## Syntax

```
type_specification ::= FROZEN<type>
```

Where

- `type` is a well-formed YCQL data type (additional restrictions for `type` are covered in the Semantics section below).

## Semantics

- Columns of type `FROZEN` can be part of the `PRIMARY KEY`.
- Type parameters of `FROZEN` type must be either [collection types](../type_collection) (`LIST`, `MAP`, or `SET`) or [user-defined types](../ddl_create_type).
- `FROZEN` types can be parameters of collection types.
- For any valid frozen type parameter `type`, values of `type` are convertible into `FROZEN<type>`.

## Examples

```sql
ycqlsh:example> CREATE TABLE directory(file FROZEN<LIST<TEXT>> PRIMARY KEY, value BLOB);
```

```sql
ycqlsh:example> INSERT INTO directory(file, value) VALUES([ 'home', 'documents', 'homework.doc' ], 0x);
```

```sql
ycqlsh:example> INSERT INTO directory(file, value) VALUES([ 'home', 'downloads', 'textbook.pdf' ], 0x12ab21ef);
```

```sql
ycqlsh:example> UPDATE directory SET value = 0xab00ff WHERE file = [ 'home', 'documents', 'homework.doc' ];
```

```sql
ycqlsh:example> SELECT * FROM directory;
```

```
 file                                  | value
---------------------------------------+------------
 ['home', 'downloads', 'textbook.pdf'] | 0x12ab21ef
 ['home', 'documents', 'homework.doc'] |   0xab00ff
 ```

## See also

- [Data Types](..#data-types)
