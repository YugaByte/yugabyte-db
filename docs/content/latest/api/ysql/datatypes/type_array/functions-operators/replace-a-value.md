---
title: array_replace()
linkTitle: array_replace() / set value
headerTitle: array_replace() and setting an array value explicitly
description: array_replace() and setting an array value explicitly
menu:
  latest:
    identifier: array-replace-a-value
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---
Each of the approaches described in this section, using the `array_replace()` function and setting an addressed array value explicitly and in place, can be used to change values in an array. But the two approaches differ importantly:

- `array_replace()` changes all values that match the specified value to the same new value, insensitively to their address in the array.

- Setting an addressed array value changes that one value insensitively to its present value.

## array_replace()

**Purpose:** Return a new array that is derived from the input array by replacing _every_ array value that is equal to the specified value with the specified new value.

**Signature** 

```
input value:       anyarray, anyelement, anyelement
return value:      anyarray
```
**Example:**

```plpgsql
create type rt as (f1 int, f2 text);
create table t(k int primary key, arr rt[]);
insert into t(k, arr)
values (1, '{"(1,rabbit)","(2,hare)","(3,squirrel)","(4,horse)"}'::rt[]);

select arr as "old value of arr" from t where k = 1;

update t
set arr = array_replace(arr, '(3,squirrel)', '(3,bobcat)')
where k = 1;

select arr as "new value of arr" from t where k = 1;
```
This is the result of the two queries:
```
                   old value of arr                   
------------------------------------------------------
 {"(1,rabbit)","(2,hare)","(3,squirrel)","(4,horse)"}

                  new value of arr                  
----------------------------------------------------
 {"(1,rabbit)","(2,hare)","(3,bobcat)","(4,horse)"}
```

**Semantics:**

_One-dimensional array of primitive scalar values_.

```plpgsql
do $body$
declare
  old_val constant int := 42;
  new_val constant int := 17;

  arr constant int[] :=
    array[1, old_val, 3, 4, 5, old_val, 6, 7];

  expected_modified_arr constant int[] :=
    array[1, new_val, 3, 4, 5, new_val, 6, 7];
begin
  assert
    array_replace(arr, old_val, new_val) = expected_modified_arr,
  'unexpected';
end;
$body$;
```

_One-dimensional array of _"row"_ type values_.

The definition of _"rt"_ used here is the same as the example above used. Don't create again if it already exists.

```plpgsql
create type rt as (f1 int, f2 text);

do $body$
declare
  old_val constant rt := (42, 'x');
  new_val constant rt := (17, 'y');

  arr constant rt[] :=
    array[(1, 'a')::rt, old_val, (1, 'a')::rt, (2, 'b')::rt, (3, 'c')::rt, 
                        old_val, (4, 'd')::rt, (5, 'e')::rt];

  expected_modified_arr constant rt[] :=
    array[(1, 'a')::rt, new_val, (1, 'a')::rt, (2, 'b')::rt, (3, 'c')::rt, 
                        new_val, (4, 'd')::rt, (5, 'e')::rt];
begin
  assert
    array_replace(arr, old_val, new_val) = expected_modified_arr,
  'unexpected';
end;
$body$;
```

_Two-dimensional array of primitive scalar values_. This is sufficient to illustrate the semantics of the general multidimensional case. The function's signature (at the start of this section) shows that the to-be-replaced value and the replacement value are instances of `anyelement`. There is no overload where these two parameters accept instances of `anyarray`. This restriction is understood by picturing the internal representation as a linear ribbon of values, as was explained in [Synopsis](../../#synopsis). The replacement works by scanning along the ribbon, finding each occurrence in turn of the to-be-replaced value, and replacing it.

Here is a postive illustration:
```plpgsql
do $body$
declare
  old_val constant int := 22;
  new_val constant int := 97;

  arr int[] :=
    array[
       array[11, 12],
       array[11, old_val],
       array[32, 33]
    ];

  expected_modified_arr constant int[] :=
    array[
       array[11, 12],
       array[11, new_val],
       array[32, 33]
    ];
begin
  arr := array_replace(arr, old_val, new_val);

  assert
    arr = expected_modified_arr,
  'unexpected';
end;
$body$;
```
And here is a negative illustration:
```plpgsql
do $body$
declare
  old_val constant int[] := array[22, 23];
  new_val constant int[] := array[87, 97];

  arr int[] :=
    array[
       array[11, 12],
       old_val,
       array[32, 33]
    ];
  expected_modified_arr constant int[] :=
    array[
       array[11, 12],
       new_val,
       array[32, 33]
    ];
begin
  begin
    -- Causes: 42883: function array_replace(integer[], integer[], integer[]) does not exist.
    arr := array_replace(arr, old_val, new_val);
  exception
    when undefined_function then null;
  end;

  -- The goal is met by replacing the scalar values one by one.
  arr := array_replace(arr, old_val[1], new_val[1]);
  arr := array_replace(arr, old_val[2], new_val[2]);

  assert
    arr = expected_modified_arr,
  'unexpected';
end;
$body$;
```

