---
title: "? and ?| and ?& (key or value existence operators) [JSON]"
headerTitle: "? and ?| and ?& (key or value existence operators)"
linkTitle: "? and ?| and ?& (key or value existence)"
description: Test if JSONB values exist as keys in an object or as string value(s) in array.
menu:
  latest:
    identifier: key-or-value-existence-operators
    parent: json-functions-operators
    weight: 17
isTocNested: true
showAsideToc: true
---

**Purpose:** (1) If the left-hand JSON value is an _object_, test if the right-hand SQL text value(s) exist as key name(s) in the _object_. (2) If the left-hand JSON value is an _array_, test if the right-hand SQL text value(s) exist as JSON _string_ value(s) in the _array_.

**Notes:** Each of these operators requires that the input is presented as `jsonb` value. There are no `json` overloads. The first variant allows a single `text` value to be provided. The second and third variants allow a list of `text` values to be provided. The second is the _or_ (any) flavor and the third is the _and_ (all) flavor.

## The&#160; &#160;?&#160; &#160;operator

**Purpose:** If the left-hand JSON value is an _object_ , test if it has a key-value pair with a _key_ whose name is given by the right-hand scalar `text` value. If the left-hand JSON value is an _array_ test if it has a _string_ value given by the right-hand scalar `text` value.

**Signature:**

```
input values:       jsonb ? text
return value:       boolean
```

**Input is an _object_:**

Here, the existence expression evaluates to `TRUE` so the  `ASSERT` succeeds:

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": "x", "b": "y"}';
  key constant text := 'a';
begin
  assert
    j ? key,
  'unexpected';
end;
$body$;
```

Here, the existence expression for this counter-example evaluates to `FALSE` because the left-hand JSON value has _"x"_ as a _value_ and not as a _key_.

````plpgsql
do $body$
declare
  j constant jsonb := '{"a": "x", "b": "y"}';
  key constant text := 'x';
begin
  assert
    not (j ? key),
  'unexpected';
end;
$body$;
````

Here, the existence expression for this counter-example evaluates to `FALSE` because the left-hand JSON value has the _object_ not at top-level but as the second subvalue in a top-level _array_:

````plpgsql
do $body$
declare
  j constant jsonb := '[1, {"a": "x", "b": "y"}]';
  key constant text := 'a';
begin
  assert
    not (j ? key),
  'unexpected';
end;
$body$;
````

**Input is an _array_:** 

```plpgsql
do $body$
declare
  j_str_arr constant jsonb := '["cat", "dog", "from"]';
  t_string  constant text  := 'dog';
begin
  assert
    j_str_arr ? t_string,
  'unexpected';
end;
$body$;
```

**Further clarification of semantics:**

The only possible (and useful) match for the right-hand scalar SQL `text` value is a _key_ name in an _object_ or a _string_ value in an _array_. Here are the counter-examples for the other primitive and compound JSON values:
```plpgsql
do $body$
declare
  -- Positive test
  j_string  constant jsonb := '"v1"';
  t_string  constant text  := 'v1';

  -- Negative tests
  j_number  constant jsonb := '42';
  j_boolean constant jsonb := 'true';
  j_null    constant jsonb := 'null';
  j_object  constant jsonb := '{"k1": "v1"}';
  j_array constant jsonb   := '[1, 2, 3]';

  t_number  constant text := '42';
  t_boolean constant text := 'true';
  t_null    constant text := 'null';
  t_object  constant text := '{"k1": "v1"}';
  t_array   constant text := '[1, 2, 3]';
begin
  assert
       (j_string  ? t_string ) and
    not(j_number  ? t_number ) and
    not(j_boolean ? t_boolean) and
    not(j_boolean ? t_boolean) and
    not(j_null    ? t_null   ) and
    not(j_object  ? t_object ) and
    not(j_array   ? t_array  ),
  'unexpected';
end;
$body$;
```
Notice that this test:
```
(j_string  ? t_string )
```
where the left hand value is a primitive JSON _string_, has no practical utility. It is included just to demonstrate the level of granularity at which the test is applied for the "exists as value in _array_" use of the `?` operator.

## The&#160; &#160;?|&#160; &#160;operator

**Purpose:** If the left-hand JSON value is an _object_, test if it has _at least one_ key-value pair where the key name is present in the right-hand list of scalar `text` values. If the left-hand JSON value is an _array_, test if it has _at least one_ _string_ value that is present in the right-hand list of scalar `text` values.

**Signature:**

```
input values:       jsonb ?| text[]
return value:       boolean
```
**Input is an _object_:**

Here, the existence expression evaluates to `TRUE`.
```plpgsql
do $body$
declare
  j constant jsonb := '{"a": "x", "b": "y", "c": "z"}';
  key_list constant text[] := array['a', 'p'];
begin
  assert
    j ?| key_list,
  'unexpected';
end;
$body$;
```

Here, the existence expression for this counter-example evaluates to `FALSE` because none of the `text` values in the right-hand array exists as the key of a key-value pair.

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": "x", "b": "y", "c": "z"}';
  key_list constant text[] := array['x', 'p'];
begin
  assert
    not (j ?| key_list),
  'unexpected';
end;
$body$;
```

(_'x'_ in the right-hand key list exists only as a primitive _string_ value for the key _"a"_.)

**Input is an _array_:**

Here, the existence expression evaluates to `TRUE`.

```plpgsql
do $body$
declare
  j constant jsonb := '["a", "b", "c"]';
  key_list constant text[] := array['a', 'p'];
begin
  assert
    (j ?| key_list),
  'unexpected';
end;
$body$;
```

## The&#160; &#160;?&&#160; &#160;operator

**Purpose:** If the left-hand JSON value is an _object_, test if _every_ value in the right-hand list of scalar `text` values is present as the name of the key of a key-value pair. If the left-hand JSON value is an _array_, test if _every_ value in the right-hand list of scalar `text` values is present as a _string_ value in the _array_.

**Signature:**

```
input values:       jsonb ?& text[]
return value:       boolean
```

**Input is an _object_:**

Here, the existence expression evaluates to _true_:

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": "w", "b": "x", "c": "y", "d": "z"}';
  key_list constant text[] := array['a', 'b', 'c'];
begin
  assert
    j ?& key_list,
  'unexpected';
end;
$body$;
```

Here, the existence expression for this counter-example evaluates to `FALSE` because _'z'_ in the right-hand key list exists as the value of a key-value pair, but not as the key of such a pair.

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": "x", "b": "y", "c": "z"}';
  key_list constant text[] := array['a', 'b', 'z'];
begin
  assert
    not(j ?& key_list),
  'unexpected';
end;
$body$;
```
**Input is an _array_:**

Here, existence expression evaluates to _true_:

```plpgsql
do $body$
declare
  j constant jsonb := '["a", "b", "c", "d"]';
  key_list constant text[] := array['a', 'b', 'c'];
begin
  assert
    j ?& key_list,
  'unexpected';
end;
$body$;
```

