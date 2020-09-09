---
title: TEXT data type [YCQL]
headerTitle: TEXT type
linkTitle: TEXT
description: Use the TEXT data type to specify data of a string of Unicode characters.
menu:
  latest:
    parent: api-cassandra
    weight: 1440
aliases:
  - /latest/api/cassandra/type_text
  - /latest/api/ycql/type_text
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `TEXT` data type to specify data of a string of Unicode characters.

## Syntax

```
type_specification ::= TEXT | VARCHAR

text_literal ::= "'" [ letter ...] "'"
```

Where 

- `TEXT` and `VARCHAR` are aliases.
- `letter` is any character except for single quote (`[^']`)

## Semantics

- Columns of type `TEXT` or `VARCHAR` can be part of the `PRIMARY KEY`.
- Implicitly, value of type `TEXT` data type are neither convertible nor comparable to non-text data types.
- The length of `TEXT` string is virtually unlimited.

## Examples

```sql
ycqlsh:example> CREATE TABLE users(user_name TEXT PRIMARY KEY, full_name VARCHAR);
```

```sql
ycqlsh:example> INSERT INTO users(user_name, full_name) VALUES ('jane', 'Jane Doe');
```

```sql
ycqlsh:example> INSERT INTO users(user_name, full_name) VALUES ('john', 'John Doe');
```

```sql
ycqlsh:example> UPDATE users set full_name = 'Jane Poe' WHERE user_name = 'jane';
```

```sql
ycqlsh:example> SELECT * FROM users;
```

```
 user_name | full_name
-----------+-----------
      jane |  Jane Poe
      john |  John Doe
```

## See also

- [Data types](..#data-types)