## Setting an array value explicitly and in place

**Purpose:** Change an array in place by changing an explicitly addressed value.

**Signature** 

```
-- Uses the notation
--   arr[idx_1][idx_2]...[idx_N]
-- for an N-dimensional array.

input/output value: anyarray, "vector of index values"
```
**Example:**
```plpgsql
create table t(k int primary key, arr int[]);

insert into t(k, arr) values (1,
  '{1, 2, 3, 4}');

update t set arr[2] = 42 where k = 1;

select arr from t where k = 1;
```
This is the result:
```
    arr     
------------
 {1,42,3,4}
```
**Semantics:**

_Array of primitive scalar values_. Notice that the starting value is "snapshotted" as `old_arr` and that this is marked `constant`. Notice too that _"expected_modified_arr"_ is marked `constant`. This proves that the modification was done in place within the only array value that is _not_ marked `constant`.

```plpgsql
do $body$
declare
  old_val constant int := 42;
  new_val constant int := 17;

  arr int[] :=                            array[1, 2, old_val, 4];
  expected_modified_arr constant int[] := array[1, 2, new_val, 4];
  old_arr constant int[] := arr;
begin
  arr[3] := new_val;
  assert
    old_arr =               '{1, 2, 42, 4}' and
    expected_modified_arr = '{1, 2, 17, 4}' and
    arr = expected_modified_arr,
  'unexpected';
end;
$body$;
```
_Array of "record" type values_.

The definition of _"rt"_ used here is the same as the example above used. Don't create again if it already exists.
```plpgsql
create type rt as (f1 int, f2 text);

do $body$
declare
  old_val constant rt := (42, 'x');
  new_val constant rt := (17, 'y');

  arr rt[] :=
    array[(1, 'a')::rt, old_val, (1, 'a')::rt, (2, 'b')::rt, (3, 'c')::rt, 
                        old_val, (4, 'd')::rt, (5, 'e')::rt];

  expected_modified_arr constant rt[] :=
    array[(1, 'a')::rt, new_val, (1, 'a')::rt, (2, 'b')::rt, (3, 'c')::rt, 
                        new_val, (4, 'd')::rt, (5, 'e')::rt];

  old_arr constant rt[] := arr;
begin
  arr[2] := new_val;
  arr[6] := new_val;

  assert
    old_arr =
      '{"(1,a)","(42,x)","(1,a)","(2,b)","(3,c)","(42,x)","(4,d)","(5,e)"}' and
    expected_modified_arr =
      '{"(1,a)","(17,y)","(1,a)","(2,b)","(3,c)","(17,y)","(4,d)","(5,e)"}' and
    arr = expected_modified_arr,
  'unexpected';
end;
$body$;
```
_Two-dimensional array of primitive scalar values_. This is sufficient to illustrate the semantics of the general multidimensional case. The approach is just the same as when `array_replace()` is used to meet the same goal. You have no choice but to target the values explicitly.

```plpgsql
do $body$
declare
  old_val constant int[] := array[21, 22, 23, 24];
  new_val constant int[] := array[81, 82, 83, 84];

  arr int[] :=
    array[
       array[11, 12, 13, 14],
       old_val,
       array[31, 32, 33, 34]
    ];

  expected_modified_arr constant int[] :=
    array[
       array[11, 12, 13, 14],
       new_val,
       array[31, 32, 33, 34]
    ];

  len_1 constant int := array_length(arr, 1);
  len_2 constant int := array_length(arr, 2);
begin
  assert
    (len_1 = 3) and (len_2 = 4),
  'unexpected';
  
  -- OK to extract a slice. But, even though it's tempting to picture this as one row,
  -- it is nevertheless a 2-d array with "array_length(arr, 1)" equal to 1.
  assert
    arr[2:2][1:4] = array[old_val],
  'unexpected';

  -- You cannot use the slice notation to specify the target of an assignment.
  -- So this 
  --   arr[2:2][1:4] = array[new_val];
  -- causes a compilation error.

  -- Similarly, this is meaningless. (But it doesn't cause a compilation error.)
  -- Because it's a 2-d array, its values (individual values or slices) must be
  -- addressed using two indexes or two slice ranges.
  assert
    arr[2] is null,
  'unexpected';

  -- Change the individual, addressable, values one by one.
  for j in array_lower(arr, 2)..array_upper(arr, 2) loop
    arr[2][j] := new_val[j];
  end loop; 

  assert
    arr = expected_modified_arr,
  'unexpected';
end;
$body$;
```
