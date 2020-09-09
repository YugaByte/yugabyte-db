---
title: ANY and ALL—test if an element is in an array
linkTitle: ANY and ALL
headerTitle: ANY and ALL — test if an element is in an array
description: The ANY and ALL operators compare an element with each of the elements in an array.
menu:
  latest:
    identifier: any-all
    parent: array-functions-operators
    weight: 10
isTocNested: true
showAsideToc: true
---

## Overview

**Signature**

Each of the `ANY` and `ALL` operators has the same signature, thus:

```
input value:       anyelement, anyarray
return value:      boolean
```
`SOME` is a synonym for `ANY`. Therefore this section will make no further mention of `SOME`.

**Note:** The term of art _element_ is used in this section for what the pseudo-type name `anyelement` represents. Each of these operators requires that the data type of the _element_ value to which the LHS expression evaluates corresponds to the data type of the array value to which the RHS expression evaluates. For example, when the data type of the LHS _element_ is, say _"t"_, then the RHS array's data type must be _"t[]"_. The RHS array can have any dimensionality. The operators are sensitive only to the actual _elements_ in the array.

The abbreviations [LHS](https://en.wikipedia.org/wiki/Sides_of_an_equation) and [RHS](https://en.wikipedia.org/wiki/Sides_of_an_equation) have their usual meanings.

**Purpose:** Return `TRUE`, `FALSE`, or `NULL` according to the outcome of the specified set of comparisons. This is the general form of the invocation:

```
any_all_boolean_expression ::=
  lhs_element { = | <> | < | <= | >= | > } { ANY | ALL} rhs_array
```

Notice that `!=` was omitted from the list of equality and inequality operators because it's just an alternative spelling of the `<>` inequality operator.

Because _"any_all_boolean_expression"_ is just that, a `boolean` expression, it can optionally be preceded with the `NOT` unary operator and it can be conjoined with other `boolean` expressions using `AND`, and `OR` in the normal way.

The evaluation of _"any_all_boolean_expression"_ visits each of the array's elements in turn (as mentioned, the array's dimensionality doesn't matter) and it performs the specified equality or inequality comparison between the LHS _element_ and the current array _element_.

## Semantics

The accounts of `ANY` and `ALL` are symmetrical and complementary. It's therefore most effective to describe the semantics jointly in a single account.

Notice that the use of _element_ and _array_ (in this overall section on `ANY` and `AND`)  acknowledges the fact that the LHS and the RHS are each, in general, expressions. So _element_ acts as shorthand for the value to which the LHS expression evaluates. and _array_ acts as shorthand for the value to which the RHS evaluates.

### Simple scenario

The simple scenario is restricted to the case that the LHS _element_ `IS NOT NULL`, the RHS array `IS NOT NULL`, and the RHS array's cardinality is at least one.

When the LHS _element_ is compared with each of the RHS array's _elements_, the comparisons use the appropriate data type overload of the particular equality or inequality operator that is used in the _"any_all_boolean_expression"_.

- If `ANY` is used, then the result is `TRUE` only if at least one of the successive comparisons evaluates to `TRUE`. (It doesn't matter if zero or more of the other comparisons evaluates to `NULL`.) If every one of the comparisons evaluates to `FALSE`, then the result is `FALSE`. If every one of the comparisons evaluates to `NULL` is the result  `NULL`.

- If `ALL` is used, then the result is `TRUE` only if every one of the successive comparisons evaluates to `TRUE`. If at least one of the comparisons evaluates to `FALSE` and not one evaluates to `NULL`, then the result is `FALSE`. If at least one of the comparisons evaluates to `NULL` then the result is `NULL`.

Notice that `ANY` is comparable to `OR` and that `ALL` is comparable to `AND` in this way:

- If `ANY` is used, then the result is the `OR` combination of the successive individual comparisons.

- If `ALL` is used, then the result is the `AND` combination of the successive individual comparisons.

This `DO` block demonstrates the semantics of `OR` and `AND`:

```plpgsql
do $body$
begin
  -- OR
  assert     (true  or  false or  null),           'assert failed';
  assert not (false or  false or  false),          'assert failed';
  assert     (false or  false or  null)   is null, 'assert failed';

  -- AND
  assert     (true  and true  and true),           'assert failed';
  assert not (true  and true  and false),          'assert failed';
  assert     (true  and true  and null)   is null, 'assert failed';
end;
$body$;
```

The next `DO` block is a mechanical (manual) re-write of the block that demonstrates the semantics of `OR` and `AND`. It correspondingly demonstrates the semantics of `ANY` and `ALL`.

```plpgsql
do $body$
begin
  -- ANY
  assert     (true = any (array[true,  false, null ]::boolean[])),         'assert failed';
  assert not (true = any (array[false, false, false]::boolean[])),         'assert failed';
  assert     (true = any (array[false, false, null ]::boolean[])) is null, 'assert failed';

  -- ALL
  assert     (true = all (array[true,  true,  true ]::boolean[])),         'assert failed';
  assert not (true = all (array[true,  true,  false]::boolean[])),         'assert failed';
  assert     (true = all (array[true,  true,  null ]::boolean[])) is null, 'assert failed';
end;
$body$;
```

The combination `= ANY` is functionally equivalent to `IN` (but `IN` is illegal syntax when the RHS is an array).

The combination `= ALL` has no functional equivalent in the way that `= ANY` is functionally equivalent to `IN`. 

The following small test emphasizes the symmetry between `ANY` and `IN`.

```plpgsql
select (
    42 = any (array[17, 42, 53])
    and
    42 in (17, 42, 53)  
  )::text as b;
```
See [The `array[]` value constructor](../../array-constructor/). This is the result:
```
  b   
------
 true
```
Notice that here, _"(17, 42, 53)"_ is _not_ a constructed record value because of the semantic effect of preceding it by `IN`. In contrast, in the following example, _"(17, 42, 53)"_ _is_ a constructed record:

```plpgsql
with v as (select (17, 42, 53) as r)
select pg_typeof(r)::text from v;
```

This is the result:

```
 pg_typeof 
-----------
 record
```

This outcome is due to the semantic effect of using _"(17, 42, 53)"_ directly as a `SELECT` list item. This, therefore, causes a syntax error:

```
with v as (select (1, 2, 3, 4) as r)
select 1 in r from v;
```

### Exotic scenario

This section explains the semantics: when one, or both, of the LHS _element_ and the RHS array `IS NULL`; and when neither the LHS or the RHS  `IS NULL` and the RHS array's cardinality is zero.

- If either the LHS _element_ or the RHS array `IS NULL`, then both the `ANY` result `IS NULL` and the `ALL` result `IS NULL`.

- If both the LHS _element_ `IS NOT NULL`and the RHS array `IS NOT NULL` and the RHS array has zero _elements_, then the `ANY` result is `FALSE`.
- If both the LHS _element_ `IS NOT NULL`and the RHS array `IS NOT NULL` and the RHS array has zero _elements_, then the `ALL` result is `TRUE`. You might think that this is a counter-intuitive rule definition. But the [PostgreSQL documentation for Version 11.2](https://www.postgresql.org/docs/11/functions-comparisons.html#id-1.5.8.28.17) states this clearly. And the first example in [Semantics demonstration](./#semantics-demonstration) is consistent with this definition. Moreover, and as is required to be the case, the behavior of the example is identical using YugabyteDB and PostgreSQL Version 11.2.

## Semantics demonstration

The following semantics demonstrations show the use of an array of _atomic elements_,  an array of _composite elements_, and an array of _elements_ that are values of a `DOMAIN`.

### Using an array of atomic elements

```plpgsql
do $body$
declare
  v1 constant int := 1;
  v2 constant int := 2;
  v3 constant int := 3;
  v4 constant int := 4;
  v5 constant int := 5;
  v6 constant int := null;

  arr1 constant int[] := array[v1, v1, v1, v1];
  arr2 constant int[] := array[v1, v1, v1, v2];
  arr3 constant int[] := array[v1, v2, v3, v4];
  arr4 constant int[] := array[v1, v1, v1, null];
  arr5 constant int[] := null;

  -- Notice that an array with zero elements is nevertheless NOT NULL.
  arr6 constant int[] not null := '{}';

  b01 constant boolean not null :=      v1 = all (arr1);
  b02 constant boolean not null := not  v1 = all (arr3);
  b03 constant boolean not null := not  v1 = all (arr2);
  b04 constant boolean not null :=      v1 = any (arr3);
  b05 constant boolean not null := not  v5 = any (arr3);

  b06 constant boolean not null :=      v1 = any (arr4);
  b07 constant boolean not null :=     (v5 = any (arr4)) is null;
  b08 constant boolean not null :=     (v1 = all (arr4)) is null;

  b09 constant boolean not null :=     (v1 = any (arr5)) is null;
  b10 constant boolean not null :=     (v6 = any (arr1)) is null;
  b11 constant boolean not null :=     (v1 = all (arr5)) is null;
  b12 constant boolean not null :=     (v6 = all (arr1)) is null;

  b13 constant boolean not null := not (v1 = any (arr6));
  b14 constant boolean not null :=     (v1 = all (arr6));
begin
  assert
    (b01 and b02 and b03 and b04 and b05 and b06 and b07 and b08
         and b09 and b10 and b11 and b12 and b13 and b14),
  'assert failed';
end;
$body$;
```
Here is a mechanically derived example from the code above that uses two-dimensional arrays in place of one-dimensional arrays. But some of the tests were removed to help readability. The outcomes of the remaining tests are unchanged because these depend only upon the array's actual elements and not upon its geometry.

```plpgsql
do $body$
declare
  v1 constant int := 1;
  v2 constant int := 2;
  v3 constant int := 3;
  v4 constant int := 4;
  v5 constant int := 5;

  arr1 constant int[] := array[array[v1, v1], array[v1, v1]];
  arr2 constant int[] := array[array[v1, v2], array[v3, v4]];
  arr3 constant int[] := array[array[v1, v1], array[v1, null]];

  b1 constant boolean not null :=     v1 = all (arr1);
  b2 constant boolean not null := not v1 = all (arr2);
  b3 constant boolean not null :=     v1 = any (arr2);
  b4 constant boolean not null := not v5 = any (arr2);

  b5 constant boolean not null :=     v1 = any (arr3);
  b6 constant boolean not null :=    (v5 = any (arr3)) is null;
  b7 constant boolean not null :=    (v1 = all (arr3)) is null;
begin
  assert
    (b1 and b2 and b3 and b4 and b5 and b6 and b7),
  'assert failed';
end;
$body$;
```

### Using an array of composite "row" type value elements

This code was produced by mechanically replacing `int` with _"rt"_ and by changing the spelling of the values that are assigned to  _"v1"_ through _"v5_" accordingly. But, again, some of the tests were removed to help readability. The rest of the remaining code is identical to its counterpart in the first example.

```plpgsql
drop type if exists rt;
create type rt as (a int, b text);

do $body$
declare
  v1 constant rt := (0, 1);
  v2 constant rt := (2, 3);
  v3 constant rt := (4, 5);
  v4 constant rt := (6, 7);
  v5 constant rt := (8, 9);

  arr1 constant rt[] := array[v1, v1, v1, v1];
  arr2 constant rt[] := array[v1, v2, v3, v4];
  arr3 constant rt[] := array[v1, v1, v1, null::rt];

  b1 constant boolean not null :=     v1 = all (arr1);
  b2 constant boolean not null := not v1 = all (arr2);
  b3 constant boolean not null :=     v1 = any (arr2);
  b4 constant boolean not null := not v5 = any (arr2);

  b5 constant boolean not null :=     v1 = any (arr3);
  b6 constant boolean not null :=    (v5 = any (arr3)) is null;
  b7 constant boolean not null :=    (v1 = all (arr3)) is null;
begin
  assert
    (b1 and b2 and b3 and b4 and b5 and b6 and b7),
  'assert failed';
end;
$body$;
```

### Using an array of elements that are values of a DOMAIN

First, consider these two examples:

```plpgsql
drop domain if exists d1_t;
create domain d1_t as int
default 42 constraint d1_t_chk check(value >= 17);

drop domain if exists d2_t;
create domain d2_t as int[];
```

It's clear that the values of _"d1_t"_, as a specialized kind of `int`, are _elements_. But what about the values of _"d2_t"_ which is a specialized kind of _array_? Critically, but somewhat counter-intuitively,  _"d2_t"_ does _not_ qualify as `anyarray`. Rather, it qualifies as `anyelement`. This code example underlines the point:

```plpgsql
create or replace procedure p(i in anyelement)
  language plpgsql
as $body$
begin
  raise info '%', pg_typeof(i);
end;
$body$;

call p(53::d1_t);

call p('{1, 2}'::d2_t);
```

The first `CALL` produces this result:

```
INFO:  d1_t
```

and the second `CALL` produces this result:

```
INFO:  d2_t
```

Once again, the following code was produced by mechanically replacing `int` — this time with _"int_arr_t"_ and by changing the spelling of the values that are assigned to  _"v1"_ through _"v5"_ accordingly. But, again, some of the tests were removed to help readability. The rest of the remaining code is identical to its counterpart in the first example.

```plpgsql
drop domain if exists int_arr_t;
create domain int_arr_t as int[];

do $body$
declare
  v1 constant int_arr_t := array[1, 1];
  v2 constant int_arr_t := array[2, 3];
  v3 constant int_arr_t := array[4, 5];
  v4 constant int_arr_t := array[5, 7];
  v5 constant int_arr_t := array[8, 9];

  arr1 constant int_arr_t[] := array[v1, v1, v1, v1];
  arr2 constant int_arr_t[] := array[v1, v2, v3, v4];
  arr3 constant int_arr_t[] := array[v1, v1, v1, null::int_arr_t];

  b1 constant boolean not null :=     v1 = all (arr1);
  b2 constant boolean not null := not v1 = all (arr2);
  b3 constant boolean not null :=     v1 = any (arr2);
  b4 constant boolean not null := not v5 = any (arr2);

  b5 constant boolean not null :=     v1 = any (arr3);
  b6 constant boolean not null :=    (v5 = any (arr3)) is null;
  b7 constant boolean not null :=    (v1 = all (arr3)) is null;
begin
  assert
    (b1 and b2 and b3 and b4 and b5 and b6 and b7),
  'assert failed';
end;
$body$;
```

### Test to show that an array's _elements_ are all the same as each other

This demonstration shows you that you can test whether an array's _elements_ are all the same as each other without needing to know their common value or anything about the array's dimensionality.

```plpgsql
do $body$
declare
  arr constant int[] not null := '
    [2:3][4:6][7:10]={
      {
        {42,42,42,42},{42,42,42,42},{42,42,42,42}
      },
      {
        {42,42,42,42},{42,42,42,42},{42,42,42,42}
      }
    }'::int[];

   val constant int not null := (
    select unnest(arr) limit 1);

  b99 boolean not null := val = all (arr);
begin
  assert b99, 'assert failed';
end;
$body$;
```
The general syntax for the literal for a multidimensional array that specifies the lower and upper index bounds along each dimension is described in [Multidimensional array of `int` values](../../literals/array-of-primitive-values/#multidimensional-array-of-int-values).

For the specification of the behavior `unnest()` when its actual argument is a multidimensional array see [Multidimensional array_agg() and unnest() — first overloads](..//array-agg-unnest/#multidimensional-array-agg-and-unnest-first-overloads).

### Using inequality comparisons

```plpgsql
do $body$
declare
  v1 constant int := 1;
  v2 constant int := 2;
  v3 constant int := 3;
  v4 constant int := 4;
  v5 constant int := 5;

  arr1 constant int[] := array[v1, v2, v3, v4];

  b01 constant boolean not null := not(v5  = any (arr1));
  b02 constant boolean not null :=     v5 <> all (arr1);
  b03 constant boolean not null :=     v2  > any (arr1);
  b04 constant boolean not null := not(v2  > all (arr1));
  b05 constant boolean not null := not(v4  < any (arr1));
  b06 constant boolean not null :=     v2  < any (arr1);
  b07 constant boolean not null := not(v5 <= any (arr1));
  b08 constant boolean not null :=     v4 >= all (arr1);
  b09 constant boolean not null :=     v5  > all (arr1);

begin
  assert
    (b01 and b02 and b03 and b04 and b05 and b06 and b07 and b08 and b09),
  'assert failed';
end;
$body$;
```
