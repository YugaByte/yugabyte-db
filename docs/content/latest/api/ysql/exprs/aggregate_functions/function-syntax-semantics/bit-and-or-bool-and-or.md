---
title: bit_and(), bit_or(), bool_and(), bool_or()
linkTitle: bit_and(), bit_or(), bool_and(), bool_or()
headerTitle: bit_and(), bit_or(), bool_and(), bool_or()
description: Describes the functionality of the bit_and(), bit_or(), bool_and(), bool_or() YSQL aggregate functions
menu:
  latest:
    identifier: bit-and-or-bool-and-or
    parent: aggregate-function-syntax-semantics
    weight: 30
isTocNested: true
showAsideToc: true
---

These four functions bear a strong mutual family resemblance and so they are all described in the same section.

The invocation semantics and syntax, using the [`GROUP BY` syntax](../avg-count-max-min-sum/#group-by-syntax) or the [`OVER` syntax](../avg-count-max-min-sum/#over-syntax), are the same as were described for [`avg()`, `count`, `max()`, `min()`, and `sum()`](../avg-count-max-min-sum/).

The `bit` data type represents an array of `false/true` values of arbitrary length. For example, `bit(13)` is legal and, when not atomically null, it inevitably represents thirteen `false/true` values. The `OR` and `AND` rules for each bit are described by the well-known two-by-two matrix, with the axes labeled _"0"_ and _"1"_ (conventionally taken to represent `true` and `false`). In contrast, the `OR` and `AND` rules for `boolean` values are ordinarily (and famously) described by a three-by-three matrix, with the axes labelled `null`, `true`, and `false`. However, the general rule for aggregate functions that any `null` elements in the to-be-aggregated set are ignored trumps the normal three-state logic rules. This means that the semantics of the `bit_and()` and `bit_or()` aggregate functions are described in the same way as are the semantics of the `bool_and()` and `bool_or()` aggregate functions. The difference is only that the rules for `bool_and()` and `bool_or()` are specified just for the set of scalar `false/true` values while the  rules for `bit_and()` and `bit_or()` are specified for each bit in the bit vector (*and in the same way for each bit).

## bit_and()

**Signature:**

```
input value:       bit, smallint, int, bigint
                   
return value:      < Same as the input value's data type. >
```

**Purpose:** Returns a value that represents the outcome of the applying the two-by-two matrix `AND` rule to each aligned set of bits for the set of `NOT NULL` input values.

## bit_or()

**Signature:**

```
input value:       bit, smallint, int, bigint
                   
return value:      < Same as the input value's data type. >
```
**Purpose:** Returns a value that represents the outcome of the applying the two-by-two matrix `OR` rule to each aligned set of bits for the set of `NOT NULL` input values.

## bool_and()

**Signature:**

```
input value:       boolean
                   
return value:      boolean
```

**Purpose:** Returns a value that represents the outcome of the applying the two-by-two matrix `AND` rule to to the set of `NOT NULL` input `boolean` values.

## bool_or()

**Signature:**

```
input value:       anyelement, int, [, anyelement]
                   
return value:      anyelement
```
**Purpose:** Returns a value that represents the outcome of the applying the two-by-two matrix `OR` rule to to the set of `NOT NULL` input `boolean` values.

## Examples

First, create the test table.

```plpgsql
drop table if exists t cascade;
create table t(
  k     int    primary key,
  class int    not null,
  b1    bit(8),
  b2    boolean);

insert into t(k, class, b1, b2) values
  (1, 1, '00000001', true),
  (2, 1, '00000010', true),
  (3, 1, '00000100', null),
  (4, 1, '00001000', false),
  (5, 2, '10011000', true),
  (6, 2, '10100100', true),
  (7, 2, null,       true),
  (8, 2, '10000001', true);

\pset null <null>
select
  k,
  class,
  b1::text,
  b2::text
from t
order by k, class;
```

This is the result:

```
 k | class |    b1    |   b2   
---+-------+----------+--------
 1 |     1 | 00000001 | true
 2 |     1 | 00000010 | true
 3 |     1 | 00000100 | <null>
 4 |     1 | 00001000 | false
 5 |     2 | 10011000 | true
 6 |     2 | 10100100 | true
 7 |     2 | <null>   | true
```

 ### GROUP BY syntax example

 Try this:

```plpgsql
select
  class,
  bit_and(b1)::text  as "bit_and(b1)",
  bit_or(b1)::text   as "bit_or(b1)",
  bool_and(b2)::text as "bool_and(b2)",
  bool_or(b2)::text  as "bool_or(b2)"
from t
group by class
order by class;
```

This is the result:

```
 class | bit_and(b1) | bit_or(b1) | bool_and(b2) | bool_or(b2) 
-------+-------------+------------+--------------+-------------
     1 | 00000000    | 00001111   | false        | true
     2 | 10000000    | 10111101   | true         | true
```

### OVER syntax

Try this:

```plpgsql
select
  k,
  class,
  (bit_and(b1)  over w)::text as "bit_and(b1)",
  (bit_or(b1)   over w)::text as "bit_or(b1)",
  (bool_and(b2) over w)::text as "bool_and(b2)",
  (bool_or(b2)  over w)::text as "bool_or(b2)"
from t
window w as (
  partition by class
  order by k
  range between unbounded preceding and current row)
order by k, class;
```

This is the result:

```
 k | class | bit_and(b1) | bit_or(b1) | bool_and(b2) | bool_or(b2) 
---+-------+-------------+------------+--------------+-------------
 1 |     1 | 00000001    | 00000001   | true         | true
 2 |     1 | 00000000    | 00000011   | true         | true
 3 |     1 | 00000000    | 00000111   | true         | true
 4 |     1 | 00000000    | 00001111   | false        | true
 5 |     2 | 10011000    | 10011000   | true         | true
 6 |     2 | 10000000    | 10111100   | true         | true
 7 |     2 | 10000000    | 10111100   | true         | true
 8 |     2 | 10000000    | 10111101   | true         | true
```