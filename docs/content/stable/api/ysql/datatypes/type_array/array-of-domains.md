---
title: Using an array of DOMAIN values
linkTitle: array of DOMAINs
headerTitle: Using an array of DOMAIN values
description: Using an array of DOMAIN values
block_indexing: true
menu:
  stable:
    identifier: array-of-domains
    parent: api-ysql-datatypes-array
    weight: 40
isTocNested: true
showAsideToc: true
---
An array of `DOMAIN` values lets you create, for example, a one-dimensional array whose values are themselves one-dimensional arrays of _different_ lengths. Stating this generally, it lets you implement a ragged multidimensional array, thereby overcoming the restriction that an array must normally be rectilinear. It meets other use cases too.

**Note:** The understanding of this section depends on the principles that are established in these sections:

- [Array data types and functionality](../../type_array/)

- [The `array[]` value constructor](../array-constructor/)

- [Creating an array value using a literal](../literals/)

## Example use case: GPS trip data (revisited)

There are use cases for which a ragged structure is essential. Most programming languages, therefore, have constructs that support this.

Look at [Example use case: GPS trip data](../../type_array/#example-use-case-gps-trip-data). It considers the representation of the GPS trips whose recording is broken up into laps, thus:

- Each trip is made up of one or many laps.
- Each lap is typically made up of a large number GPS data points.

The representation that was explained in that section met a modest ambition level:

- Each lap was represented as a row in the _"laps"_ table" that had a multivalued field implemented as an array of GPS data points.
- But each trip was represented classically by a row in the _"trips"_ table whose set of laps were child rows in the _"laps"_ table. The master-child relationship was supported ordinarily by a foreign key constraint. Notice that each lap has a different number of GPS points from other laps.

The next level of ambition dispenses with the separate _"laps"_ table by representing the entire trip  as a ragged array of arrays in the _"trips"_ table. This scheme requires the ability to represent a trip as an array of the _"GPS data point array"_ data type—in other words, it depends on the ability to create such a named data type. The `DOMAIN` brings this ability.

## Creating a ragged array of arrays

### The apparent paradox

The syntax for defining a table column, or a PL/pgSQL variable or formal parameter, seems at first glance to present a paradox that thwarts the goal. For example, this PL/pgSQL declaration defines _"v"_ as, apparently, a one-dimensional array.

```
declare
  v int[];
```
However, this executable section first assigns a two-dimensional array value (with size three-by-three) to _"v"_, setting each of the array's values initially to `17`. Then it assigns a three-dimensional array value (with size two-by-two-by-two) to _"v"_, setting each of the array's values initially to `42`:
```
begin
  v := array_fill(17, '{3, 3}');
  ...
  v := array_fill(42, '{2, 2, 2}');
```
See [`array_fill()`](..//functions-operators/array-fill/). 

The property of the declaration of an array variable that it cannot fix the dimensionality of a value that is subsequently assigned to the variable was pointed out in [Array data types and functionality](../../type_array/). A column in a table with an array data type shares this property so that the column  can hold arrays of different dimensionality in different rows. This goes hand-in-hand with the fact that the following declarations of _"v1"_ and "_v2"_, though apparently different, define identical semantics.

```
declare
  v1 int[];
  v2 int[][];
```

This syntactical quirk pinpoints the paradox. Is _"v2"_ a two-dimensional array or an array of the type `int[]`? The answer is it's neither. Rather, it's an array of _any_ dimensionality, and as such it is _rectilinear_.

How then _would_ you write the declaration of an array of arrays  You might guess that this would work:
```
declare
  v (int[])[];
```

But this causes a compilation error. The paradox is exactly that `int[]` is anonymous.

### The solution

The `DOMAIN` brings the functionality that overcomes the apparent restriction. First, do this:

```plpgsql
set client_min_messages = warning;
drop domain if exists int_arr_t cascade;
create domain int_arr_t as int[];
create table t(k serial primary key, v1 int_arr_t[], typecast text, v2 int_arr_t[]);
```
Notice that the use of the `CREATE DOMAIN` statement is an example of _type construction_. And the user-defined data type _"int_arr_t"_ is an example of such a constructed data type.

The columns _"v1"_ and _"v2"_ are now ready to store ragged arrays of arrays. Prove it like this:

```plpgsql
do $body$
declare
  arr_1      constant int_arr_t := array[1, 2];
  arr_2      constant int_arr_t := array[3, 4, 5];
  ragged_arr constant int_arr_t[] := array[arr_1, arr_2];
begin
  insert into t(v1) values(ragged_arr);
end;
$body$;
```
By using a `DO` block to set the value of _"ragged_arr"_ by building it bottom-up, you emphasize the fact that it really is a one-dimensional array of one-dimensional arrays of different lengths. It is, then, clearly _not_ a rectilinear two-dimensional array.

Now use the technique that [The non-lossy round trip: value to text typecast and back to value](../literals/text-typecasting-and-literals/#the-non-lossy-round-trip-value-to-text-typecast-and-back-to-value) explained to inspect the `::text` typecast of the ragged array and then to show that, by typecasting this back to a value of the original data type, it can serve as the literal for the original value. First, do this:
```plpgsql
update t
set typecast = v1::text
where k = 1;

select typecast from t where k = 1;
```
This is the result:

```
      typecast       
---------------------
 {"{1,2}","{3,4,5}"}
```

This sentence is copied from [The non-lossy round trip: value to text typecast and back to value](..//literals/text-typecasting-and-literals/#the-non-lossy-round-trip-value-to-text-typecast-and-back-to-value):

> Notice how the syntax for the _array of arrays_ `text` value compares with the syntax for the _2-d array_ `text` value. Because the _array of arrays_ is ragged, the two inner `{}` pairs contain respectively two and three values. To distinguish between this case and the ordinary rectilinear case, the inner `{}` pairs are surrounded by double quotes.

Now do this:

```plpgsql
update t
set v2 = typecast::int_arr_t[]
where k = 1;

select (v1 = v2)::text as "v1 = v2" from t where k = 1;
```
This is the result:

```
 v1 = v2 
---------
 true
```
The original value has been recreated.

### Addressing values in a ragged array of arrays

First, consider this counter example:
```plpgsql
\pset null '<IS NULL'>
with v as (
  select  '{{1,2},{3,4}}'::int[] as two_d_arr)
select
  two_d_arr[2][1] as "[2][1] -- meaningful",
  two_d_arr[2]    as    "[2] -- meaningless"     
from v;
```
This is the result:

```
 [2][1] -- meaningful | [2] -- meaningless 
----------------------+--------------------
                    3 |          <IS NULL>
```

This reminds you how to address a single value in a rectilinear multidimensional array: you use this general scheme:

```
[idx_1][idx_2]...[idx_n]
```
And it reminds you that you must supply exactly as many index values as the array has dimensions. Further, it shows you that if you do this wrong, and supply too many or too few index values, then you don't see an error but, rather, silently get `NULL`. This reminder prompts the obvious question:

- How do you address, for example, the _first_ value in the array that is itself the _second_ array in the ragged array of arrays?

You know before typing it that this can't be right:
```plpgsql
select v1[2][1] as "v1[2][1]" from t where k = 1;
```
Sure enough, it shows this:
```
 v1[2][1]  
-----------
 <IS NULL>
```
You don't get an error. But neither do you get what you want. Try this instead:
```plpgsql
select v1[2] as "v1[2]" from t where k = 1;
```
This is the result:
```
  v1[2]  
---------
 {3,4,5}
```
This is the clue. You have identified the leaf array, in the array of arrays, that you want to. Now you have to identify the desired value in _that_ array. Try this:
```plpgsql
select (v1[2])[1] as "(v1[2])[1]" from t where k = 1;
```
This is the result:
```
 (v1[2])[1] 
------------
          3
```
In the same way, you can inspect the geometric properties of the leaf array like this:
```plpgsql
select
  array_lower(v1[1], 1) as v1_lb,
  array_upper(v1[1], 1) as v1_ub,
  array_lower(v1[2], 1) as v2_lb,
  array_upper(v1[2], 1) as v2_ub
from t where k = 1;
```
This is the result:
```
 v1_lb | v1_ub | v2_lb | v2_ub 
-------+-------+-------+-------
     1 |     2 |     1 |     3
```
Finally, try this counter example:

```plpgsql
with v as (
  select  '{{1,2},{3,4}}'::int[] as two_d_arr)
select
  (two_d_arr[2])[1]
from v;
```
It causes a SQL compilation error. You must know whether the value at hand, within which you want to address a value,  is a rectilinear multidimensional array or a ragged array of arrays.

### Using FOREACH with an array of DOMAINs

This example demonstrates the problem.

```plpgsql
set client_min_messages = warning;
drop domain if exists array_t cascade;
drop domain if exists arrays_t cascade;

create domain array_t  as int[];
create domain arrays_t as array_t[];

\set VERBOSITY verbose
do $body$
declare
  arrays arrays_t := array[
    array[1, 2]::array_t, array[3, 4, 5]::array_t];

  runner array_t not null := '{}';
begin
  -- Error 42804 here.
  foreach runner in array arrays loop
    raise info '%', runner::text;
  end loop;
end;
$body$;
```
It causes this syntax error:

```
42804: FOREACH expression must yield an array, not type arrays_t
```
The error text might confuse you. It really means that the argument of the `ARRAY` keyword in the loop header must, literally, be an explicit array—and _not_ a domain that names such an array.

A simple workaround is to declare _"arrays"_ explicitly as
_"array_t[]"_ rather that use _"arrays_t"_ as a shorthand for this.

```plpgsql
\set VERBOSITY default
do $body$
declare
  arrays array_t[] := array[
    array[1, 2]::array_t, array[3, 4, 5]::array_t];

  runner array_t not null := '{}';
begin
  foreach runner in array arrays loop
    raise info '%', runner::text;
  end loop;
end;
$body$;
```

It shows this:

```
INFO:  {1,2}
INFO:  {3,4,5}
```

What if you really _do_ need to use a `DOMAIN`? For example, you might want to define a constraint like this:

```plpgsql
set client_min_messages = warning;
drop domain if exists arrays_t cascade;
create domain arrays_t as array_t[]
check ((cardinality(value) = 2));
```

The workaround is to typecast the argument of the `ARRAY` keyword _in situ_ to an array of the appropriate element data type.

```plpgsql
do $body$
declare
  arrays arrays_t := array[
    array[1, 2]::array_t, array[3, 4, 5]::array_t];

  runner array_t not null := '{}';
begin
  foreach runner in array arrays::array_t[] loop
    raise info '%', runner::text;
  end loop;
end;
$body$;
```
Once again, it shows this:

```
INFO:  {1,2}
INFO:  {3,4,5}
```

### Using array_agg() to produce an array of DOMAIN values

See [array_agg()](../functions-operators/array-agg-unnest/#array-agg-first-overload). It turns out that directly aggregating `DOMAIN` values that represent a ragged array is not supported. But a simple PL/pgSQL function provides the workaround. This sets up to demonstrate the problem:

```plpgsql
set client_min_messages = warning;
drop table if exists t cascade;
drop domain if exists array_t cascade;
drop domain if exists arrays_t cascade;

create domain array_t  as int[];
create domain arrays_t as array_t[];

create table t(k serial primary key, v array_t);

insert into t(v) values
  ('{2,6}'),
  ('{1,4,5,6}'),
  ('{4,5}'),
  ('{2,3}'),
  ('{4,5}'),
  ('{3,5,7}');

select v from t order by k;
```
It shows the raggedness thus:

```
     v     
-----------
 {2,6}
 {1,4,5,6}
 {4,5}
 {2,3}
 {4,5}
 {3,5,7}
```

And this demonstrates the problem:

```plpgsql
\set VERBOSITY verbose
select
  array_agg(v order by k)
from t;
```

It causes this error:

```
2202E: cannot accumulate arrays of different dimensionality
```

Typecasting cannot come to the rescue here. But this function produces the required result:

```plpgsql
\set VERBOSITY default
create or replace function array_agg_v()
  returns arrays_t
  immutable
  language plpgsql
as $body$
<<b>>declare
  v  array_t    not null := '{}';
  n  int        not null := 0;
  r  array_t[]  not null := '{}';
begin
  for b.v in (select t.v from t order by k) loop
    n := n + 1;
    r[n] := b.v;
  end loop;
  return r;
end b;
$body$;
```
Do this:

```plpgsql
select array_agg_v();
```

This is the result:

```
                       array_agg_v                       
---------------------------------------------------------
 {"{2,6}","{1,4,5,6}","{4,5}","{2,3}","{4,5}","{3,5,7}"}
```

## Creating a matrix of matrices

The Wikipedia article entitled [Matrix (mathematics)](https://en.wikipedia.org/wiki/Matrix_(mathematics)) defines the term _"matrix"_ like this:

> a matrix... is a rectangular array... of numbers, symbols, or expressions, arranged in rows and columns.

Look for the heading [Matrices with more general entries](https://en.wikipedia.org/wiki/Matrix_%28mathematics%29#Matrices_with_more_general_entries) and in particular for this sentence:

> One special but common case is block matrices, which may be considered as matrices whose entries themselves are matrices.

Various disciplines in mathematics, physics, and the like, use block matrices. [Uses of arrays](../../type_array/#uses-of-arrays) explains how such cases generate various kinds of arrays in client-side programs and need to use these values later, again in client-side programs. This brings the requirement, in the present use case, to persist and to retrieve block matrices.

Though this use case is relatively exotic, the techniques that are used to implement the required structures (and in particular, the dependency of a viable solution upon user-defined `DOMAIN` data types) are of general utility. Its for this reason that the approach is explained here.

### Defining the required data types

First, define the data type for the "payload" matrix. To make it interesting, assume that the following rules need to be enforced:
- The payload matrix must not be atomically null.
- By definition, it must be two-dimensional.
- It must be exactly three-by-three.
- The lower bound along each dimension must be `1`.
- Each of the matrix's values must be `NOT NULL`.

The motivating requirement for the `DOMAIN` type constructor is that it must allow arbitrary constraints to be defined on values of the constructed type, like this:
```plpgsql
create domain matrix_t as text[]
check (
  (value is not null)         and
  (array_ndims(value) = 2)    and
  (array_lower(value, 1) = 1) and
  (array_lower(value, 2) = 1) and
  (array_upper(value, 1) = 3) and
  (array_upper(value, 2) = 3) and
  (value[1][1] is not null  ) and
  (value[1][2] is not null  ) and
  (value[1][3] is not null  ) and
  (value[2][1] is not null  ) and
  (value[2][2] is not null  ) and
  (value[2][3] is not null  ) and
  (value[3][1] is not null  ) and
  (value[3][2] is not null  ) and
  (value[3][3] is not null  )
);
```
Next, define the block matrix as a matrix of _"matrix_t"_. Assume that similar following rules need to be enforced:

- The block matrix must not be atomically null.
- By definition, it must be two-dimensional.
- It must be exactly two-by-two.
- The lower bound along each dimension must be `1`.
- Each of the matrix's values must be `NOT NULL`.

The `CREATE DOMAIN` statement for _"block_matrix_t"_ is therefore similar to that for _"matrix_t"_:
```plpgsql
create domain block_matrix_t as matrix_t[]
check (
  (value is not null)         and
  (array_ndims(value) = 2)    and
  (array_lower(value, 1) = 1) and
  (array_lower(value, 2) = 1) and
  (array_upper(value, 1) = 2) and
  (array_upper(value, 2) = 2) and
  (value[1][1] is not null  ) and
  (value[1][2] is not null  ) and
  (value[2][1] is not null  ) and
  (value[2][2] is not null  )
);
```
These two `CREATE DOMAIN` statements are uncomfortably verbose and repetitive. But they are sufficient to illustrate the basis of the approach. It would be better, for use in a real application, to encapsulate all the `CHECK` rules in a PL/pgSQL function that takes the `DOMAIN` value as input and that returns a `boolean`, and to use this as a single `CHECK` predicate. The function could use the `array_lower()` and  `array_length()` functions to compute the ranges of two nested `FOR` loops to check that the array's individual values all satisfy the `NOT NULL` rule.

### Using the "block_matrix_t" DOMAIN

Next, create a block matrix value, insert it, and its `::text` typecast, into a table, and inspect the typecast's value.

```plpgsql
create table block_matrices_1(k int primary key, v block_matrix_t, text_typecast text);

do $body$
declare
  -- The definitions of the two domains imply "not null" constraints
  -- on each of the variables "matrix_t" and "block_matrix_t".
  m matrix_t       := array_fill('00'::text, array[3, 3], array[1, 1]);
  b block_matrix_t := array_fill(m,          array[2, 2], array[1, 1]);

  n int not null := 0;
  ms matrix_t[];
begin
  -- Define four matrix_t values so that, for readability of the result,
  -- the in-total 24 values are taken from an increasing dense series.
  for i in 1..4 loop
    for j in 1..3 loop
      for k in 1..3 loop
        n := n + 1;
        m[j][k] := ltrim(to_char(n, '09'));
      end loop;
    end loop;
    ms[i] := m;
  end loop;

  n := 0;
  for j in 1..2 loop
    for k in 1..2 loop
      n := n + 1;
      b[j][k] := ms[n];
    end loop;
  end loop;

  insert into block_matrices_1(k, v, text_typecast)
  values(1, b, b::text);
end;
$body$;

select text_typecast
from block_matrices_1
where k = 1;
```
This is the result (after manual whitespace formatting):
```
{
  {
    "{
      {01,02,03},     block_matrix[1][1]
      {04,05,06},
      {07,08,09}
    }",
    "{
      {10,11,12},     block_matrix[1][2]
      {13,14,15},
      {16,17,18}
    }"
  },
  {
    "{
      {19,20,21},     block_matrix[2][1]
      {22,23,24},
      {25,26,27}
    }",
    "{
      {28,29,30},     block_matrix[2][2]
      {31,32,33},
      {34,35,36}
    }"
  }
}
```
**Note:** The annotations _"block_matrix"_, and so on, are just that. Because they are _within_ the `text` value, they are part of that value and therefore render it illegal. They were added manually just to highlight the meaning of the overall `text` value.

Finally, check that even this exotic structure conforms to the universal rule, copied from [The non-lossy round trip: value to text typecast and back to value](..//literals/text-typecasting-and-literals/#the-non-lossy-round-trip-value-to-text-typecast-and-back-to-value):

> - Any value of any data type, primitive or composite, can be `::text` typecasted. Similarly, there always exists a `text` value that, when properly spelled, can be typecasted to a value of any desired data type, primitive or composite.
> - If you `::text` typecast a value of any data type and then typecast that `text` value to the original value's data type, then the value that you get is identical to the original value.

```plpgsql
create table block_matrices_2(k int primary key, v block_matrix_t);

insert into block_matrices_2(k, v)
select k, text_typecast::block_matrix_t
from block_matrices_1
where k = 1;

with a as (
  select k, t1.v as v1, t2.v as v2
  from 
  block_matrices_1 as t1
  inner join
  block_matrices_2 as t2
  using (k)
  )
select (v1 = v2)::text as "v1 = v2"
from a
where k = 1;
```
This is the result:
```
 v1 = v2 
---------
 true
```
The rule holds.

### Using unnest() on an array of arrays
First, produce the list of _"matrix_t"_ values, in row-major order:
```plpgsql
with matrices as (
  select unnest(v) as m
  from block_matrices_1
  where k = 1)
select
  row_number() over(order by m) as r,
  m
from matrices order by m;
```
See [`unnest()`](../functions-operators/array-agg-unnest/#unnest).

The term _"row-major order"_ is explained in [Joint semantics](../functions-operators/properties/#joint-semantics) within the section _"Functions for reporting the geometric properties of an array"_..

This is the result:

```
 r |                 m                  
---+------------------------------------
 1 | {{01,02,03},{04,05,06},{07,08,09}}
 2 | {{10,11,12},{13,14,15},{16,17,18}}
 3 | {{19,20,21},{22,23,24},{25,26,27}}
 4 | {{28,29,30},{31,32,33},{34,35,36}}
```
Now unnest a _"matrix_t"_ value of interest:
```plpgsql
select unnest(v[2][1]) as val
from block_matrices_1
where k = 1
order by val;
```
This is the result:
```
 val 
-----
 19
 20
 ...
 26
 27
```
Use this query if you want to see _all_ of the leaf values in row-major order:
```plpgsql
with
  matrixes as (
    select unnest(v) as m
    from block_matrices_1
    where k = 1),
  vals as (
    select unnest(m) as val
    from matrixes)
select
  row_number() over(order by val) as r,
  val
from vals
order by 1;
```
This is the result:
```
 r  | val 
----+-----
  1 | 01
  2 | 02
  3 | 03
 ...
 34 | 34
 35 | 35
 36 | 36
```

