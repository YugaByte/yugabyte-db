---
title: YSQL array
linkTitle: Array
headerTitle: Array data types and functionality
description: YSQL lets you construct an array data type, of any dimensionality, of any built-in or user-defined data type. You can use this constructed data type for a table column and for a variable or formal parameter in a PL/pgSQL procedure.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: api-ysql-datatypes-array
    parent: api-ysql-datatypes
aliases:
  - /latest/api/ysql/datatypes/type_array
isTocNested: true
showAsideToc: true
---
**On this page**<br>
&#160;&#160;&#160;&#160;[Synopsis](./#synopsis)<br>
&#160;&#160;&#160;&#160;[Atomically null vs having all values null](./#atomically-null-vs-having-all-values-null)<br>
&#160;&#160;&#160;&#160;[Type construction](./#type-construction)<br>
&#160;&#160;&#160;&#160;[Informal sketch of array functionality](./#informal-sketch-of-array-functionality)<br>
&#160;&#160;&#160;&#160;[Uses of arrays](./#uses-of-arrays)<br>
&#160;&#160;&#160;&#160;[Example use case: GPS trip data](./#example-use-case-gps-trip-data)<br>
&#160;&#160;&#160;&#160;[Organization of the remaining array functionality content](./#organization-of-the-remaining-array-functionality-content)

## Synopsis

A multidimensional array lets you store a large composite value in a single field (row-column intersection) in a table; and it lets you assign such a value to a PL/pgSQL variable, or pass it via a procedure's, or a function's, formal parameter. 

You can see from the declarations below that every value in an array is non-negotiably of the same data type—either a primitive data type like `text` or `numeric`, or a user-defined scalar or composite data type (like a _"row"_ type).

An array is, by definition, a rectilinear N-dimensional set of "cells". You can picture a one-dimensional array as a line of cells, a two-dimensional array as a rectangle of cells, and a three-dimensional array as a cuboid of cells. The terms "line", "rectangle", and "cuboid" are the only specific ones. The generic term "N-dimensional array" includes these and all others. The meaning of "rectilinear" is sometimes captured by saying that the shape has no ragged edges or surfaces. If you try to create an array value that is not rectilinear, then you get an error whose detail says _"Multidimensional arrays must have sub-arrays with matching dimensions"_. The number of dimensions that an array has is called its _dimensionality_.

{{< note title="Ragged arrays" >}}
Sometimes, a ragged structure is useful. Here's an example:
- a one-dimensional array of "payload" one-dimensional arrays, each of which might have a different length

This structure is crucially different from a rectilinear two-dimensional array. A `DOMAIN` lets you create such a structure by providing the means to give the payload array data type a name. [Using an array of `DOMAIN` values](./array-of-domains/) shows how to do this.
{{< /note >}}

A value within an array is specified by a tuple of _index_ values, like this (for a four-dimensional array):
```
arr[13][7][5][17]
```
The index is the cell number along the dimension in question. The index values along each dimension are consecutive—in other words, you cannot delete a cell within a array. This reflects the fact that an array is rectilinear. However, a value in a cell can, of course, be `NULL`.

The leftmost value (`13` in the example) is the index along the first dimension; the rightmost value (`17` in this example) is the index along the Nth dimension—that is, the fourth dimension in this example. The value of the index of the first cell along a particular dimension is known as the _lower bound_ for that dimension. If you take no special steps when you create an array value, then the lower bound of each dimension is `1`. But, if you find it useful, you can specify any positive or negative integer, or zero, as the lower bound of the specified dimension. The lower bounds of an array are fixed at creation time, and so is its dimensionality.

Correspondingly, each dimension has an upper bound. This, too, is fixed at array creation time. The index values along each dimension are consecutive. The fact that each dimension has a single value for its upper and lower bound reflects the fact that an array is rectilinear.

If you read a within-array value with a tuple of index values that put it outside of the array bounds, then you silently get `NULL`. But if you attempt to set such an out-of-bounds value, then, because this is an implicit attempt to change the array's bounds, you get the _"array subscript out of range"_ error.

