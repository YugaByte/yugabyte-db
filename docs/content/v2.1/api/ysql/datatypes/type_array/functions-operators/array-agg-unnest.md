---
title: array_agg() and unnest()
linkTitle: array_agg() / unnest()
headerTitle: array_agg() and unnest()
description: array_agg() and unnest()
block_indexing: true
menu:
  v2.1:
    identifier: array-agg-unnest
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---

For one-dimensional arrays, but _only for these_ (see [Multidimensional `array_agg()` and `unnest()`](./#multidimensional-array-and-and-unnest)), these two functions have mutually complementary effects in the following sense. After this sequence (the notation is informal):
```
array_agg of "SETOF tuples #1" => "result array"
unnest of "result array" => "SETOF tuples #3"
```
The _"SETOF tuples #3"_ has identical shape and content to that of "_SETOF tuples #1"_. And the data type of _"result array"_ is an array of the data type of the tuples.

For this reason, the two functions, `array_agg()` and `unnest()`, are described in the same section.

## array_agg()

This function has two overloads.

### array_agg() — first overload

**Purpose:** Return a one-dimensional array from a SQL subquery. Its rows might be scalars (that is, the `SELECT` list might be a single column). But, in typical use, they are likely to be of _"row"_ type values.

**Signature:**
```
input value:       SETOF anyelement
return value:      anyarray
```
In normal use, `array_agg()` is applied to the `SELECT` list from a physical table, or maybe from a view that encapsulates the query. This is shown in the _"[Realistic use case](./#realistic-use-case)"_ example below. But first, you can demonstrate the functionality without creating and populating a table by using, instead, the `VALUES` statement. Try this:

```postgresql
values
  (1::int, 'dog'::text),
  (2::int, 'cat'::text),
  (3::int, 'ant'::text);
```

It produces this result:
```
 column1 | column2 
---------+---------
       1 | dog
       2 | cat
       3 | ant
```
Notice that YSQL has named the `SELECT` list items _"column1"_ and _"column2_". The result is a so-called `SETOF`. It means a set of rows, just as is produced by a `SELECT` statement. (You'll term if you describe the `generate_series()` built-in table function with the `\df` metacommand.) To use the rows that the `VALUES` statement produces as the input for `array_agg()`, you need to use a named `type`, thus:
```postgresql
create type rt as (f1 int, f2 text);

with tab as (
  values
    (1::int, 'dog'::text),
    (2::int, 'cat'::text),
    (3::int, 'ant'::text))
select array_agg((column1, column2)::rt order by column1) as arr
from tab;
```
It produces this result:
```
               arr               
---------------------------------
 {"(1,dog)","(2,cat)","(3,ant)"}
```
You recognize this as the text of the literal that represents an array of tuples that are shape-compatible with _"type rt"_. The underlying notions that explain what is seen here are explained in the section [The non-lossy round trip: value to `text` typecast and back to value](../../literals/text-typecasting-and-literals/#the-non-lossy-round-trip-value-to-text-typecast-and-back-to-value).

Recall from the [`array[]` constructor](../../array-constructor/) section that this value doesn't encode the type name. In fact, you could typecast it to any shape compatible type.

You can understand the effect of `array_agg()` thus:

- Treat each row as a _"rt[]"_ array with a single-value.
- Concatenate (see the [`||` operator](../concatenation/#the-160-160-160-160-operator)) the values from all the rows in the specified order into a new _"rt[]"_ array.

This code illustrates this point:
```postgresql
-- Consider this SELECT:
with tab as (
  values
    ((1, 'dog')::rt),
    ((2, 'cat')::rt),
    ((3, 'ant')::rt))
select array_agg(column1 order by column1) as arr
from tab;

-- It can be seen as equivalent this SELECT:
select
  array[(1, 'dog')::rt] ||
  array[(2, 'cat')::rt] ||
  array[(3, 'ant')::rt]
as arr;
```
Each of the three _"select... as arr"_ queries above produces the same result, as was shown after the first of them. This demonstrates their semantic equivalence.

To prepare for the demonstration of `unnest()`, save the single-valued result from the most recent of the three queries (but any one of them would do) into a `ysqlsh` variable by using the `\gset` metacommand. This takes a single argument, conventionally spelled with a trailing underscore (for example, _"result&#95;"_) and re-runs the `SELECT` statement that, as the last submitted `ysqlsh` command, is still in the command buffer. (If the `SELECT` doesn't return a single row, then you get a clear error.) In general, when the `SELECT` list has _N_ members, called _"c1"_ through _"cN"_, each of these values is stored in automatically-created variables called _"result&#95;c1"_ through _"result&#95;cN"_.

if you aren't already familiar with the `\gset` metacommand, you can read a brief account of how it works in the section [Meta-commands](../../../../../../admin/ysqlsh/#meta-commands) within the major section on `ysqlsh`.

Immediately after running the _"with... select array_agg(...) as arr..."_ query above, do this:

```postgresql
\gset result_
\echo :result_arr
```
The `\gset` metacommand is silent. The `\echo` metacommand shows this:
```
{"(1,dog)","(2,cat)","(3,ant)"}
```
The text of the literal is now available for re-use, as was intended.

Before considering `unnest()`, look at `array_agg()`'s second overload:

### array_agg() — second overload

**Purpose:** Return a (N+1)-dimensional array from a SQL subquery whose rows are N-dimensional arrays. The aggregated arrays must all have the same dimensionality.

**Signature:**
```
input value:       SETOF anyarray
return value:      anyarray
```
Here is a positive example:
```postgresql
with tab as (
  values
    ('{a, b, c}'::text[]),
    ('{d, e, f}'::text[]))
select array_agg((column1)::text[] order by column1) as arr
from tab;
```
It produces this result:
```
        arr        
-------------------
 {{a,b,c},{d,e,f}}
```
And here is a negative example:
```
with tab as (
  values
    ('{a, b, c}'::text[]),
    ('{d, e   }'::text[]))
select array_agg((column1)::text[] order by column1) as arr
from tab;
```
It causes this error:
```
2202E: cannot accumulate arrays of different dimensionality
```

## unnest()

This function has two overloads. The first is straightforward and has an obvious usefulness. The second is rather exotic.

### unnest() — simple overload

**Purpose:** Transform the values in a single array into a SQL table (that is, a `SETOF`) these values.

**Signature:**
```
input value:       anyarray
return value:      SETOF anyelement
```

As the sketch at the start of this page indicated, the input to unnest is an array. To use what the code example in the account of array_agg() set in the `ysqlsh` variable _"result&#95;arr"_ in a SQL statement, you must quote it and typecast it to _"rt[]"_. This can be done with the \set metacommand, thus:

```postgresql
\set unnest_arg '\'':result_arr'\'::rt[]'
\echo :unnest_arg
```

The `\set` metacommand uses the backslash character to escape the single quote character that it also uses to surround the string that it assigns to the target `ysqlsh` variable. The `\echo` metacommand shows this:
```
'{"(1,dog)","(2,cat)","(3,ant)"}'::rt[]
```
Now use it as the actual argument for `unnest()` thus:
```postgresql
with
  rows as (
    select unnest(:unnest_arg) as rec)
select
  (rec).f1,
  (rec).f2
from rows
order by 1;
```
The parentheses around the column alias _"rec"_ are required to remove what the SQL compiler would otherwise see as an ambiguity, and would report as a _"42P01 undefined_table"_ error. This is the result:

```
 f1 |  f2  
---+-----
 1 | dog
 2 | cat
 3 | ant
```

As promised, the original `SETOF` tuples has been recovered.

### unnest() — exotic overload

**Purpose:** Transform the values in a variadic list of arrays into a SQL table whose columns each are a `SETOF` the corresponding input array's values. This overload can be used only in the `FROM` clause of a subquery. Each input array might have a different type and a different cardinality. The input array with the greatest cardinality determines the number of output rows. The rows of those input arrays that have smaller cardinalities are filled at the end with `NULL`s. The optional `WITH ORDINALITY` clause adds a column that numbers the rows.

**Signature:**
```
input value:       <variadic list of> anyarray
return value:      many coordinated columns of SETOF anyelement
```

```postgresql
create type rt as (a int, b text);

\pset null '<is null>'
select *
from unnest(
  array[1, 2],
  array[10, 20, 30, 45, 50],
  array['a', 'b', 'c', 'd'],
  array[(1, 'p')::rt, (2, 'q')::rt, (3, 'r')::rt, (4, 's')::rt]
)
with ordinality
as result(arr1, arr2, arr3, arr4_a, arr4_n, n);
```
It produces this result:
```
   arr1    | arr2 |   arr3    |  arr4_a   |  arr4_n   | n 
-----------+------+-----------+-----------+-----------+---
         1 |   10 | a         |         1 | p         | 1
         2 |   20 | b         |         2 | q         | 2
 <is null> |   30 | c         |         3 | r         | 3
 <is null> |   45 | d         |         4 | s         | 4
 <is null> |   50 | <is null> | <is null> | <is null> | 5
```

## Multidimensional array_agg() and unnest() — first overloads

Start by aggregating three `int[]` array instances and by preparing the result as an `int[]` literal for the next step using the same `\gset` technique that was used above:
```postgresql
with tab as (
  values
    ('{1, 2, 3}'::int[]),
    ('{4, 5, 6}'::int[]),
    ('{7, 8, 9}'::int[]))
select array_agg(column1 order by column1) as arr
from tab

\gset result_
\set unnest_arg '\'':result_arr'\'::int[]'
\echo :unnest_arg
```
Notice that the SQL statement, this time, is _not_ terminated with a semicolon. Rather, the `\gset` metacommand acts as the terminator. This makes the `ysqlsh` output less noisy. This is the result:

```
'{{1,2,3},{4,5,6},{7,8,9}}'::int[]
```
You recognize this as the literal for a two-dimensional array. Now use this as the actual argument for `unnest()`:
```postgresql
select unnest(:unnest_arg) as val 
order by 1;
```
It produces this result:
```
 val 
-----
   1
   2
   3
   4
   5
   6
   7
   8
   9
```
This `SETOF` result lists all of the input array's "leaf" values in row-major order. This term is explained in the [Joint semantics](./functions-operators/properties/#joint-semantics)) section within the _"Functions for reporting the geometric properties of an array"_ section.

Notice that, for the multidimensional case, the original input to `array_agg()` was _not_, therefore, regained. This point is emphasized by aggregating the result:

```postgresql
with a as
  (select unnest(:unnest_arg) as val)
select array_agg(val order by val) from a;
```
It produces this result:
```
      array_agg      
---------------------
 {1,2,3,4,5,6,7,8,9}
```
You started with a two-dimensional array. But now you have a one-dimensional array with the same values as the input array in the same row-major order.

This result has the same semantic content that the `array_to_string()` function produces:

```postgresql
select array_to_string(:unnest_arg, ',');
```
It produces this result:
```
  array_to_string  
-------------------
 1,2,3,4,5,6,7,8,9
```
See the section [Looping through arrays in PL/pgSQL](../../looping-through-arrays/). This shows how you can use the `FOREACH` loop in procedural code, with an appropriate value for the `SLICE` operand, to unnest an array into a set of subarrays whose dimensionality you can choose. At one end of the range, you can mimmic `unnest()` and produce scalar values. At the other end of the range, you can produce a set of arrays with dimensionality `n - 1` where `n` is the dimensionality of the input array.

## Realistic use case

The basic illustration of the functionality of `array_agg()` showed how it can convert the entire contents of a table (or, by extension, the `SETOF` rows defined by a `SELECT` execution) into a single array value. This can be useful to return a large `SELECT` result in its entirety (in other words, in a single round trip) to a client program.

Another use is to populate a single newly-created _"masters_with_details"_ table from the fully projected and unrestricted `INNER JOIN` of a classic _"masters"_ and _"details"_ pair of tables. The new table has all the columns that the source _"masters"_ table has and all of its rows. And it has an additional _"details"_ column that holds, for each _"masters"_ row, a _"details_t[]"_ array that represents all of the child rows that it has in the source _"details"_ table. The type _"details&#95;t"_ has all of the columns of the _"details"_ table except the _"details.masters_pk"_ foreign key column. This column vanishes because, as the _join_ column, it vanishes in the `INNER JOIN`. The _"details"_ table's "payload" is now held in place in a single multivalued field in the new _"masters&#95;with&#95;details"_ table.

Start by creating and populating the _"masters"_ and _"details"_ tables:
```postgresql
create table masters(
  master_pk int primary key,
  master_name text not null);

insert into masters(master_pk, master_name)
values
  (1, 'John'),
  (2, 'Mary'),
  (3, 'Joze');

create table details(
  master_pk int not null,
  seq int not null,
  detail_name text not null,

  constraint details_pk primary key(master_pk, seq),

  constraint master_pk_fk foreign key(master_pk)
    references masters(master_pk)
    match full
    on delete cascade
    on update restrict);

insert into details(master_pk, seq, detail_name)
values
  (1, 1, 'cat'),    (1, 2, 'dog'),
  (2, 1, 'rabbit'), (2, 2, 'hare'), (2, 3, 'squirrel'), (2, 4, 'horse'),
  (3, 1, 'swan'),   (3, 2, 'duck'), (3, 3, 'turkey');
```
Next, create a view that encodes the fully projected, unrestricted _inner join_ of the original data, and inspect the result set that it represents:
```postgresql
create view original_data as
select
  master_pk,
  m.master_name,
  d.seq,
  d.detail_name
from masters m inner join details d using (master_pk);

select
  master_pk,
  master_name,
  seq,
  detail_name
from original_data
order by
master_pk, seq;
```
This is the result:
```
 master_pk | master_name | seq | detail_name 
-----------+-------------+-----+-------------
         1 | John        |   1 | cat
         1 | John        |   2 | dog
         2 | Mary        |   1 | rabbit
         2 | Mary        |   2 | hare
         2 | Mary        |   3 | squirrel
         2 | Mary        |   4 | horse
         3 | Joze        |   1 | swan
         3 | Joze        |   2 | duck
         3 | Joze        |   3 | turkey
```
Next, create the type _"details&#95;t"_ and the new table:

```postgresql
create type details_t as (seq int, detail_name text);

create table masters_with_details (
  master_pk int primary key,
  master_name text not null,
  details details_t[] not null);
```
Notice that you made the _"details"_ column `not null`. This was a choice. It adds semantics that are notoriously difficult to capture in the original two table design without tricky, and therefore error-prone, programming of triggers and the like. You have implemented the so-called _"mandatory one-to-many"_ rule. In the present example, the rule says (in the context of the entity-relationship model that specifies the requirements) that an occurrence of a _"Master"_ entity type cannot exist unless it has at least one, but possibly many, child occurrences of a _"Detail"_ entity type.

Next, populate the new table and inspect its contents:
```postgresql
insert into masters_with_details
select
  master_pk,
  master_name,
  array_agg((seq, detail_name)::details_t order by seq) as agg
from original_data
group by master_pk, master_name;

select master_pk, master_name, details
from masters_with_details
order by 1;
```
This is the result:
```
 master_pk | master_name |                       details                        
-----------+-------------+------------------------------------------------------
         1 | John        | {"(1,cat)","(2,dog)"}
         2 | Mary        | {"(1,rabbit)","(2,hare)","(3,squirrel)","(4,horse)"}
         3 | Joze        | {"(1,swan)","(2,duck)","(3,turkey)"}
```
Here's a helper function to show the primitive values that the _"details&#95;t[]"_ array encodes without the clutter of the array literal syntax:

```postgresql
create function pretty_details(arr in details_t[])
  returns text
  immutable
  language plpgsql
as $body$
declare
  arr_type constant text := pg_typeof(arr);
  ndims constant int := array_ndims(arr);
  lb constant int := array_lower(arr, 1);
  ub constant int := array_upper(arr, 1);
begin
  assert arr_type = 'details_t[]', 'assert failed: ndims = %', arr_type;
  assert ndims = 1, 'assert failed: ndims = %', ndims;
  declare
    line text not null :=
      rpad(arr[lb].seq::text||': '||arr[lb].detail_name::text, 12)||
      ' | ';
  begin
    for j in (lb + 1)..ub loop
      line := line||
      rpad(arr[j].seq::text||': '||arr[j].detail_name::text, 12)||
      ' | ';
    end loop;
    return line;
  end;
end;
$body$;
```
Notice that this is not a general purpose function. Rather, it expects that the input is a _"details&#95;t[]"_ array. So it first checks that this pre-condition is met. It then discovers the lower and upper bounds of the array so that it can loop over its values. It uses these functions for reporting the geometric properties of the input array: [`array_ndims()`](../../functions-operators/properties/#array-ndims); [`array_lower()`](../../functions-operators/properties/#array-lower); and [`array_upper()`](../../functions-operators/properties/#array-upper).

Invoke it like this:

```postgresql
select master_pk, master_name, pretty_details(details)
from masters_with_details
order by 1;
```
It produces this result:
```
 master_pk | master_name |                        pretty_details                        
-----------+-------------+--------------------------------------------------------------
         1 | John        | 1: cat       | 2: dog       | 
         2 | Mary        | 1: rabbit    | 2: hare      | 3: squirrel  | 4: horse     | 
         3 | Joze        | 1: swan      | 2: duck      | 3: turkey    |
```
Next, create a view that uses `unnest()` to re-create the effect of the fully projected, unrestricted _inner join_ of the original data, and inspect the result set that it represents:
```postgresql
create view new_data as
with v as (
  select
    master_pk,
    master_name,
    unnest(details) as details
  from masters_with_details)
select
  master_pk,
  master_name,
  (details).seq,
  (details).detail_name
from v;

select
  master_pk,
  master_name,
  seq,
  detail_name
from new_data
order by
master_pk, seq;
```
The result is identical to what the _"original&#95;data"_ view represents. But rather than relying on visual inspection, can check that the _"new&#95;data"_ view and the the _"original&#95;data"_ view represent the identical result by using SQL thus:
```postgresql
with
  original_except_new as (
    select master_pk, master_name, seq, detail_name 
    from original_data
    except
    select master_pk, master_name, seq, detail_name
    from new_data),

  new_except_original as (
    select master_pk, master_name, seq, detail_name
    from new_data
    except
    select master_pk, master_name, seq, detail_name
    from original_data),

  original_except_new_union_new_except_original as (
    select master_pk, master_name, seq, detail_name
    from original_except_new
    union
    select master_pk, master_name, seq, detail_name
    from new_except_original)

select
  case count(*)
    when 0 then '"new_data" is identical to "original_data."'
    else        '"new_data" differs from "original_data".'
  end as result
from original_except_new_union_new_except_original;
```
This is the result:
```
                   result                    
---------------------------------------------
 "new_data" is identical to "original_data."
```
Notice that if you choose the _"masters&#95;with&#95;details"_ approach (either as a migration from a two-table approach in an extant application, or as an initial choice in a new application) you must appreciate the trade-offs.

**Prerequisite:**

- You must be confident that the _"details"_ rows are genuinely private each to its own master and do not implement a many-to-many relationship in the way that the _"order&#95;items"_ table does between the _"customers"_ table and the _"items"_ table in the classic sales order entry model that is frequently used to teach table design according to the relational model.

**Pros:**

- You can enforce the mandatory one-to-many requirement declaratively and effortlessly.
- Changing and querying the data will be faster because you use single table, single-row access rather than two-table, multi-row access.
- You can trivially recapture the query functionality of the two-table approach by implementing a _"new&#95;data"_ unnesting view as has been shown. So you can still find, for example, rows in the _"masters&#95;with&#95;details"_ table where the _"details"_ array has the specified values like this:
  ```postgresql
  with v as (
    select master_pk, master_name, seq, detail_name
    from new_data
    where detail_name in ('rabbit', 'horse', 'duck', 'turkey'))
  select
    master_pk,
    master_name,
    array_agg((seq, detail_name)::details_t order by seq) as agg
  from v
  group by master_pk, master_name
  order by 1;
  ```
&#160;&#160;&#160;&#160;This is the result:
  ```
   master_pk | master_name |            agg             
  -----------+-------------+----------------------------
           2 | Mary        | {"(1,rabbit)","(4,horse)"}
           3 | Joze        | {"(2,duck)","(3,turkey)"}
  ```
**Cons:**
- Changing the data in the _"details"_ array is rather difficult. Try this (in the two-table regime):
  ```postgresql
  update details
  set detail_name = 'bobcat'
  where master_pk = 2
  and detail_name = 'squirrel';

  select
    master_pk,
    master_name,
    seq,
    detail_name
  from original_data
  where master_pk = 2
  order by
  master_pk, seq;
  ```
&#160;&#160;&#160;&#160;This is the result:
```
 master_pk | master_name | seq | detail_name 
-----------+-------------+-----+-------------
         2 | Mary        |   1 | rabbit
         2 | Mary        |   2 | hare
         2 | Mary        |   3 | bobcat
         2 | Mary        |   4 | horse
```
- Here's how you achieve the same effect, and check that it worked as intended, in the new regime. Notice that you need to know the value of _"seq"_ for the _"rt"_ object that has the _"detail&#95;name"_ value of interest. This can be done by implementing a dedicated PL/pgSQL function that encapsulates `array_replace()` or that replaces a value directly by addressing it using its index. But it's hard to do without that. (These methods are described in [`array_replace()` and setting an array value explicitly](../../functions-operators/replace-a-value/).)

  ```postgresql
  update masters_with_details
  set details = array_replace(details, '(3,squirrel)', '(3,bobcat)')
  where master_pk = 2;

  select
    master_pk,
    master_name,
    seq,
    detail_name
  from new_data
  where master_pk = 2
  order by
  master_pk, seq;
  ```
&#160;&#160;&#160;&#160;The result is identical to the result shown for querying _"original&#95;data"_ above.

- Implementing the requirement that the values of _"detail&#95;name"_ must be unique for a given _"masters"_ row is trivial in the old regime:
  ```postgresql
  create unique index on details(master_pk, detail_name);
  ```
To achieve the effect in the new regime, you'd need to write a PL/pgSQL function, with return type `boolean` that scans the values in the _"details"_ array and returns `TRUE` when there are no duplicates among the values of the _"detail&#95;name"_ field and that otherwise returns `FALSE`. Then you'd use this function as the basis for a check constraint in the definition of the _"details&#95;with&#95;masters"_ table. This is a straightforward programming task, but it does take more effort than the declarative implementation of the business rule that the two-table regime allows.
