---
title: Looping through arrays in PL/pgSQL
linkTitle: FOREACH loop (PL/pgSQL)
headerTitle: Looping through arrays in PL/pgSQL
description: Looping through arrays in PL/pgSQL
menu:
  latest:
    identifier: looping-through-arrays
    parent: api-ysql-datatypes-array
    weight: 30
isTocNested: true
showAsideToc: true
---
The PL/pgSQL `FOREACH` loop brings dedicated syntax for looping over the contents of an array.

## Overview

**Note:** See [array_lower()](../functions-operators/properties/#array-lower), [array_upper()](../functions-operators/properties/#array-upper), [array_ndims()](../functions-operators/properties/#array-ndims) and [cardinality()](../functions-operators/properties/#cardinality) for descriptions of the functions that the following account mentions. It also mentions _"row-major order"_. See [Joint semantics](../functions-operators/properties/#joint-semantics), within the section _"Functions for reporting the geometric properties of an array"_, for an explanation of this term. [Syntax and semantics](./#syntax-and-semantics) shows where, in the `FOREACH` loop header, the `SLICE` keyword is used.

- When the operand of the `SLICE` clause is `0`, and for the general case where the iterand array has any number of dimensions, YSQL assigns its successive values, in row-major order, to the loop iterator. Here, its effect is functionally analogous to that of [`unnest()`](../functions-operators/array-agg-unnest/#unnest).

- For the special case where the iterand array is one-dimensional, the `FOREACH` loop is useful only when the operand of the `SLICE` clause is `0`. In this use, it is a syntactically more compact way to achieve the effect that is achieved with a `FOR var IN` loop like this:

  ```
  for var in array_lower(iterand_arr, 1)..array_upper(iterand_arr, 1) loop
    ... iterand_arr[var] ...
  end loop;
  ```

- When the operand of the `SLICE` clause is greater than `0`, and when the dimensionality of the iterand array is greater than `1`, the `FOREACH` loop provides functionality that `unnest()` cannot provide. Briefly, when the iterand array has `n` dimensions and the operand of the `SLICE` clause is `s`, YSQL assigns _slices_ (that is, subarrays) of dimensionality `s` to the iterator. The values in such a slice are those from the iterand array that remain when the distinct values of the first `(n - s)` indexes are used to drive the iteration. These two pseudocode blocks illustrate the idea:

  ```
  -- In this example, the SLICE operand is 1.
  -- As a consequence, array_ndims(iterator_array) is 1.
  -- Assume that array_ndims(iterand_arr) is 4.
  -- There are therefore (4 - 1) = 3 nested loops in this pseudocode.
  for i in in array_lower(iterand_arr, 1)..array_upper(iterand_arr, 1) loop
    for j in in array_lower(iterand_arr, 2)..array_upper(iterand_arr, 2) loop
      for k in in array_lower(iterand_arr, 3)..array_upper(iterand_arr, 3) loop

        the (i, j, k)th iterator_array is set to
          iterand_arr[i][j][k][ for all values the remaining 4th index ]

      end loop;
    end loop;
  end loop;
  ```

  ```
  -- In this example, the SLICE operand is 3.
  -- As a consequence, array_ndims(iterator_array) is 3.
  -- Assume that array_ndims(iterand_arr) is 4.
  -- There is therefore (4 - 3) = 1 nested loop in this pseudocode.
  for i in in array_lower(iterand_arr, 1)..array_upper(iterand_arr, 1) loop

    the (i)th iterator_array is set to
      iterand_arr[i][ for all values the remaining 2nd, 3rd, and 4th indexes ]

  end loop;
  ```

The examples below clarify the behavior of `FOREACH`.

## Syntax and semantics
```
[ <<label>> ]
FOREACH var [ SLICE non_negative_integer_literal ] IN ARRAY expression LOOP
  statements
END LOOP [ label ];
```
- `var` must be explicitly declared before the `FOREACH` loop.
- The operand of the optional `SLICE` clause must be a non-negative `int` literal.
- Assume that `expression` has the data type `some_type[]`.
		- When `SLICE 0` is used, `var` must be declared as `some_type`.
  - When the `SLICE` clause's operand is positive, `var` must be declared as `some_type[]`.
- `SLICE 0` has the same effect as omitting the `SLICE` clause.
- When `SLICE 0` is used, or the `SLICE` clause is omitted, YSQL assigns each in turn of the array's  values, visited in row-major order, to `var`.
- When the `SLICE` clause's operand is positive, then YSQL assigns successive slices of the iterand array to `var` according to the following rule:

  - The extracted slices all have the same dimensionality given by this equality:<br>
`array_ndims(iterator_array) = slice_operand`.
  - The number of extracted slices is given by this equality:<br>
  `number_of_slices = cardinality(iterand_array)/cardinality(iterator_array)`.
  - The value of the `SLICE` operand must not exceed `array_ndims(iterator_array)`.
  - When the value of the `SLICE` operand is equal to `array_ndims(iterator_array) - 1`, the `FOREACH` loop produces just a single iterator array value, and this is identical to the iterand array.

- The useful range for the `SLICE` operand is therefore `0..(array_ndims(iterator_array) - 1)`.

## Looping over the values in an array without the SLICE keyword

This loop is functionally identical to the `FOR var IN` loop. However, in the `FOR` loop, YSQL automatically defines `var` with the type `int` so that its scope is limited to the loop; but in the `FOREACH` loop, `var` must be explicitly declared, as noted  in the _"Syntax and semantics"_ section above.

```plpgsql
do $body$
declare
  arr1 int[] := array[1, 2];
  arr2 int[] := array[3, 4, 5];
  var int;
begin
  <<"FOREACH eaxmple">>
  foreach var in array arr1||arr2 loop
    raise info '%', var;
  end loop "FOREACH eaxmple";
end;
$body$;
```
It shows this (after manually stripping the _"INFO:"_ prompt):

```
1
2
3
4
5
```

The [`||`](../functions-operators/concatenation/#the-160-160-160-160-operator) operator is used only to emphasize that the iterand can be any expression whose data type is an array.

The next loop shows these things of note:

- The iterand array is multidimensional.
- The array type is _"rt[]"_ where _rt_ is a user-defined _"row"_ type.
- The syntax spot where _"var"_ is used above need not be occupied by a single variable. Rather, _"f1"_ and _"f2"_ are used, to correspond to the fields in _"rt"_.
- The `FOREACH` loop is followed by a _"cursor"_ loop whose `SELECT` statement uses `unnest()`.
- The `FOREACH` loop is more terse than the _"cursor"_ loop. In particular, you can use the pair of declared variables _"f1"_ and _"f2"_ without any fuss (just as you could have used a single variable _"r"_ of type _"rt"_ without any fuss) as the iterator. YSQL looks after the proper assignment in both cases. But when you use `unnest()`, you have to look after this yourself.
```plpgsql
create type rt as (f1 int, f2 text);

do $body$
declare
  a1 rt[] := array[(1, 'dog'), (2, 'cat')];
  a2 rt[] := array[(3, 'ant'), (4, 'rat')];
  arr rt[] := array[a1, a2];
  f1 int;
  f2 text;
begin
  raise info '';
  raise info 'FOREACH';
  foreach f1, f2 in array arr loop
    raise info '% | %', f1::text, f2;
  end loop;

  raise info '';
  raise info 'unnest()';
  for f1, f2 in (
    with v as (
      select unnest(arr) as r)
    select (r).f1, (r).f2 from v)
  loop
    raise info '% | %', f1::text, f2;
  end loop;
end;
$body$;
```
It shows this:
```
FOREACH
1 | dog
2 | cat
3 | ant
4 | rat

unnest()
1 | dog
2 | cat
3 | ant
4 | rat
```
This shows that this use of the `FOREACH` loop (with an implied `0` as the `SLICE` clause's  operand) is functionally equivalent to `unnest()`.

## Looping over the contents of a multidimensional array taking advantage of a non-zero SLICE operand

First, store a three-dimensional two-by-two-by-two array in a `::text[]` field in a single-row table. Each of the subsequent `DO` blocks uses it.

```plpgsql
create table t(k int primary key, arr text[] not null);
insert into t(k, arr) values(1, '
  {
    {
      {001,002},
      {003,004}
    },
    {
      {005,006},
      {007,008}
    }
  }'::text[]);
```
Next, show the outcome when a bad value is used for the `SLICE` operand:
```plpgsql
do $body$
declare
  arr constant text[] not null := (select arr from t where k = 1);
  slice_iterator text[] not null := '{?}';
begin
  raise info 'array_ndims(arr): %', array_ndims(arr)::text;
  foreach slice_iterator slice 4 in array arr loop
    raise info '%', slice_iterator::text;
  end loop;
end;
$body$;
```
It shows `array_ndims(arr): 3` and then it reports this error:

```
2202E: slice dimension (4) is out of the valid range 0..3
```

This test confirms that the value of the `SLICE` operand must not exceed the iterand array's dimensionality.

As has been seen, `SLICE 0` (or equivalently omitting the `SLICE` clause) scans the array values in row-major order. The next test, with `SLICE 3`, demonstrates the meaning when the `SLICE` operand is equal to the iterand array's dimensionality.

```plpgsql
do $body$
declare
  arr constant text[] not null := (select arr from t where k = 1);
  slice_iterator text[] not null := '{?}';
  n int not null := 0;
begin
  raise info 'FOREACH SLICE 3';
  n := 0;
  foreach slice_iterator slice 3 in array arr loop
    assert
      (array_ndims(slice_iterator) = 3) and
      (slice_iterator = arr)           ,
    'assert failed';
    n := n + 1;
    raise info '% | %', n::text, slice_iterator::text;
  end loop;
end;
$body$;
```
It shows this:
```
FOREACH SLICE 3
1 | {{{001,002},{003,004}},{{005,006},{007,008}}}
```
The `FOREACH` loop generates just a single iterator slice. And, as the `assert` shows, this is identical to the iterand array. In other words, setting the `SLICE` operand to be equal to the iterand array's dimensionality, while the result is well-defined, is not useful. So, using this example iterand array, the useful range for the `SLICE` operand is `0..2`.

The next test uses `SLICE 2`:

```plpgsql
do $body$
declare
  arr constant text[] not null := (select arr from t where k = 1);
  slice_iterator text[] not null := '{?}';
  n int not null := 0;
begin
  raise info 'FOREACH SLICE 2';
  n := 0;
  foreach slice_iterator slice 2 in array arr loop
    assert (array_ndims(slice_iterator) = 2), 'assert failed';
    n := n + 1;
    raise info '% | %', n::text, slice_iterator::text;
  end loop;
end;
$body$;
```
It shows this:
```
FOREACH SLICE 2
1 | {{001,002},{003,004}}
2 | {{005,006},{007,008}}
```
As the `assert` shows, the operand of the `SLICE` operator determines the dimensionality of the iterator slices.

The next test uses `SLICE 1`: 

```plpgsql
do $body$
declare
  arr constant text[] not null := (select arr from t where k = 1);
  slice_iterator text[] not null := '{?}';
  n int not null := 0;
begin
  raise info 'FOREACH SLICE 1';
  n := 0;
  foreach slice_iterator slice 1 in array arr loop
    assert (array_ndims(slice_iterator) = 1), 'assert failed';
    n := n + 1;
    raise info '% | %', n::text, slice_iterator::text;
  end loop;
end;
$body$;
```
It shows this:
```
FOREACH SLICE 1
1 | {001,002}
2 | {003,004}
3 | {005,006}
4 | {007,008}
```
Once again, the `assert` shows that the operand of the `SLICE` operator determines the dimensionality of the iterator slices.

The last `FOREACH` test uses `SLICE 0`. Notice that, now, the iterator is declared as the scalar `text` variable  _"var"_:

```plpgsql
do $body$
declare
  arr constant text[] not null := (select arr from t where k = 1);
  var text not null := '?';
  n int not null := 0;
begin
  raise info 'FOREACH SLICE 0';
  n := 0;
  foreach var slice 0 in array arr loop
    n := n + 1;
    raise info '% | %', n::text, var;
  end loop;
end;
$body$;
```
It shows this:
```
FOREACH SLICE 0
1 | 001
2 | 002
3 | 003
4 | 004
5 | 005
6 | 006
7 | 007
8 | 008
```
This is functionally equivalent to `unnest()` as the final test shows:
```plpgsql
do $body$
<<b>>declare
  arr constant text[] not null := (select arr from t where k = 1);
  var text not null := '?';
  n int not null := 0;
begin
  raise info 'unnest()';
  n := 0;
  for b.n, b.var in (
    with
      v1 as (
        select unnest(arr) as var),
      v2 as (
        select
          v1.var,
          row_number() over(order by v1.var) as n
        from v1)
    select v2.n, v2.var from v2)
  loop
    n := n + 1;
    raise info '% | %', n::text, var;
  end loop;
end b;
$body$;
```
It shows this:
```
unnest()
2 | 001
3 | 002
4 | 003
5 | 004
6 | 005
7 | 006
8 | 007
9 | 008
```

## Using FOREACH to iterate over the elements in an array of DOMAIN values

You need to be aware of some special considerations to implement this scenario. [Using FOREACH with an array of DOMAINs](../array-of-domains/#using-foreach-with-an-array-of-domains), within the dedicated section [Using an array of DOMAIN values](../array-of-domains/) explains what you need to know. 

## Using a wrapper PL/pgSQL table function to expose the SLICE operand as a formal parameter

The fact that the `SLICE` operand must be a literal means that there are only two ways two parameterize this—and neither is satisfactory for real application code. Each uses a table function whose input is the iterand array and the value for the `SLICE` operand, and whose output is a `SETOF` iterator array values.

- The first approach is to encapsulate some particular range of `SLICE` operand values in an ordinary statically defined function that uses a `CASE` statement to select the `FOREACH` loop that has the required `SLICE` operand literal. This is unsatisfactory because you have to decide the range of `SLICE` operand values that you'll support up front. 
- The second approach overcomes the limitation of the up front determination of the supported range of `SLICE` operand values by encapsulating code in, a statically defined function, that in turn dynamically generates a function with the required`FOREACH` loop and `SLICE` operand value and that then invokes it dynamically. This is unsatisfactory because it's some effort to implement and test such an approach. But it's unsatisfactory mainly because of the performance cost that dynamic generation and execution brings.

However, the requirements specification for real application code is unlikely to need more than one, or possibly just a few, specific values for the `SLICE` operand. Therefore, in overwhelming majority of practically important use cases, you can write exactly the code you need where you need it.

The code that follows uses the first approach. It's included here because it demonstrates a generically valuable PL/pgSQL programming technique: user-defined functions and procedures with polymorphic formal parameters (in this case `anyarray` and `anyelement`). The examples also use `assert` statements to confirm that the expected relationships hold between these quantities:

- the dimensionality of the iterand array
- the value of the `SLICE` operand
- the lengths along the iterand array's dimensions
- the cardinalities of the iterand array and the iterator arrays
- the number of returned iterator values.

Here is the basic encapsulation. It's hard-coded to handle values for the `SLICE` operand in the range `0..4`.

Recall that the iterator for `SLICE 0` is a scalar and that the iterators for other values of the `SLICE` operand are arrays. And recall that a pair of functions with the same definitions of the input formal parameters cannot be overload-distinguished by the data type of their return values. For this reason, the encapsulation of the `FOREACH` loop for `SLICE 0` is a dedicated function with just one input formal parameter: the iterand array. And the encapsulation of the `FOREACH` loop for other values of the  `SLICE` operand is a second function with two input formal parameters: the iterand array and the value of the  `SLICE` operand. Here they are:

```plpgsql
-- First overload
create function array_slices(arr in anyarray)
  returns table(ret anyelement)
  language plpgsql
as $body$
declare
  no_of_values int not null := 0;
begin
  -- "slice 0" means the same
  -- as omitting the "slice" clause.
  foreach ret slice 0 in array arr
  loop
    no_of_values := no_of_values + 1;
    return next;
  end loop;
  assert
    (no_of_values = cardinality(arr)),
  'array_slices 1st overload: no_of_values assert failed';
end;
$body$;
```
And:
```plpgsql
-- Second overload
create function array_slices(arr in anyarray, slice_operand in int)
  returns table(ret anyarray)
  language plpgsql
as $body$
declare
  no_of_values int not null := 0;
  lengths_product int not null := 0;
begin
  case slice_operand
    when 1 then
      lengths_product := array_length(arr, 4);
      foreach ret slice 1 in array arr
      loop
        no_of_values := no_of_values + 1;
        assert
          (array_ndims(ret) = 1)               and
          (cardinality(ret) = lengths_product) ,
        'assert failed';
        return next;
      end loop;
      assert
        (no_of_values = cardinality(arr)/lengths_product),
      'array_slices 2nd overload: no_of_values assert #1 failed';

    when 2 then
      lengths_product := array_length(arr, 4) *
                         array_length(arr, 3);
      foreach ret slice 2 in array arr
      loop
        no_of_values := no_of_values + 1;
        assert
          (array_ndims(ret) = 2)               and
          (cardinality(ret) = lengths_product) ,
        'assert failed';
        return next;
      end loop;
      assert
        (no_of_values = cardinality(arr)/lengths_product),
      'array_slices 2nd overload: no_of_values assert #2 failed';

    when 3 then
      lengths_product := array_length(arr, 4) *
                         array_length(arr, 3) *
                         array_length(arr, 2);
      foreach ret slice 3 in array arr
      loop
        no_of_values := no_of_values + 1;
        assert
          (array_ndims(ret) = 3)               and
          (cardinality(ret) = lengths_product) ,
        'assert failed';
        return next;
      end loop;
      assert
        (no_of_values = cardinality(arr)/lengths_product),
      'array_slices 2nd overload: no_of_values assert #3 failed';

    when 4 then
      lengths_product := array_length(arr, 4) *
                         array_length(arr, 3) *
                         array_length(arr, 2) *
                         array_length(arr, 1);
      foreach ret slice 4 in array arr
      loop
        no_of_values := no_of_values + 1;
        assert
          (array_ndims(ret) = 4)               and
          (cardinality(ret) = lengths_product) ,
        'assert failed';
        return next;
      end loop;
      assert
        (no_of_values = cardinality(arr)/lengths_product),
      'array_slices 2nd overload: no_of_values assert #4 failed';

    else
      raise exception 'slice_operand > 4 not supported';
  end case;
end;
$body$;
```
You can see that each leg of the `CASE` is "generated" formulaically—albeit manually—by following a pattern that could be parameterized. You can use these encapsulations for iterand arrays of any dimensionality. But you must take responsibility for following the rule that the value of the `SLICE` operand must fall within the acceptable range. Otherwise, you'll get the error that was demonstrated above:
```
2202E: slice dimension % is out of the valid range 0..%
```
Here is the test harness. Both this procedure and the function that generates the to-be-tested iterand array are hard-coding for a dimensionality of `4`.
```plpgsql
-- Exercise each of the meaningful calls to array_slices().
--
-- You cannot declare local variables as "anyelement" or "anyarray".
-- (The attempt causes a compilation error). It's obvious why.
-- It's the caller's responsibility to determine the
-- real type by using appropriate actual arguments.
-- "val" (scalar) and "slice" (array) are needed as FOREACH loop runners.
-- "in out" is used to avoid the nominal performance penalty
--  of extra copying brought by plain "out".
-- The caller has no interest in whatever values they have
-- on return from this procedure.
--
-- NOTE: while you _can_ declare a local variable as
-- "some_formal%type", you _cannot_ use that mechanism to declare
-- a scalar with the data type that defines an array when
-- all you have to anchor "%type" to is the array.
--
create procedure test_array_slices(
  -- The "real" formal.
  arr   in     anyarray,

  -- used as "local varables
  val   in out anyelement,
  slice in out anyarray)
  language plpgsql
as $body$
declare
  arr_ndims constant int := array_ndims(arr);
begin
  assert
    (arr_ndims = 4),
  'assert failed: test_array_slices() requires a 4-d array';
  raise info '%', array_dims(arr);

  declare
    len_1 constant int := array_length(arr, 1);
    len_2 constant int := array_length(arr, 2);
    len_3 constant int := array_length(arr, 3);
    len_4 constant int := array_length(arr, 4);

    expected_slice_cardinalities constant int[] not null :=
      array[
        len_4,
        len_4*len_3,
        len_4*len_3,
        len_4*len_3*len_2,
        len_4*len_3*len_2*len_1];

    slice_cardinality int not null := 0;

    -- val anyelement not null := '?';
    -- slice anyarray not null := '{?}';
  begin
    raise info ''; raise info 'slice_operand: %', 0;

    for val in (select array_slices(arr)) loop
      raise info '%', val::text;
    end loop;

    for slice_operand in 1..arr_ndims loop
      raise info ''; raise info 'slice_operand: %', slice_operand;

      for slice in (select array_slices(arr, slice_operand)) loop
        slice_cardinality := cardinality(slice);
        assert
          (array_ndims(slice) = slice_operand) ,
          (slice_cardinality = expected_slice_cardinalities[slice_operand]) ,
        'assert failed.';
        raise info '%', slice::text;
        if slice_operand = arr_ndims then
          assert (slice = arr), 'assert (slice = arr) failed';
        end if;
      end loop;
    end loop;
  end;
end;
$body$;
```
Here is a function to generate a four-dimensional array. Notice that the actual argument for  the _"lengths"_ formal parameter must be a one-dimensional `int[]` array with four values. These specify the lengths along each of the output array's dimensions.
```plpgsql
create function four_d_array(lengths in int[])
  returns text[]
  language plpgsql
as $body$
declare
  lengths_ndims       constant int := array_ndims(lengths);
  lengths_cardinality constant int := cardinality(lengths);
begin
  assert
    (lengths_ndims = 1)       and
    (lengths_cardinality = 4) ,
  'assert failed: four_d_array() creates only a 4-d array.';

  declare
    -- Take the default for array_fill's optional 2nd formal:
    -- all lower bounds are 1.
    arr text[] not null := array_fill('00'::text, lengths);
  begin
    -- For readability of the results, define the created array's values so that,
    -- when scanned in row-major order, they are seen to be a dense series
    -- that increases in even steps, 001, 002, 003, and so on.
    declare
      n int not null := 0;
    begin
      for i1 in 1..lengths[1] loop
        for i2 in 1..lengths[2] loop
          for i3 in 1..lengths[3] loop
            for i4 in 1..lengths[4] loop
              n := n + 1;
              arr[i1][i2][i3][i4] := ltrim(to_char(n, '009'));
            end loop;
          end loop;
        end loop;
      end loop;
    end;
    assert
      (array_ndims(arr) = lengths_cardinality),
    'assert failed.';

    -- Sanity check. Include to demonstrate the useful
    -- terseness of the FOREACH loop.
    declare
      product int not null := 1;
      len int not null := 0;
    begin
      foreach len in array lengths loop
        product := product*len;
      end loop;
      assert
        (cardinality(arr) = product) ,
      'assert failed.';
    end;
    return arr;
  end;
end;
$body$;
```
And here is one example test invocation:
```plpgsql
do $body$
declare
  arr constant text[] not null := four_d_array('{2, 2, 2, 2}'::int[]);
  dummy_var text := '?';
  dummy_arr text[] := '{/}';
begin
  call test_array_slices(arr, dummy_var, dummy_arr);
end;
$body$;
```
It produces this result:
```
[1:2][1:2][1:2][1:2]

slice_operand: 0
001
002
003
004
005
006
007
008
009
010
011
012
013
014
015
016

slice_operand: 1
{001,002}
{003,004}
{005,006}
{007,008}
{009,010}
{011,012}
{013,014}
{015,016}

slice_operand: 2
{{001,002},{003,004}}
{{005,006},{007,008}}
{{009,010},{011,012}}
{{013,014},{015,016}}

slice_operand: 3
{{{001,002},{003,004}},{{005,006},{007,008}}}
{{{009,010},{011,012}},{{013,014},{015,016}}}

slice_operand: 4
{{{{001,002},{003,004}},{{005,006},{007,008}}},{{{009,010},{011,012}},{{013,014},{015,016}}}}
```
