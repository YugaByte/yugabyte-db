---
title: Numeric data types [YSQL]
headerTitle: Numeric data types
linkTitle: Numeric
description: YSQL numeric data types represent integers, floating-point numbers, and fixed-point numbers of different value ranges and precisions.
menu:
  latest:
    identifier: api-ysql-datatypes-numeric
    parent: api-ysql-datatypes
aliases:
  - /latest/api/ysql/type_number/
  - /latest/api/ysql/type_int/
isTocNested: true
showAsideToc: true
---

## Synopsis

YSQL support integers, floating-point numbers, and fixed-point numbers of different value ranges and precisions.

Data type | Description | Min | Max |
----------|-------------|-----|-----|
`BIGINT` | 8 bytes | -9,223,372,036,854,775,808 | 9,223,372,036,854,775,807 |
`DEC` | user-specified precision, exact | <131072 digits.16383 digits> | <131072 digits.16383 digits> |
`DECIMAL` | user-specified precision, exact | <131072 digits.16383 digits> | <131072 digits.16383 digits> |
`DOUBLE PRECISION` | Inexact 64-bit floating point number | 15-digit precision | 15-digit precision|
`FLOAT` | Inexact 64-bit floating point number | variable | variable |
`INTEGER` | 4-byte integer | -2,147,483,648 | 2,147,483,647 |
`INT` | 4-byte interger | -2,147,483,648 | 2,147,483,647 |
`NUMERIC` | user-specified precision, exact | <131072 digits.16383 digits> | <131072 digits.16383 digits> |
`REAL` | Inexact 32-bit floating point number | 6-digit precision | 6-digit precision |
`SMALLINT` | 2-byte integer | -32,768 | 32,767 |

## Integers

The following keywords are used to specify a column of type integer for different constraints, including its value ranges.

```
type_specification ::= SMALLINT | INT | INTEGER | BIGINT
integer_literal ::= [ + | - ] digit [ { digit | , } ... ]
```

- Columns of type `SMALLINT`, `INT`, `INTEGER`, or `BIGINT` can be part of the `PRIMARY KEY`.
- Values of different integer data types are comparable and convertible to one another.
- Values of integer data types are convertible but not comparable to floating point number.
- Currently, values of floating point data types are not convertible to integers. This restriction will be removed in the near future.

## Floating-point numbers

The following keywords are used to specify a column of floating-point types for different constraints including its value ranges.

```
type_specification ::= { FLOAT | DOUBLE PRECISION | REAL }
floating_point_literal ::= non_integer_fixed_point_literal | "NaN" | "Infinity" | "-Infinity"
```

- Columns of type `REAL`, `DOUBLE PRECISION`, and `FLOAT` can be part of the `PRIMARY KEY`.
- Values of different floating-point and fixed-point data types are comparable and convertible to one another.
- Conversion from floating-point types into `DECIMAL` will raise an error for the special values `NaN`, `Infinity`, and `-Infinity`.
- The ordering for special floating-point values is defined as (in ascending order): `-Infinity`, all negative values in order, all positive values in order, `Infinity`, and `NaN`.
- Values of non-integer numeric data types are neither comparable nor convertible to integer although integers are convertible to them. This restriction will be removed.

## Fixed-point numbers

The following keywords are used to specify a column of exact user-specified precision types for different constraints
 including its value ranges.

```
type_specification ::= { DEC | DECIMAL | NUMERIC }
fixed_point_literal ::= [ + | - ] { digit [ digit ...] '.' [ digit ...] | '.' digit [ digit ...] }

```

- Columns of type `DEC`, `DECIMAL`, and `NUMERIC` can be part of the `PRIMARY KEY`.
- Values of different floating-point and fixed-point data types are comparable and convertible to one another.
- Values of non-integer numeric data types are neither comparable nor convertible to integer although integers are convertible to them. This restriction will be removed.
