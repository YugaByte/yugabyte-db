---
title: lastval() function [YSQL]
headerTitle: lastval()
linkTitle: lastval()
description: Returns the value returned from the last call to nextval(), for any sequence, in the current session.
menu:
  latest:
    identifier: api-ysql-exprs-lastval
    parent: api-ysql-exprs
aliases:
  - /latest/api/ysql/exprs/func_lastval
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `lastval()` function to return the value returned from the last call to `nextval()`, for any sequence, in the current session.

## Semantics

- An error is raised if `nextval()` has not been called in the current session.

## Examples

Create two sequences and call `nextval()` for each of them.

```plpgsql
yugabyte=# CREATE SEQUENCE s1;
```

```
CREATE SEQUENCE
```

```plpgsql
yugabyte=# CREATE SEQUENCE s2 START -100 MINVALUE -100;
```

```
CREATE SEQUENCE
```

```plpgsql
yugabyte=# SELECT nextval('s1');
```

```
 nextval
---------
       1
(1 row)
```

```plpgsql
yugabyte=# SELECT nextval('s2');
```

```
 nextval
---------
    -100
(1 row)
```

Call `lastval()`.

```plpgsql
yugabyte=# SELECT lastval()
```

```
 lastval
---------
    -100
(1 row)

```

## See also

[`CREATE SEQUENCE`](../../the-sql-language/statements/ddl_create_sequence)
[`DROP SEQUENCE`](../../the-sql-language/statements/ddl_drop_sequence)
[`currval()`](../func_currval)
[`nextval()`](../func_nextval)
