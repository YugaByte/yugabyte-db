---
title: Array properties
linkTitle: Array properties
headerTitle: Functions for reporting the geometric properties of an array
description: Functions for reporting the geometric properties of an array
menu:
  latest:
    identifier: array-properties
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---

These functions return the various dimensional properties that jointly characterize an array. These three together completely specify the shape and size:

- its dimensionality (`int`)
- the lower bound along each dimension (`int`)
- the upper bound along each dimension (`int`).

There are functions for returning two other properties: the length along each dimension (`int`); and its cardinality (`int`). But each of these can be derived from set set of the lower and upper bounds for all the dimensions. There is also a function that returns the values for the lower and upper bounds for all the dimensions as a single `text` value.

## Overview

The behavior of each of the functions for reporting the geometric properties of an array is illustrated by supplying the same two arrays as the first actual argument to two invocations of the function. The return value, in all cases, is insensitive to the array's data type and the values within the array.

Create and populate _"table t"_ thus:

```plpgsql
create table t(k int primary key, arr_1 text[], arr_2 text[]);
insert into t(k, arr_1, arr_2) values(1,
      '[3:10]={ 1, 2, 3, 4,   5, 6, 7, 8}',
  '[2:3][4:7]={{1, 2, 3, 4}, {5, 6, 7, 8}}'
  );
```

