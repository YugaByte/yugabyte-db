---
title: setval() function [YSQL]
headerTitle: setval()
linkTitle: setval()
description: Set and return the value for the specified sequence.
menu:
  latest:
    identifier: api-ysql-exprs-setval
    parent: api-ysql-setval
aliases:
  - /latest/api/ysql/exprs/func_setval
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `setval( sequence_name , value, is_called)` function to set and return the value for the specified sequence.

## Semantics

### _sequence_name_

Specify the name of the sequence.

### _sequence_name_

Specify the name of the sequence.

### _value_

Specify the value of the sequence.

### _flag_

Specify the name of the sequence.

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

- [`CREATE SEQUENCE`](../../commands/ddl_create_sequence)
- [`DROP SEQUENCE`](../../commands/drop_sequence)
- [`lastval()`](../func_lastval)
- [`nextval()`](../func_nextval)