Notice that you can create an array, using a single assignment, as a so-called "slice" of an array, by specifying desired lower and upper index values along each axis of the source array. The new array cannot have a different dimensionality than its source. You should specify the lower and upper index values for the slice, along each dimension of the source array, to lie within (or, maximally, coincide with) the bounds of that dimension. If you specify the slice with a lower bound less than the corresponding lower bound of the source array, then the new lower bound is silently interpreted as the extant corresponding source lower bound. The same is true for the upper bounds. The syntax of this method means that the lower bounds of the new array inevitably all start at `1`. Here is an example (in PL/pgSQL syntax) using a two-dimensional source array:

```
new_arr := source_arr[3:4][7:9];
```
**Note:** A one-dimensional array is a special case because, uniquely among N-dimensional shapes, it is tautologically rectilinear. You can increase the length of such an array implicitly, by setting a value in a cell that has a lower index value than the present lower bound or a higher index value than the present upper bound. Once you've done this, there is no way to reduce the length because there is no explicit operation for this and no "unset" operation for a specified cell. You can, however, create a slice so that the new array has the source array's original size.

The following properties determine the shape of an array. Each can be observed using the listed dedicated function. The first formal parameter (with data type `anyarray`) is the array of interest . When appropriate, there's a second formal parameter (with data type `int`) that specifies the dimension of interest. The return is an `int` value, except in one case where it's a `text` value, as detailed below.