You will use it in the example for each of the functions. (The optional syntax items `[3:10]` and `[2:3][2:5]` specify the lower and upper bounds along the one dimension of the first array and along both dimensions of the second array. This syntax is explained in [Multidimensional array of numeric values](../../literals/array-of-primitive-values/#multidimensional-array-of-numeric-values).

Run the `SELECT` statement for each function to illustrate what produces for the same pair of input arrays.

### array_ndims()

**Purpose:** Return the number of dimensions (that is, the _dimensionality_) of the specified array.

**Signature:**
```
input value:       anyarray
return value:      int
```
**Example:**

```plpgsql
select
  array_ndims(arr_1) as ndims_1,
  array_ndims(arr_2) as ndims_2
from t where k = 1;
```
It produces this result:
```
  ndims_1 | ndims_2 
---------+---------
       1 |       2
```
### array_lower()

**Purpose:** Return the lower bound of the specified array along the specified dimension.

**Signature:**
```
input value:       anyarray, int
return value:      int
```
**Example:**

```plpgsql
select
  array_lower(arr_1, 1) as arr_1_lb,
  array_lower(arr_2, 1) as arr_2_lb_1,
  array_lower(arr_2, 2) as arr_2_lb_2
from t where k = 1;
```
It produces this result:
```
 arr_1_lb | arr_2_lb_1 | arr_2_lb_2 
----------+------------+------------
        3 |          2 |          4
```
### array_upper()

**Purpose:** Return the upper bound of the specified array along the specified dimension.

**Signature:**
```
input value:       anyarray, int
return value:      int
```
**Example:**
The use of `array_upper()` is exactly symmetrical with the use of `array_lower()`.
```plpgsql
select
  array_upper(arr_1, 1) as arr_1_ub,
  array_upper(arr_2, 1) as arr_2_ub_1,
  array_upper(arr_2, 2) as arr_2_ub_2
from t where k = 1;
```
It produces this result:
```
 arr_1_ub | arr_2_ub_1 | arr_2_ub_2 
----------+------------+------------
       10 |          3 |          7
```
### array_length()

**Purpose:** Return the length of the specified array along the specified dimension. Notice that, among `array_lower()` and `array_upper()` and `array_length()`, the result of any two of them determines the result of the third. You could therefore decide, for example, only to use `array_lower()` and `array_upper()` and to determine the length, when you need it, by subtraction. But, for code clarity and brevity, you may as well use exactly the one that matches your present purpose.

**Signature:**
```
input value:       anyarray, int
return value:      int
```
**Example:**
The use of `array_length()` is exactly symmetrical with the use of `array_lower()` and `array_upper()`.

```plpgsql
select
  array_length(arr_1, 1) as arr_1_len,
  array_length(arr_2, 1) as arr_2_len_1,
  array_length(arr_2, 2) as arr_2_len_2
from t where k = 1;
```
It produces this result:
```
 arr_1_len | arr_2_len_1 | arr_2_len_2 
-----------+-------------+-------------
         8 |           2 |           4
```
### cardinality()

**Purpose:** Return the total number of values in the specified array. Notice that the value that this function returns can be computed as the product of values returned by the `array_length()` function along each direction.

**Signature:**
```
input value:       anyarray
return value:      int
```
**Example:**

```plpgsql
select
  cardinality(arr_1) as card_1,
  cardinality(arr_2) as card_2
from t where k = 1;
```
It produces this result:
```
 card_1 | card_2 
--------+--------
      8 |      8
```
### array_dims()

**Purpose:** Return a text representation of the same information as `array_lower()` and `array_length()` return, for all dimensions, in a single text value.

**Signature:**
```
input value:       anyarray
return value:      text
```
**Example:**
The `array_dims()` function is useful to produce a result that is easily humanly readable. If you want to use the information that it returns programmatically, then you should use `array_lower()`, `array_upper()`, or `array_length()`.

```plpgsql
select
  array_dims(arr_1) as arr_1_dims,
  array_dims(arr_2) as arr_2_dims
from t where k = 1;
```
It produces this result:
```
 arr_1_dims | arr_2_dims 
------------+------------
 [3:10]     | [2:3][4:7]
```

## Joint semantics

Create the procedure _"assert&#95;semantics&#95;and&#95;traverse&#95;values()"_ and then invoke it for each of the three provided data sets. You supply it with a one-dimensional array and a two dimensional array and, for each, your humanly determined estimates of these values:

- what `array_ndims()` returns
- what `array_lower()` returns for each dimension
- what `array_upper()` returns for each dimension.

The procedure obtains the actual values, programmatically, for all of the values that you supply and asserts that they agree.

Notice that the cardinality and the length along each dimension are omitted, by design, from this list. The procedure also obtains these values, programmatically, and checks that these agree with the values that are computed from, respectively, the length along each dimension and the upper and lower bounds along each dimension.

Notice, too, that the value that `array_dims()` returns can be computed from the upper and lower bounds along each dimension. So the procedure does this too and checks that the value returned by `array_dims()` is consistent with the values returned by `array_lower()` and `array_upper()`.

The procedure has some particular requirements:

- The cardinality of each of the two supplied arrays must be the same.
- The actual array values, in row-major order, must be the same, pairwise.

Briefly, _"row-major"_ order is the order in which the last subscript varies most rapidly.

Meeting these requirements are allows the procedure to deliver two bonus benefits. _First_, it demonstrates how to traverse array values in row-major order using the values returned by the functions that this section describes, thereby demonstrating what the term "row-major order" means. _Second_. it compares the values, pairwise, for equality. This comparison rule is the basis of the semantics of the comparison operations described in [Operators for comparing two arrays](../comparison).

**Note:** There are no built-in functions for computing, for example, the product of two matrices or the product of a vector and a matrix. (A vector is a one-dimensional array, and a matrix is a two-dimensional array.) But, as long as you know how to traverse the values in a matrix in row-major order, you can implement the missing vector and matrix multiplication functionality for yourself.

```plpgsql
create procedure assert_semantics_and_traverse_values(
  a in int[], b in int[],

  a_ndims in int, a_lb   in int, a_ub   in int,

  b_ndims in int, b_lb_1 in int, b_ub_1 in int,
                  b_lb_2 in int, b_ub_2 in int)
  language plpgsql
as $body$
declare
  -- Get the facts that are implied by the user-supplied facts.
  a_len   constant int := array_length(a, 1);
  b_len_1 constant int := array_length(b, 1);
  b_len_2 constant int := array_length(b, 2);

  a_dims constant text := '['||a_lb::text||':'||a_ub::text||']';
  b_dims constant text := '['||b_lb_1::text||':'||b_ub_1::text||']'||
                          '['||b_lb_2::text||':'||b_ub_2::text||']';
begin
  -- Confirm that the supplied arrays meet the basic
  -- dimensionalities requirement.
  assert
    a_ndims = 1 and b_ndims = 2,
  'ndims assert failed';

  -- Confirm the user-supplied facts about the shape and size of "a".
  assert
    array_ndims(a) = a_ndims           and
    array_lower(a, 1) = a_lb           and
    array_upper(a, 1) = a_ub           ,
  '"a" dimensions assert failed';

  -- Confirm the user-supplied facts about the shape and size of "b".
  assert
    array_ndims(b) = b_ndims           and
    array_lower(b, 1) = b_lb_1         and
    array_upper(b, 1) = b_ub_1         and
    array_lower(b, 2) = b_lb_2         and
    array_upper(b, 2) = b_ub_2         ,
  '"b" dimensions assert failed';

  -- Confirm the length overspecification rule.
  assert
    (a_ub   - a_lb + 1  ) = a_len and
    (b_ub_1 - b_lb_1 + 1) = b_len_1 and
    (b_ub_2 - b_lb_2 + 1) = b_len_2 ,
  'Length overspecification rule assert failed.';

  -- Confirm the cardinality overspecification rule.
  assert
    cardinality(a) = a_len             and
    cardinality(b) = b_len_1 * b_len_2 ,
  'Cardinality overspecification rule assert failed.';

  -- Confirm the "dims" overspecification rule.
  assert
    array_dims(a) = a_dims             and
    array_dims(b) = b_dims             ,
  '"dims" overspecification rule assert failed.';

  -- Do the row-major order traversal and
  -- check that the values are pairwise-identical. 
  for j in 0..(a_len - 1) loop
    declare
      -- Traversing a 1-d array is trivial.
      a_idx   constant int := j + a_lb;

      -- Traversing a 2-d array is need a bit more thought.
      b_idx_1 constant int := floor(j/b_len_2)          + b_lb_1;
      b_idx_2 constant int := ((j + b_len_2) % b_len_2) + b_lb_2;

      a_txt   constant text := lpad(a_idx::text,    2);
      b_txt_1 constant text := lpad(b_idx_1::text,  2);
      b_txt_2 constant text := lpad(b_idx_2::text,  2);
      val     constant text := lpad(a[a_idx]::text, 2);

      line constant text := 
        'a['||a_txt||'] = '||
        'b['||b_txt_1||']['||b_txt_2||'] = '||
        val;
    begin
      assert
        a[a_idx] = b[b_idx_1][b_idx_2],
      'Row-major order pairwise equality assert failed';
      raise info '%', line;
    end;
  end loop;
end;
$body$;
```

Try it on the first data set:
```plpgsql
do $body$
declare
  a constant int[] :=            '{ 1, 2,   3,  4,   5,  6}';
  b constant int[] :=            '{{1, 2}, {3,  4}, {5,  6}}';

  a_ndims constant int  := 1;
  a_lb    constant int  := 1;
  a_ub    constant int  := 6;

  b_ndims constant int  := 2;
  b_lb_1  constant int  := 1;
  b_ub_1  constant int  := 3;
  b_lb_2  constant int  := 1;
  b_ub_2  constant int  := 2;
begin
  call assert_semantics_and_traverse_values(
    a, b,
    a_ndims, a_lb,   a_ub,
    b_ndims, b_lb_1, b_ub_1,
             b_lb_2, b_ub_2);
end;
$body$;
```
It produces this result (after manually removing the _"INFO:"_ prompts):
```
a[ 1] = b[ 1][ 1] =  1
a[ 2] = b[ 1][ 2] =  2
a[ 3] = b[ 2][ 1] =  3
a[ 4] = b[ 2][ 2] =  4
a[ 5] = b[ 3][ 1] =  5
a[ 6] = b[ 3][ 2] =  6
```
Try it on the second data set:
```plpgsql
do $body$
declare
  a constant int[] :=     '[3:14]={ 1, 2, 3,   4, 5, 6,   7, 8, 9,   10, 11, 12}';
  b constant int[] := '[3:6][6:8]={{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}}';

  a_ndims constant int  := 1;
  a_lb    constant int  := 3;
  a_ub    constant int  := 14;

  b_ndims constant int  := 2;
  b_lb_1  constant int  := 3;
  b_ub_1  constant int  := 6;
  b_lb_2  constant int  := 6;
  b_ub_2  constant int  := 8;
begin
  call assert_semantics_and_traverse_values(
    a, b,
    a_ndims, a_lb,   a_ub,
    b_ndims, b_lb_1, b_ub_1,
             b_lb_2, b_ub_2);
end;
$body$;
```
It produces this result:
```
a[ 3] = b[ 3][ 6] =  1
a[ 4] = b[ 3][ 7] =  2
a[ 5] = b[ 3][ 8] =  3
a[ 6] = b[ 4][ 6] =  4
a[ 7] = b[ 4][ 7] =  5
a[ 8] = b[ 4][ 8] =  6
a[ 9] = b[ 5][ 6] =  7
a[10] = b[ 5][ 7] =  8
a[11] = b[ 5][ 8] =  9
a[12] = b[ 6][ 6] = 10
a[13] = b[ 6][ 7] = 11
a[14] = b[ 6][ 8] = 12
```
Try it on the third data set:
```plpgsql
do $body$
declare
  a constant int[] :=     '[3:18]={ 1, 2, 3, 4,   5, 6, 7, 8,   9, 10, 11, 12,   13, 14, 15, 16}';
  b constant int[] := '[3:6][2:5]={{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}}';

  a_ndims constant int  := 1;
  a_lb    constant int  := 3;
  a_ub    constant int  := 18;

  b_ndims constant int  := 2;
  b_lb_1  constant int  := 3;
  b_ub_1  constant int  := 6;
  b_lb_2  constant int  := 2;
  b_ub_2  constant int  := 5;
begin
  call assert_semantics_and_traverse_values(
    a, b,
    a_ndims, a_lb,   a_ub,
    b_ndims, b_lb_1, b_ub_1,
             b_lb_2, b_ub_2);
end;
$body$;
```
It produces this result:
```
a[ 3] = b[ 3][ 2] =  1
a[ 4] = b[ 3][ 3] =  2
a[ 5] = b[ 3][ 4] =  3
a[ 6] = b[ 3][ 5] =  4
a[ 7] = b[ 4][ 2] =  5
a[ 8] = b[ 4][ 3] =  6
a[ 9] = b[ 4][ 4] =  7
a[10] = b[ 4][ 5] =  8
a[11] = b[ 5][ 2] =  9
a[12] = b[ 5][ 3] = 10
a[13] = b[ 5][ 4] = 11
a[14] = b[ 5][ 5] = 12
a[15] = b[ 6][ 2] = 13
a[16] = b[ 6][ 3] = 14
a[17] = b[ 6][ 4] = 15
a[18] = b[ 6][ 5] = 16
```
