---
title: nextval() function [YSQL]
headerTitle: nextval()
linkTitle: nextval()
description: Returns the next value from the sequence cache for the current session. 
menu:
  latest:
    identifier: api-ysql-exprs-nextval
    parent: api-ysql-exprs
aliases:
  - /latest/api/ysql/exprs/func_nextval
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `nextval( sequence_name )` function to return the next value from the sequence cache for the current session. If no more values are available in the cache, the session allocates a block of numbers for the cache and returns the first one. The number of elements allocated is determined by the `cache` option specified as part of the `CREATE SEQUENCE` statement.

## Semantics

### _sequence_name_

Specify the name of the sequence.

- An error is raised if a sequence reaches its minimum or maximum value.

## Examples

### Create a simple sequence that increments by 1 every time nextval() is called

```plpgsql
yugabyte=# CREATE SEQUENCE s;
```

```
CREATE SEQUENCE
```

Call nextval() a couple of times.

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
yugabyte=# SELECT nextval('s');
```

```
 nextval
---------
       2
(1 row)
```

### Create a sequence with a cache of 3 values

```plpgsql
yugabyte=# CREATE SEQUENCE s2 CACHE 3;
```

```
CREATE SEQUENCE
```

In the same session, call `nextval()`. The first time it's called, the session's cache will allocate numbers 1, 2, and 3. This means that the data for this sequence will have its `last_val` set to 3. This modification requires two RPC requests.

```plpgsql
SELECT nextval('s2');
```

```
 nextval
---------
       1
(1 row)
```

The next call of `nextval()` in the same session will not generate new numbers for the sequence, so it is much faster than the first `nextval()` call because it will just use the next value available from the cache.

```plpgsql
SELECT nextval('s2');
```

```
nextval
---------
       2
(1 row)
```

## See also

- [`CREATE SEQUENCE`](../../the-sql-language/statements/ddl_create_sequence)
- [`DROP SEQUENCE`](../../the-sql-language/statements/ddl_drop_sequence)
- [`currval()`](../func_currval)
- [`lastval()`](../func_lastval)
