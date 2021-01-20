---
title: jsonb_build_array() and json_build_array()
headerTitle: jsonb_build_array() and json_build_array()
linkTitle: jsonb_build_array()
description: Build a JSON array from a variadic list of array values of arbitrary SQL data type.
menu:
  latest:
    identifier: jsonb_build_array-each
    parent: json-functions-operators
    weight: 110
isTocNested: true
showAsideToc: true
---


**Purpose:** Create a JSON _array_ from a variadic list of _array_ values of arbitrary SQL data type.

**Signature** For the `jsonb` variant:

```
input value:       VARIADIC "any"
return value:      jsonb
```

**Notes:** Each function in this pair takes an arbitrary number of actual arguments of mixed SQL data types. The data type of each argument must have a direct JSON equivalent or allow implicit conversion to such an equivalent.

Use this `ysqlsh` script to create the required type _"t"_ and then to execute the `ASSERT`.

```plpgsql
create type t as (a int, b text);

do $body$
declare
  v_17 constant int := 17;
  v_dog constant text := 'dog';
  v_true constant boolean := true;
  v_t constant t := (17::int, 'cat'::text);

  j constant jsonb := jsonb_build_array(v_17, v_dog, v_true, v_t);
  expected_j constant jsonb := '[17, "dog", true, {"a": 17, "b": "cat"}]';
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```

Using `jsonb_build_array()` is straightforward when you know the structure of your target JSON value statically, and just discover the values dynamically. However, it doesn't accommodate the case that you discover the desired structure dynamically.

The following `ysqlsh` script shows a feasible general workaround for this use case. The helper function _"f()"_ generates the variadic argument list as the text representation of a comma-separated list of SQL literals of various data types. Then it invokes `jsonb_build_array()` dynamically. Obviously this brings a performance cost. But you might not have an alternative.

```plpgsql
create function f(variadic_array_elements in text) returns jsonb
  language plpgsql
as $body$
declare
  stmt text := '
    select jsonb_build_array('||variadic_array_elements||')';
  j jsonb;
begin
  execute stmt into j;
  return j;
end;
$body$;

-- Relies on "type t as (a int, b text)" created above.
do $body$
declare
  v_17 constant int := 17;
  v_dog constant text := 'dog';
  v_true constant boolean := true;
  v_t constant t := (17::int, 'cat'::text);

  expected_j constant jsonb := jsonb_build_array(v_17, v_dog, v_true, v_t);
  j constant jsonb := f(
    $$
      17::integer,
      'dog'::text,
      true::boolean,
      (17::int, 'cat'::text)::t
    $$
    );
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```