- [`array_ndims()`](functions-operators/properties/#array-ndims) returns the dimensionality of the specified array.

- [`array_lower()`](functions-operators/properties/#array-lower) returns the lower bound of the specified array on the specified dimension.

- [`array_upper()`](functions-operators/properties/#array-upper) returns the upper bound of the specified array on the specified dimension.

- [`array_length()`](functions-operators/properties/#array-length) returns the length of the specified array on the specified dimension. The length, the upper bound, and the lower bound, for a particular dimension, are mutually related, thus:
```
 "length" = "upper bound" - "lower bound" + 1
```

- [`cardinality()`](functions-operators/properties/#cardinality) returns the total number of cells (and therefore values) in the specified array. The cardinality and length along each dimension are mutually related, thus:
```
 "cardinality" = "length 1" * "length 2" * ... * "length N"
```

- [`array_dims()`](functions-operators/properties/#array-dims) returns a text representation of the same information as `array_lower()` and `array_length()` return, for all dimension in a single `text` value, showing the upper and lower bounds like this: `[3:4][7:9][2:5]` for a three-dimensional array. Use this for human consumption. Use `array_lower()` and `array_length()` for programmatic consumption.

Arrays are special because (unlike is the case for, for example, numeric data types like `decimal` and `int`, or character data types like `text` and `varchar`) there are no ready-made array data types. Rather, you construct the array data type that you need using an array _type constructor_. Here's an example:

```plpgsql
create table t1(k int primary key, arr text array[4]);
```
This syntax conforms to the SQL Standard. Notice that `array` is a reserved word. (You cannot, for example, create a table with that name.) It appears to let you specify just a one-dimensional array and to specify how many values it holds. But both of these apparent declarations of intent are ignored and act, therefore, only as potentially misleading documentation.

The following illustrates the PostgreSQL extension to the Standard that YSQL, therefore, inherits.:

```plpgsql
create table t2(
  k int primary key,
  one_dimensional_array int[],
  two_dimensional_array int[10][10]);
```
Notice that it appears, optionally, to let you specify how many values each dimension holds. (The Standard syntax allows the specification of the length of just one dimension.) However, these apparent declarations of intent, too, are silently ignored. Moreover, even the _dimensionality_ is ignored. The value, in a particular row, in a table column with an array data type (or its cousin, a variable in a PL/pgSQL program) can hold an array value of _any_ dimensionality. This is demonstrated by example in [Multidimensional array of `int` values](./literals/array-of-primitive-values/#multidimensional-array-of-int-values). This means that declaring an array using the reserved word `array`, which apparently lets you define only a one-dimensional array, and declaring an array using `[]`, which apparently lets you define array of any dimensionality, where one, some, or all of the dimensions are nominally constrained, are entirely equivalent.

The possibility that that different rows in the same table column can hold array values of different dimensionality is explained by picturing the implementation. Array values are held, in an opaque internal representation, as a linear "ribbon" of suitably delimited values of the array's data type. The array's actual dimensionality, and the upper and lower bound of the index along each dimension, is suitably represented in a header. This information is used, in a trivial arithmetic formula, to translate an address specification like `arr[13][7][5][17]` into the position of the value, as a single integer, along the ribbon of values. Understanding this explains why, except for the special case of a one-dimensional array, the dimensionality and the bounds of an array value are fixed at creation time. It also explains why a few of the array functions are supported only for one-dimensional arrays.

Yugabyte recommends that, for uniformity, you choose to declare arrays only with this syntax:

```
create table t2(
  k int primary key,
  one_dimensional_array int[],
  two_dimensional_array int[]);
```

The `array_ndims()` function lets you define a table constraint to insist that the array dimensionality is fixed for every row in a table column with such a data type. The `array_length()` function lets you insist that each dimension of a multidimensional array has a specified length for every row, or that its length doesn't exceed a specified limit for any row.

## Atomically null vs having all values null

Here is a minimal example:
```plpgsql
create table t(k int primary key, v int[]);
insert into t(k) values(1);
insert into t(k, v) values (2, '{null}'::int[]);
\pset null '<is null>'
select k, v, array_dims(v) as dims from t order by k;
```
It shows this:

```
 k |     v     |   dims    
---+-----------+-----------
 1 | <is null> | <is null>
 2 | {NULL}    | [1:1]
```

Because _"v"_ has no constraint, it can be `NULL`, just like when its data type is scalar. This is the case for the row with _"k = 1"_. Here, _"v"_ is said to be _atomically null_. (This term is usually used only when the data type is composite to distinguish the outcome from what is seen for the row with _"k = 2"_ where _"v"_ is not atomically null. The array properties of the first row's _"v"_, like its dimensionality, are all `NULL`. But for the second row, they have meaningful, `not null`, values. Now try this:
```plpgsql
update t set v = v||'{null}'::int[] where k = 2;
select k, v, array_dims(v) as dims from t where k = 2;
```
The `||` operator is explained in [Array concatenation functions and operators](./functions-operators/concatenation/#the-160-160-160-160-operator). The query shows this:

```
 k |      v      | dims  
---+-------------+-------
 2 | {NULL,NULL} | [1:2]
```
Here, _"v"_ for the second row, while not atomically null, has all of its values `NULL`. Its dimensionality cannot be changed, but because it is a one dimensional array, its length can be extended, as was explained above. This is allowed:
```plpgsql
update t set v[0] = 17 where k = 2;
select k, v, array_dims(v) as dims from t where k = 2;
```
It shows this:
```
 k |             v             | dims  
---+---------------------------+-------
 2 | [0:3]={17,NULL,NULL,NULL} | [0:3]
```
 This, too, is allowed:
```plpgsql
update t set v[1] = 42 where k = 1;
select k, v, array_dims(v) as dims from t where k = 1;
```
It shows this:
```
 k |  v   | dims  
---+------+-------
 1 | {42} | [1:1]
```

The dimensionality of _"v"_ for this first row has now been irrevocably established.


## Type construction

Arrays are not the only example of type construction. So, also, are _"row"_ types and `DOMAIN`s:

```plpgsql
create type rec_t as(f1 int, f2 text);

create domain medal_t as text
check(
  length(value) <= 6 and
  value in ('gold', 'silver', 'bronze')
);

create table t3(k int primary key, rec rec_t, medal medal_t);
```

Notice that you must define a _"row"_ type or a `DOMAIN` as a schema object. But you define the data type of an array "in place" when you create a table or write PL/pgSQL code, as was illustrated above. To put this another way, you _cannot_ name a constructed array type. Rather, you can use it only "on the fly" to define the data type of a column, a PL/pgSQL variable, or a PL/pgSQL formal parameter. The consequence of this is that while you _can_ define, for example, the data type of a named field in a _"row"_ type as an array of a specified data type, you _cannot_ define an array of a specified array data type. (If you try to write such a declaration, you'll see, as you type it, that you have no way to express what you're trying to say.)

## Informal sketch of array functionality

This sections within this "Array data types and functionality" major section carefully describe what is sketched here.

_First_, create a table with an `int[]` column and populate it with a two-dimensional array by using an array literal.
```plpgsql
create table t(
  k int primary key, v int[]);

insert into t(k, v) values(1,
   '{
      {11, 12, 13},
      {21, 22, 23}
    }
  '::int[]);
```
_Next_, look at a direct `::text` typecast of the value that was inserted:

```plpgsql
select v::text from t where k = 1;
```
It shows this:
```
            v            
-------------------------
 {{11,12,13},{21,22,23}}
```
Notice that, apart from the fact that it has no whitespace, this representation is identical to the literal that defined the inserted array. It can therefore be used in this way.

_Next_ check that the inserted array value has the expected properties:
```plpgsql
select
  array_ndims(v),
  array_length(v, 1),
  array_length(v, 2),
  array_dims(v)
from t where k = 1;
```
It shows this:
```
  array_ndims | array_length | array_length | array_dims 
-------------+--------------+--------------+------------
           2 |            2 |            3 | [1:2][1:3]
```

The `array_ndims()` function reports the dimensionality of the array; `array_length()` reports the length of the specified dimension (that is, the number of values that this dimension has); and `array_dims()` presents the same information, as a single `text` value, as using `array_length()` in turn for each dimension does. Notice that `array_length()` returns a _single_ `int` value for the specified dimension. Its design rests upon a rule, exemplified by saying that a two-dimensional array must be a rectangle (it cannot have a ragged edge). In the same way, a three-dimensional array must be a cuboid (it cannot have an uneven surface). This notion, though its harder to visualise, continues to apply as the number of dimensions increases.

Here's an example that violates the rule:
```plpgsql
insert into t(k, v) values(2,
   '{
      {11, 12, 13},
      {21, 22, 23, 24}
    }
  '::int[]);
```

The formatting emphasizes that its edge is ragged. It causes a _"22P02: malformed array literal"_ error whose detail says _"Multidimensional arrays must have sub-arrays with matching dimensions"_.

Finally, in this sketch, this `DO` block shows how you can visualise the values in a two-dimensional array as a rectangular grid.

```plpgsql
do $body$
declare
  arr constant int[] not null:= '{
      {11, 12, 13, 14},
      {21, 22, 23, 24},
      {31, 32, 33, 34}
    }'::int[];

  ndims constant int not null := array_ndims(arr);
  line text;
begin
  if array_ndims(arr) <> 2 then
    raise exception 'This code handles only a two-dimensional array.';
  end if;

  declare
    len1 constant int not null := array_length(arr, 1);
    len2 constant int not null := array_length(arr, 2);
  begin
    for row in 1..len1 loop
      line := ' ';
      for col in 1..len2 loop
        line := line||lpad(arr[row][col]::text, 5);
      end loop;
      raise info '%', line;
    end loop;
  end;
end;
$body$;
```
It produces this result (after manually stripping the _"INFO:"_ prompts):
```
   11   12   13   14
   21   22   23   24
   31   32   33   34
```
This approach isn't practical for an array with higher dimensionality or for a two-dimensional array whose second dimension is large. Rather, this code is included here to show how you can address individual elements. The names of the implicitly declared `FOR` loop variables _"row"_ and _"col"_ correspond intuitively to how the values are laid out in the literal that defines the array value. The nested loops are designed to visit the values in so-called row-major order (the last subscript varies most rapidly).

The term _"row-major order"_ is explained in [Joint semantics](./functions-operators/properties/#joint-semantics) within the section _"Functions for reporting the geometric properties of an array"_.

When, for example, the values of same-dimensioned multidimensional arrays are compared, they are visited in this order and compared pairwise in just the same way that scalar values are compared.

**Note:** The term "_row-major order"_ is explained in [Joint semantics](./functions-operators/properties/#joint-semantics)) within the _"Functions for reporting the geometric properties of an array"_ section. it contains a an example PL/pgSQL procedure that shows how to traverse an arbitrary two-dimensional array's values, where the lower bounds and lengths along each dimension are unknown beforehand, in this order.

Notice that, in the example above, the first value in each dimension has index value 1. This is the case when an array value is created using a literal and you say nothing about the index values. The next example shows how you can control where the index values for each dimension start and end.
```plpgsql
\pset null '<is null>'
with v as (
  select '[2:4][5:8]=
    {
      {25, 26, 27, 28},
      {35, 36, 37, 38},
      {45, 46, 47, 48}
    }'::int[] as arr)
select
  arr[0][0] as "[0][0]",
  arr[2][5] as "[2][5]",
  arr[2][8] as "[2][8]",
  arr[4][5] as "[4][5]",
  arr[4][8] as "[4][8]",
  arr[9][9] as "[9][9]"
from v;
```
In this syntax, `[2:4]` says that the index runs from 2 through 4 on the first dimension; and `[5:8]` says that runs from 5 through 8 on the second dimension. The values have been chosen to illustrate this. Of course, you must provide the right number of values for each dimension. The query produces this result:
```
  [0][0]   | [2][5] | [2][8] | [4][5] | [4][8] |  [9][9]   
-----------+--------+--------+--------+--------+-----------
 <is null> |     25 |     28 |     45 |     48 | <is null>
```
Notice that if you access an element whose index values put it outside the ranges of the defined values, then, as mentioned, you silently get `NULL`.

The values in an array are stored by laying out their internal representations consecutively in row-major order. This term is explained in [Joint semantics](./functions-operators/properties/#joint-semantics)) within the _"Functions for reporting the geometric properties of an array"_ section. Because every value has the same data type, this means that a value of interest can be addressed quickly, without index support, by calculating its offset. The value itself knows its dimensions. This explains how arrays of different dimensionality can be stored in a single table column. Even when the representations are of variable length (as is the case with, for example, `text` values), each knows its length so that the value boundaries can be calculated.

## Uses of arrays

You can use a one-dimensional array to store a graph, like temperature readings as a function of time. But the time axis is implicit: it's defined by each successive value's index. The application decides how to translate the integral index value to a time value.

You can use a two-dimensional array to store a surface. For example you could decide to interpret the first index as an increment in latitude, and the second index as an increment in longitude. You might, then, use the array values to represent, say, the average temperature, over some period, at a location measured at points on a rectangular grid.

A trained machine learning model is likely to be either a single array with maybe five or six dimensions and with fixed size. Or might be a collection of such arrays. It's useful, for various practical reasons, to store several of such models, corresponding to different stages of training or to different detailed use areas. The large physics applications at the Lawrence Livermore National Laboratory represent, and store, observations as multi-dimensional arrays.

In these uses, your requirement is to persist the data and then to retrieve it (possibly retrieving just a slice) for programmatic analysis of the kind for which SQL is at best cumbersome or at worst inappropriate. For example, a one-dimensional array might be used to represent a path on a horizontal surface, where the value is a row representing the _(x, y)_ coordinate pair, and you might want to fit a curve through the data points to smooth out measurement inaccuracies. The [GPS trip data](./#example-use-case-gps-trip-data) use case, described below, typifies this use of arrays.

Some use cases call for a multidimensional _ragged_ array-like structure. Such a structure doesn't qualify for the name "array" because it isn't rectilinear. The note above points to [Using an array of `DOMAIN` values](./array-of-domains/) which shows how to implement such a ragged structure.

## Example use case: GPS trip data

Amateur cyclists like to record their trips using a GPS device and then to upload the recorded data to one of no end of Internet sites, dedicated to that purpose, so that they can review their trips, and those of others, whenever they want to into the indefinite future. Such a site might use a SQL database to store all these trips.

The GPS device lets the cyclist split the trip into successive intervals, usually called laps, so that they can later focus their review attention on particular laps of interest like, for example, a notorious steep hill. So each trip has one or many laps. A lap is typically no more than about 100 km—and often more like 5-10 km. But it could be as large as, say, 300 km. The resolution of modern devices is typically just a few paces under good conditions—say 3m. So a lap could have as many as 100,000 GPS data points, each of which records the timestamp, position, and no end of other associated instantaneous values of facts like, for example, heart rate.

This sounds like a classic three table design, with foreign key constraints to capture the notion that a GPS data point belongs to a lap and that a lap belongs to a trip. The array data type allows all of the GPS data points that belong to a lap to be recorded in a single row in the _"laps"_ table—in other words as a multivalued field, thus: 

```plpgsql
create type gps_data_point_t as (
  ts          timestamp,
  lat         numeric,
  long        numeric,
  alt         numeric,
  cadence     int,
  heart_rate  int
  ...
  );

create table laps(
  lap_start_ts     timestamp,
  trip_start_ts    timestamp,
  userid           uuid,
  gps_data_points  gps_data_point_t[],

  constraint laps_pk primary key (lap_start_ts, trip_start_ts, userid),

  constraint laps_fk foreign key (trip_start_ts, userid)
    references trips(trip_start_ts, userid)
    match full on delete cascade on update restrict);
```
**Note:** In PostgreSQL, the maximum number of values that an array of any dimensionality can hold is `(2^27 - 1)` (about 137 million). If you exceed this limit, then you get a clear _"54000: array size exceeds the maximum allowed (134217727)"_ error. This maps to the PL/pgSQL exception _"program_limit_exceeded"_. In PostgreSQL, array values are stored out of line. However, in the YugabyteDB YSQL subsystem, they are stored in line, just like, for example, a `json` or `jsonb` value. As a consequence, the maximum number of values that a YSQL array can accommodate is smaller than the PostgreSQL limit. Moreover, the actual YSQL limit depends on circumstances—and when it's exceeded you get a "time out" error. Experiment shows that the limit is about 30 million values. You can test this for yourself using [`array_fill()`](./functions-operators/array-fill/)) function.

With about 100,000 GPS data points, a 300 km trip is easily accommodated.

The design that stores the GPS points in an array certainly breaks one of the time-honored rules of relational design: that column data types should be scalars. It does, however, bring definite advantages without the correctness risk and loss of functionality that it might in other use cases.

For example, in the classic _"orders"_ and _"order_lines"_ design, an order line is for a quantity of a particular item from the vendor's catalog. And order lines for many different users will doubtless refer to the same catalog item. The catalog item has lots of fields; and some of them (especially the price) sometimes must be updated. Moreover, the overall business context implies queries like this: _find the total number of a specified catalog item that was ordered, by any user, during a specified time period_. Clearly a fully normal Codd-and-Date design is called for here.

It's different with GPS data. The resolution of modern devices is so fine (typically just a few paces, as mentioned) that it's hugely unlikely that two different GPS data points would have the same position. It's even less likely that different point would share the same heart rate and all the other facts that are recorded at each position. In other words it's inconceivable that a query like the example given for the *"orders"* use case (_find the trips, by any user, that all share a common GPS data point_) would be useful. Moreover, all typical uses require fetching a trip and all its GPS data in a single query. One obvious example is to plot the transit of a lap on a map. Another example is to compute the generous containing envelope for a lap so that the set of coinciding lap envelopes can be discovered and analyzed to generate leader board reports and the like. SQL is not up to this kind of computation. Rather, you need procedural code—either in a stored procedure or in a client-side program.

The is use case is taken one step, by using a ragged array-like structure, in [Example use case: GPS trip data (revisited)](./array-of-domains/#example-use-case-gps-trip-data-revisited).

## Organization of the remaining array functionality content

The following sections explain the details about array data types and functionality:

- [The `array[]` value constructor](./array-constructor/)
- [Creating an array value using a literal](./literals/)
- [Built-in SQL functions and operators for arrays](./functions-operators/)
- [Using an array of `DOMAIN` values](./array-of-domains)
