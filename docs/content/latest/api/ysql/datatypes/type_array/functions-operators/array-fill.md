---
title: array_fill()
linkTitle: array_fill()
headerTitle: array_fill()
description: array_fill()
menu:
  latest:
    identifier: array-fill
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---
**Signature:**
```
input value:       anyelement, int[] [, int[]]
return value:      anyarray
```
**Purpose:** Return a new "blank canvas" array of the specified shape with all cells set to the same specified value.

- The first parameter determines the value and data type for every cell, and therefore the data type of the new array as a whole. It can be a value of a primitive data type, or, for example, a _"row"_ type value. It can also be written `NULL::some_type` if this suits your purpose. You would presumably set a `NOT NULL` value if, for example, you wanted to insert the array into a table column on which you have created a constraint, based upon a PL/pgSQL function, that explicitly tests the array's geometric properties and the `NOT NULL` status of each of its values. Try this:
  ```plpgsql
  select pg_typeof(array_fill(null::text, '{1}')) as "type of the new array";
  ```
&#160;&#160;&#160;&#160;This is the result:
  ```
   type of the new array 
  -----------------------
   text[]
  ```
- The second parameter is an `int[]` array. Each of its values specifies the value that `array_length(new_arr, n)` returns—where _"n"_ is the dimension number, starting with the major dimension. So the cardinality of the array that you supply here specifies the value returned by `array_ndims(new_arr)`.
- The third parameter is optional. When supplied, it must be an `int[]` array with the same cardinality as the second parameter. Each of its values specifies the value that `array_lower(new_arr, n)` returns.

The shape of the new array is, therefore, fully specified by the second and third parameters.

**Note:** Why does `array_fill()` exist? In other words, why not just set the values that you want by directly indexing each cell and assigning the value you want to it? Recall that, as described in [Synopsis](../../#synopsis), an array value is rectilinear. This means that its shape, when its number of dimensions exceeds one, is non-negotiably fixed at creation time. This `DO` block emphasizes the point.

```plpgsql
do $body$
declare
  a int[];
  b int[] := array_fill(null::int, '{3, 4}');
begin
  a[1][1] := 42;
  begin
    -- Causes ERROR: array subscript out of range
    a[2][2] := 17;
  exception
    when array_subscript_error then null;
  end;
  raise info
    'cardinality(a), cardinality(b): %, %', cardinality(a), cardinality(b);
end;
$body$;
```

It shows this (after manually stripping the _"INFO:"_ prompt):

```
cardinality(a), cardinality(b): 1, 12
```

So the array _"a"_ is stuck as one dimensional, one-by-one value.

**Example:**

Run this:
```plpgsql
create table t(k int primary key, arr text[]);

insert into t(k, arr)
values(1, array_fill('-----'::text, '{3, 4}', '{2, 7}')::text[]);

select
  array_length(arr, 1)  as len_1,
  array_length(arr, 2)  as len_2,
  array_lower(arr,  1)  as lb_1,
  array_lower(arr,  2)  as lb_2,
  array_ndims(arr)      as ndims,
  cardinality(arr)      as cardinality
from t
where k = 1;
```
It shows this:
```
 len_1 | len_2 | lb_1 | lb_2 | ndims | cardinality 
-------+-------+------+------+-------+-------------
     3 |     4 |    2 |    7 |     2 |          12
```

Now run this:
```plpgsql
update t
set
  arr[2][ 7] = '2---7',
  arr[2][10] = '2--10',
  arr[4][ 7] = '4---7',
  arr[4][10] = '4--10'
where k = 1;

select arr::text from t where k = 1;
```
It shows this (after some manual white-space formatting for readability):
```
[2:4][7:10]=
  {
    {2---7,-----,-----,2--10},
    {-----,-----,-----,-----},
    {4---7,-----,-----,4--10}
  }
```

Finally, run this:
```plpgsql
\set VERBOSITY verbose
update t
set
  arr[1][17] = 'Hmm...'
where k = 1;
```
It reports this error, as expected:
```
2202E: array subscript out of range
```
