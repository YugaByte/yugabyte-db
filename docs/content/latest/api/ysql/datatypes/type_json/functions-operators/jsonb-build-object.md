---
title: jsonb_build_object() and json_build_object()
headerTitle: jsonb_build_object() and json_build_object()
linkTitle: jsonb_build_object() 
description: Build a JSON object from a variadic list that specifies keys with values of arbitrary SQL data type.
menu:
  latest:
    identifier: jsonb-build-object
    parent: json-functions-operators
    weight: 100
isTocNested: true
showAsideToc: true
---

**Purpose:** Create a JSON _object_ from a variadic list that specifies keys with values of arbitrary SQL data type.

**Signature** For the `jsonb` variant:

```
input value:       VARIADIC "any"
return value:      jsonb
```

**Notes:** The key names are given as SQL `text` values. The SQL data type of key's value argument must have a direct JSON equivalent or allow implicit conversion to such an equivalent.

The `jsonb_build_object()` variadic function is the obvious counterpart to [`jsonb_build_array()`](../jsonb-build-array).

The argument list has the form:

```
key1::text, value1::the_data_type1,
key1::text, value2::the_data_type2,
...
keyN::text, valueN::the_data_typeN
```

Use this `ysqlsh` script to create the required type _"t"_ and then to execute the `ASSERT`.

```plpgsql
create type t as (a int, b text);

do $body$
declare
  v_17 constant int := 17;
  v_dog constant text := 'dog';
  v_true constant boolean := true;
  v_t constant t := (17::int, 'cat'::text);

  j constant jsonb := jsonb_build_object(
    'a', v_17,
    'b', v_dog,
    'c', v_true,
    'd', v_t);
  expected_j constant jsonb := 
    '{"a": 17, "b": "dog", "c": true, "d": {"a": 17, "b": "cat"}}';
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```

Just as with [`jsonb_build_array()`](../jsonb-build-array), using `jsonb_build_object()` is straightforward when you know the structure of your target JSON value statically, and just discover the values dynamically. But again, it doesn't accommodate the case that you discover the desired structure dynamically.

The following `ysqlsh` script shows a feasible general workaround for this use case. The helper function _"f()"_ generates the variadic argument list as the text representation of a comma-separated list of SQL literals of various data types. Then it invokes `jsonb_build_object()` dynamically. Obviously this brings a performance cost. But you might not have an alternative.

```plpgsql
create function f(variadic_array_elements in text) returns jsonb
  language plpgsql
as $body$
declare
  stmt text := '
    select jsonb_build_object('||variadic_array_elements||')';
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

  expected_j constant jsonb := jsonb_build_object(
    'a', v_17,
    'b', v_dog,
    'c', v_true,
    'd', v_t);

  j constant jsonb :=  f(
    $$
      'a'::text, 17::integer,
      'b'::text, 'dog'::text,
      'c'::text, true::boolean,
      'd'::text, (17::int, 'cat'::text)::t
    $$);
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```
