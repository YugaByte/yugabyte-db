---
title: currval() function [YSQL]
headerTitle: currval()
linkTitle: currval()
description: Returns the last value returned by the nextval() function for the specified sequence in the current session.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-exprs-currval
    parent: api-ysql-exprs
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

```postgresql
yugabyte=# CREATE SEQUENCE s;
```

```
CREATE SEQUENCE
```

Call `nextval()`.

```postgresql
yugabyte=# SELECT nextval('s');
```

```
 nextval
---------
       1
(1 row)
```

```postgresql
yugabyte=# SELECT currval('s');
```

```
 currval
---------
       1
(1 row)
```

Call `currval()` before `nextval()` is called.

```postgresql
yugabyte=# CREATE SEQUENCE s2;
```

```
CREATE SEQUENCE
```

```postgresql
SELECT currval('s2');
```

```
ERROR:  currval of sequence "s2" is not yet defined in this session
```

## See also

- [`CREATE SEQUENCE`](../../commands/ddl_create_sequence)
- [`DROP SEQUENCE`](../../commands/drop_sequence)
- [`lastval()`](../func_lastval)
- [`nextval()`](../func_nextval)
