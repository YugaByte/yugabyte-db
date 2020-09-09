---
title: Array comparison
linkTitle: Array comparison
headerTitle: Operators for comparing two arrays
description: Operators for comparing two arrays
menu:
  latest:
    identifier: array-comparison
    parent: array-functions-operators
    weight: 20
isTocNested: true
showAsideToc: true
---

## Comparison operators overview

**Purpose:** Each of the comparison operators returns `TRUE` or `FALSE` according to the outcome of the particular comparison test between the input [LHS and RHS](https://en.wikipedia.org/wiki/Sides_of_an_equation) arrays.

**Signature**

These operators all have the same signature, thus:

```
input value:       anyarray, anyarray
return value:      boolean
```
**Note:** These operators require that the LHS and RHS arrays have the same data type. (It's the same rule for the comparison of scalars.) However, they do _not_ require that the arrays have identical geometric properties. Rules are defined so that a difference between one or more of these properties does not mean that comparison is disallowed. Rather, the LHS array might be deemed to be less than, or greater than, the RHS array. It's essential, therefore, to understand the comparison algorithm.

### Comparison criteria

These are the unique characteristics of an array with respect to the algorithm that compares two array values:

- the actual values, compared pairwise in row-major order
- the cardinality
- the number of dimensions
- the lower bound on each dimension.

The term _"row-major order"_ is explained in [Joint semantics](../properties/#joint-semantics) within the section _"Functions for reporting the geometric properties of an array"_.

The other geometric properties (the length and upper bound along each dimension) can be derived from the properties that the bullets list..

There is, of course, a well-defined priority among the comparisons. Briefly, value comparison is done first. Then, but only if no difference is detected, are the geometric properties compared.


### Pairwise comparison of values

The first comparison test scans the values in each of the LHS and RHS arrays in row-major order (see [Joint semantics](../functions-operators/properties/#joint-semantics)) and does a pairwise comparison. Notably, the comparison rule non-negotiably uses `IS NOT DISTINCT FROM` semantics. Moreover, when a `not null` array value is pairwise compared with a `NULL` value, the `not null` value is deemed to be _less than_ the `NULL` value.

Notice the contrast with the `=` operator comparison rule for free-standing scalar values. This comparison uses `NULL` semantics but, of course, lets you use `IS NOT DISTINCT FROM` comparison if this better suits your purpose. 

Otherwise, the comparison rules are the same as those for scalar values and, by extension, with those for, for example, _"row"_ type values.

If a pairwise comparison results in inequality, then the LHS and RHS arrays are immediately deemed to be unequal with no regard to the geometric properties. The outcome of the _first_ pairwise comparison, when these values differ, determines the outcome of the array comparison. Remaining pairwise comparisons are not considered.

Notice that the two arrays might not have the same cardinality. If all possible pairwise comparisons result in equality, then the array with the greater cardinality is deemed to be greater than the other array, and the other geometric properties are not considered.

### The priority of differences among the geometric properties

The previous section stated the rule that the cardinality comparison has the highest priority among the geometric property comparisons. And that this rule kicks in only of all possible value comparisons result in equality.

When _both_ all possible value comparisons _and_ the cardinality comparison result in equality, then the comparison between the number of dimensions has a higher priority than the comparison between the lower bound on each dimension. Of course, the array with the greater number of dimensions is deemed to be the greater array.

This means that the lower bounds are significant when two arrays are compared _only_ when they are identical in pairwise value comparison, cardinality, and the number of dimensions. Then the array with the greater lower bound, in dimension order, is deemed to be the greater array.

[Equality and inequality semantics](./#equality-and-inequality-semantics) demonstrates each of the rules that this _"Comparison operators overview"_  section has stated.

## Containment and overlap operators overview

These three operators are insensitive to the geometric properties of the two to-be-compared arrays.
- The two containment operators test if the distinct set of values in one array contains, or is contained by, the distinct set of values in the other array.
- The overlap operator tests if the distinct set of values in one array and the distinct set of values in the other array have at least one value in common.

[Containment and overlap operators semantics](./#containment-and-overlap-operators-semantics) below demonstrates each of the rules that this section has stated.

## Examples for each operator

### The&#160; &#160;=&#160; &#160;and&#160; &#160;<>&#160; &#160;operators

- The `=` operator returns `TRUE` if the LHS and RHS arrays are equal.
- The `<>` operator is the natural complement: it returns `TRUE` if the LHS and RHS arrays are not equal.

```plpgsql
with
  v as (
    select
      (select array['a', 'b', null, 'd']::text[]) as a1,
      (select      '{a,   b,  null,  d}'::text[]) as a2
  )
select (a1 = a2)::text as "EQUALITY comparison result"
from v;
```
This is the result:
```
 EQUALITY comparison result 
----------------------------
 true
```

```plpgsql
with
  v as (
    select
      (select array['a', 'b', 'c',  'd']::text[]) as a1,
      (select      '{a,   b,  null,  d}'::text[]) as a2
  )
select (a1 <> a2)::text as "INEQUALITY comparison result"
from v;
```
This is the result:
```
 INEQUALITY comparison result
------------------------------
 true
```

### The&#160; &#160;>&#160; &#160;and&#160; &#160;>=&#160; &#160;and&#160; &#160;<=&#160; &#160;and&#160; &#160;<&#160; &#160;and&#160; &#160;<>&#160; &#160;operators

These four operators implement the familiar inequality comparisons.
- The `>` operator returns `TRUE` if the LHS array is greater than the RHS array.
- The `>=` operator returns `TRUE` if the LHS array is greater than or equal to the RHS array.
- The `<=` operator returns `TRUE` if the LHS array is less than or equal to the RHS array.
- The `<` operator returns `TRUE` if the LHS array is less than the RHS array.

It's sufficient, therefore, to provide an example for just the `<` operator.
```plpgsql
with
  v as (
    select
      (select array['a', 'b', 'c',  'd']::text[]) as a1,
      (select array['a', 'b', 'e',  'd']::text[]) as a2,
      (select      '{a,   b,  null,  d}'::text[]) as a3
  )
select
  (a1 < a2)::text as "'LESS THAN' comparison result 1",
  (a1 < a3)::text as "'LESS THAN' comparison result 2"
from v;
```
This is the result:
```
 'LESS THAN' comparison result 1 | 'LESS THAN' comparison result 2 
---------------------------------+---------------------------------
 true                            | true
```

### The&#160; &#160;@>&#160; &#160;and&#160; &#160;<@&#160; &#160;operators

- The `@>` operator returns `TRUE` if the LHS array contains the RHS array—that is, if every distinct value in the RHS array is found among the LHS array's distinct values.
- The `<@` operator is the natural complement: it returns `TRUE` if every distinct value in the LHS array is found among the RHS array's distinct values.

```plpgsql
with
  v as (
    select
      (select array['a', 'b', 'c',  'd']::text[]) as a1,
      (select array['a',      'c'      ]::text[]) as a2
  )
select
  (a1 @> a2)::text as "CONTAINS comparison result",
  (a2 <@ a1)::text as "'IS CONTAINED BY' comparison result"
from v;
```
This is the result:
```
 CONTAINS comparison result | 'IS CONTAINED BY' comparison result 
----------------------------+-------------------------------------
 true                       | true
```

### The&#160; &#160;&&&#160; &#160;operator

The `&&` operator returns `TRUE` if the LHS and RHS arrays overlap—that is, if they have at least one value in common. The definition of this operator makes it insensitive to which of the two to-be-compared is used on the LHS and which is used on the RHS.

```plpgsql
with
  v as (
    select
      (select array['a', 'b', 'c',  'd']::text[]) as a1,
      (select array['d', 'e', 'f',  'g']::text[]) as a2
  )
select
  (a1 && a2)::text as "'a1 OVERLAPS a2' comparison result",
  (a2 && a1)::text as "'a2 OVERLAPS a1' comparison result"
from v;
```
This is the result:
```
 'a1 OVERLAPS a2' comparison result | 'a2 OVERLAPS a1' comparison result 
------------------------------------+------------------------------------
 true                               | true
```

## Equality and inequality semantics

This section demonstrates each of the rules that [Comparison operators overview](./#comparison-operators-overview) above stated.

```plpgsql
-- Any two arrays can be compared without error if they have the same data type.
do $body$
begin
  ------------------------------------------------------------------------------
  -- Illustrate "IS NOT DISTINCT FROM" semantics.
  declare
    v1 constant int := 1;
    v2 constant int := 1;
    n1 constant int := null;
    n2 constant int := null;
  begin
    assert
      (v1 = v2)                    and
      (v1 is not distinct from v2) and

      ((n1 = n2) is null)          and
      (n1 is not distinct from n2),
    'unexpected';
  end;

  ------------------------------------------------------------------------------
  -- Basic demonstration of equaliy when the geom. properties of
  -- the two arrays are identical.
  -- Shows that pairwise comparison uses "IS NOT DISTINCT FROM" semantics and NOT
  -- the conventional NULL semantics used when scalars are compared.
  declare
    a constant int[] := '{10, null, 30}';
    b constant int[] := '{10, null, 30}'; -- Identical to a.
  begin
    assert
      (a = b),
    '"a = b" assert failed';

    -- Because of this, there's no need ever to write this.
    assert
      (a is not distinct from b),
    '"a is not distinct from b" assert failed';
  end;

  ------------------------------------------------------------------------------
  -- Basic demonstration of inequality when the geometric properties of
  -- the two arrays are identical.
  -- When the first difference is encountered in row-major order, the comparison
  -- is made. Other differences are irrelevant.
  declare
    a constant int[] := '{10, 20, 30}';
    b constant int[] := '{10, 19, 31}'; 
  begin
    assert
      (a <> b) and
      (a >  b) and
      (a >= b) and
      (b <= a) and
      (b <  a) ,
    '"a > b" assert failed';
  end;

  ------------------------------------------------------------------------------
  -- Demonstration of inequality when the geometric properties of
  -- the two arrays are identical.
  -- Here, the first pairwise difference is NOT NULL versus NULL.
  declare
    a constant int[] := '{10, 20,   30}';
    b constant int[] := '{10, null, 29}';
  begin
    -- Bizarrely, a NOT NULL value counts as LESS THAN a NULL value in the
    -- pairwise comparison.
    assert
      (a <> b) and
      (a <  b),
    '"a < b" assert failed';

    -- Again, because of this, there's no need ever to write this.
    assert
      (a is distinct from b) ,
    '"a is distinct from b" assert failed';
  end;

  ------------------------------------------------------------------------------
  -- Extreme demonstration of priority.
  -- c has just a single value and d has several.
  -- c has one dimension and d has two.
  -- c's first lower bound is less than d's first lower.
  -- d's second lower bound is greater than one, but is presumably irrelevant.
  -- But c's first value is GREATER THAN d's first value,
  -- scanning in row-major order.
  --
  -- Pairwise value comparison has the hoghest priority.
  -- therefore c is deemed to be GREATER THAN d.
  
  declare
    c constant int[] := '{2}';

    -- Notice that d's first value is at [2][3].
    d constant int[] := '[2:3][3:4]={{1, 2}, {3, 3}}';

  begin
    assert
      cardinality(c) < cardinality(d),
    '"cardinality(c) < cardinality(d)" assert failed';

    assert
      array_ndims(c) < array_ndims(d),
    '"ndims(c) < ndims(d)" assert failed';
    assert
      array_lower(c, 1) < array_lower(d, 1),
    '"lower(c, 1) < lower(d, 1)" assert failed';

    assert
      c[1] > d[2][3],
    '"c[1] > d[2][3]" assert failed';

   assert
     c > d,
   '"c > d" assert failed';
  end;

  ------------------------------------------------------------------------------
  -- Pairwise comparison is equal are far as it is feasible.
  -- e's ndims < f's.
  -- e's lb-1 < f's.
  -- BUT e's cardinality > f's.
  -- Cardinality has highest priority among the geom. propoerties,
  -- so e is deemed to be GREATER THAN f.
  declare
    e constant int[] := '{10, 20, 30, 40, 50, 60, 70}';
    f constant int[] := '[2:3][3:5]={{10, 20, 30}, {40, 50, 60}}'; 
  begin
    assert
      e[1] = f[2][3] and
      e[2] = f[2][4] and
      e[3] = f[2][5] and
      e[4] = f[3][3] and
      e[5] = f[3][4] and
      e[6] = f[3][5] ,
    '"e-to-f" eqality test, as far as feasible, assert failed';

    assert
      array_ndims(e) < array_ndims(f),
    '"ndims(e) < ndims(f)" assert failed';

    assert
      array_lower(e, 1) < array_lower(f, 1),
    '"lower(e, 1) < lower(f, 1)" assert failed';

    assert
      cardinality(e) > cardinality(f),
    '"cardinality(e) > cardinality(f)" assert failed';

    assert
      (e > f) ,
    'e > f assert failed';
  end;

  ------------------------------------------------------------------------------
  -- g's cardinality = h's.
  -- So pairwise comparison is feasible for all values, and is equal.
  -- g's ndims > h's.
  -- g's lb-1 < h's.
  -- Ndims has higher priority among ndims and lower bounds,
  -- so g is deemed to be GREATER THAN h.
  declare
    g constant int[] := '{{10, 20, 30}, {40, 50, 60}}'; 
    h constant int[] := '[2:7]={10, 20, 30, 40, 50, 60}';
  begin
    assert
      cardinality(g) = cardinality(h),
    '"cardinality(g) = cardinality(h)" assert failed';

    assert
      g[1][1] = h[2] and
      g[1][2] = h[3] and
      g[1][3] = h[4] and
      g[2][1] = h[5] and
      g[2][2] = h[6] and
      g[2][3] = h[7] ,
    '"g-to-h" eqality test assert failed';

    assert
      array_ndims(g) > array_ndims(h),
    '"ndims(g) > ndims(h)" assert failed';

    assert
      array_lower(g, 1) < array_lower(h, 1),
    '"lower(g, 1) < lower(h, 1)" assert failed';

    assert
      (g > h) ,
    '"g > h" assert failed';
  end;

  ------------------------------------------------------------------------------
  declare
    i constant int[] := '[5:6][4:6]={{10, 20, 30}, {40, 50, 60}}'; 
    j constant int[] := '[3:4][6:8]={{10, 20, 30}, {40, 50, 60}}';
  begin
    assert
      cardinality(i) = cardinality(j),
    '"cardinality(i) = cardinality(j)" assert failed';

    assert
      i[5][4] = j[3][6] and
      i[5][5] = j[3][7] and
      i[5][6] = j[3][8] and
      i[6][4] = j[4][6] and
      i[6][5] = j[4][7] and
      i[6][6] = j[4][8] ,
    '"i-to-j" eqality test assert failed';

    assert
      array_ndims(i) = array_ndims(j),
    '"ndims(i) = ndims(j)" assert failed';

    assert
      array_lower(i, 1) > array_lower(j, 1),
    '"lower(i, 1) > lower(j, 1)" assert failed';

    assert
      (i > j) ,
    '"i > j" assert failed';
  end;

  ------------------------------------------------------------------------------
end;
$body$;
```

## Containment and overlap operators semantics

This section demonstrates each of the rules that [Containment and overlap operators overview](./#containment-and-overlap-operators-overview) stated.

```plpgsql
-- Any two arrays can be compared without error if they have the same data type.
-- Insensitive to the geometric properties.
do $body$
declare
  a constant int[] := '[2:3][4:5]={{10, 20}, {30, 40}}';
  b constant int[] := '[5:6]={20, 30}';
  c constant int[] := '[6:9]={40, 50, 70, 70}';
  d constant int[] := '[2:4]={50, 60, 70}';
begin
  assert
    -- Containment
    (b @> b) and
    (b <@ a) and

    -- Overlap.
    -- The definition of the semantics makes the LHS, RHS order immaterial.
    (a && c) and
    (c && a) and

    -- a and d have NO values in common.
    not (a && d),
  'unexpected';
end;
$body$;
```
