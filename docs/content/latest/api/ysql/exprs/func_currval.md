---
title: currval() function [YSQL]
headerTitle: currval()
linkTitle: currval()
description: Returns the last value returned by the nextval() function for the specified sequence in the current session.
menu:
  latest:
    identifier: api-ysql-exprs-currval
    parent: api-ysql-exprs
aliases:
  - /latest/api/ysql/exprs/func_currval
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `currval( sequence_name )` function to return the last value returned by the `nextval( sequence_name )` function for the specified sequence in the current session.

## Semantics

### _sequence_name_

Specify the name of the sequence.

- An error is raised if `nextval( sequence_name )` has not been called for the specified sequence in the current session.

## Examples

### Create a sequence

```plpgsql
yugabyte=# CREATE SEQUENCE s;
```

```
CREATE SEQUENCE
```

Call `nextval()`.

```plpgsql
yugabyte=# SELECT nextval('s');
```

```
 nextval
---------
       1
(1 row)
```

```plpgsql
yugabyte=# SELECT currval('s');
```

```
 currval
---------
       1
(1 row)
```

Call `currval()` before `nextval()` is called.

```plpgsql
yugabyte=# CREATE SEQUENCE s2;
```

```
CREATE SEQUENCE
```

```plpgsql
SELECT currval('s2');
```

```
ERROR:  currval of sequence "s2" is not yet defined in this session
```

## See also

- [`CREATE SEQUENCE`](../../the-sql-language/statements/ddl_create_sequence)
- [`DROP SEQUENCE`](../../the-sql-language/statements/ddl_drop_sequence/)
- [`lastval()`](../func_lastval)
- [`nextval()`](../func_nextval)
