---
title: BLOB data type [YCQL]
headerTitle: BLOB data type
linkTitle: BLOB
description: Use the BLOB data type to represent arbitrary binary data of variable length.
menu:
  latest:
    parent: api-cassandra
    weight: 1370
aliases:
  - /latest/api/cassandra/type_blob
  - /latest/api/ycql/type_blob
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `BLOB` data type to represent arbitrary binary data of variable length.

## Syntax

```
type_specification ::= BLOB

blob_literal ::= "0x" [ hex_digit hex_digit ...]
```

Where

- `hex_digit` is a hexadecimal digit (`[0-9a-fA-F]`).

## Semantics

- Columns of type `BLOB` can be part of the `PRIMARY KEY`.
- Implicitly, `BLOB` data type is neither convertible nor comparable with other data types.
- Two series of builtin-functions `BlobAs<Type>` and `<Type>AsBlob` are provided for conversion between `BLOB` and other data types.
- `BLOB` size is virtually unlimited.

## Examples

```sql
ycqlsh:example> CREATE TABLE messages(id INT PRIMARY KEY, content BLOB);
```

```sql
ycqlsh:example> INSERT INTO messages (id, content) VALUES (1, 0xab00ff);
```

```sql
ycqlsh:example> INSERT INTO messages (id, content) VALUES (2, 0x);
```

```sql
ycqlsh:example> UPDATE messages SET content = 0x0f0f WHERE id = 2;
```

```sql
ycqlsh:example> SELECT * FROM messages;
```

```
 id | content
----+----------
  2 |   0x0f0f
  1 | 0xab00ff
```

## See also

- [Data types](..#data-types)
