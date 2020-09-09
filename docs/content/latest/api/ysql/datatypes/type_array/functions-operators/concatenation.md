---
title: Array concatenation functions and operators
linkTitle: Array concatenation
headerTitle: Array concatenation functions and operators
description: Array concatenation functions and operators
menu:
  latest:
    identifier: array-concatenation
    parent: array-functions-operators
    weight: 40
isTocNested: true
showAsideToc: true
---

The `||` operator implements, by itself, all of the functionality that each of the `array_cat()`, `array_append()`, and `array_prepend()` functions individually implement. Yugabyte recommends that you use the `||` operator and avoid the functions. They are documented here for completeness—especially in case you find them in inherited code.

## The&#160; &#160;||&#160; &#160;operator

**Purpose:** Return the concatenation of any number of compatible `anyarray` and `anyelement` values.

**Signature**

```
LHS and RHS input value:  [anyarray | anyelement] [anyarray | anyelement]*
return value:             anyarray
```
**Note:** "_Compatible"_ is used here to denote two requirements:

- The values within the array, or the value of the scalar, must be of the same data type, for example, an `int[]` array and an `int` scalar.
- The LHS and the RHS must be _dimensionally_ compatible. For example, you can produce a one-dimensional array: _either_ by concatenating two scalars; _or_ by concatenating a scalar and a one-dimensional array; _or_ by concatenating two one-dimensional arrays. This notion extends to multidimensional arrays. The next bullet gives the rules.
- When you concatenate two N-dimensional arrays, the lengths along the major (that is, the first dimension) may be different but the lengths along the other dimensions must be identical. And when (as the analogy of concatenating a one-dimensional array and a scalar) you concatenate an N-dimensional and an (N-1)-dimensional array, the lengths along the dimensions of the (N-1)-dimensional array must all be identical to the corresponding lengths along the dimensions that follow the major dimension in the N-dimensional array.

These rules follow directly from the fact that arrays are rectilinear. For examples, see [|| operator semantics](./#operator-semantics) below.

**Example:**

```plpgsql
create table t(k int primary key, arr int[]);
insert into t(k, arr)
values (1, '{3, 4, 5}'::int[]);

select arr as "old value of arr" from t where k = 1;

update t
set arr = '{1, 2}'::int[]||arr||6::int
where k = 1;

select arr as "new value of arr" from t where k = 1;
```
It shows this:
```
 old value of arr 
------------------
 {3,4,5}
```
and then this:
```
 new value of arr 
------------------
 {1,2,3,4,5,6}
```

## array_cat()

**Purpose:** Return the concatenation of two compatible `anyarray` values.

**Signature**
```
input value:              anyarray, anyarray
return value:             anyarray
```
**Note:** The `DO` block shows that the `||` operator is able to implement the full functionality of the `array_cat()` function.

```plpgsql
do $body$
declare
  arr_1 constant int[] := '{1, 2, 3}'::int[];
  arr_2 constant int[] := '{4, 5, 6}'::int[];
  val constant int := 5;
  workaround constant int[] := array[val];
begin
  assert
    array_cat(arr_1, arr_2)      = arr_1||arr_2 and
    array_cat(arr_1, workaround) = arr_1||val   ,
  'unexpected';
end;
$body$;
```
## array_append()
**Purpose:** Return an array that results from appending a scalar value to (that is, _after_) an array value.

**Signature**
```
input value:              anyarray, anyelement
return value:             anyarray
```
**Note:** The `DO` block shows that the `||` operator is able to implement the full functionality of the `array_append()` function. The values must be compatible.

```plpgsql
do $body$
declare
  arr constant int[] := '{1, 2, 3, 4}'::int[];
  val constant int := 5;
  workaround constant int[] := array[val];
begin
  assert
    array_append(arr, val) = arr||val                   and
    array_append(arr, val) = array_cat(arr, workaround) ,
  'unexpected';
end;
$body$;
```
## array_prepend()
**Purpose:** Return an array that results from prepending a scalar value to (that is, _before_) an array value.

**Signature**
```
input value:              anyelement, anyarray
return value:             anyarray
```
**Note:** The `DO` block shows that the `||` operator is able to implement the full functionality of the `array_prepend()` function. The values must be compatible.

```plpgsql
do $body$
declare
  arr constant int[] := '{1, 2, 3, 4}'::int[];
  val constant int := 5;
  workaround constant int[] := array[val];
begin
  assert
    array_prepend(val, arr) = val||arr                   and
    array_prepend(val, arr) = array_cat(workaround, arr) ,
  'unexpected';
end;
$body$;
```
## Concatenation semantics

**Semantics for one-arrays**

```plpgsql
create type rt as (f1 int, f2 text);

do $body$
declare
  arr constant rt[] := array[(3, 'c')::rt, (4, 'd')::rt, (5, 'e')::rt];

  prepend_row  constant rt := (0, 'z')::rt;
  prepend_arr  constant rt[] := array[(1, 'a')::rt, (2, 'b')::rt];
  append_row   constant rt := (6, 'f')::rt;

  cat_result   constant rt[] := prepend_row||prepend_arr||arr||append_row;

  expected_result constant rt[] :=
    array[(0, 'z')::rt, (1, 'a')::rt, (2, 'b')::rt, (3, 'c')::rt, 
         (4, 'd')::rt, (5, 'e')::rt, (6, 'f')::rt];

begin
  assert
    (cat_result   = expected_result),
  'unexpected';
end;
$body$;
```

**Semantics for multidimensional arrays**

```plpgsql
do $body$
declare
  -- arr_1 and arr_2 are demensionally compatible.
  -- Its's OK for array_length(*, 1) to differ.
  -- But array_length(*, 1) must be the same.
  arr_1 constant int[] :=
    array[
       array[11, 12, 13],
       array[21, 22, 23]
    ];

  arr_2 constant int[] :=
    array[
       array[31, 32, 33],
       array[41, 42, 43],
       array[51, 52, 53]
    ];

  -- Notice that this is a 1-d array.
  -- Its lenth is the same as as that of arr_1
  -- along arr_1's SECOND dimension.
  arr_3 constant int[] := array[31, 32, 33];

  -- Notice that bad_arr is dimensionally INCOMPATIBLE with arr_1:
  -- they have different lengths along their SECOND major dimension.
  bad_arr constant int[] :=
    array[
       array[61, 62, 63, 64],
       array[71, 72, 73, 74],
       array[81, 82, 83, 84]
    ];

  expected_cat_1 constant int[] :=
    array[
       array[11, 12, 13],
       array[21, 22, 23],
       array[31, 32, 33],
       array[41, 42, 43],
       array[51, 52, 53]
    ];

  expected_cat_2 constant int[] :=
    array[
       array[11, 12, 13],
       array[21, 22, 23],
       array[31, 32, 33]
    ];
begin
  assert
    arr_1||arr_2 = expected_cat_1 and
    arr_1||arr_3 = expected_cat_2,
  'unexpected';

  declare
    a int[];
  begin
    -- ERROR: cannot concatenate incompatible arrays.
    a := arr_1||bad_arr;
  exception
    when array_subscript_error then null;
  end;
end;
$body$;
```
