---
title: "- and #- (remove operators) [JSON]"
headerTitle: "- and #- (remove operators)"
linkTitle: "- and #- (remove)"
description: Remove key-value pairs from an object or remove a single value from an array.
menu:
  latest:
    identifier: remove-operators
    parent: json-functions-operators
    weight: 13
isTocNested: true
showAsideToc: true
---

**Purpose:** Remove key-value pairs from an _object_ or a single value from an _array_. The plain `-` variant takes the specified object itself. The `#-` variant takes the path from the specified object.

**Notes:** Describing the behavior by using the term "remove" is a convenient shorthand. The actual effect of these operators is to create a _new_ `jsonb` value from the specified `jsonb` value according to the rule that the operator implements, parameterized by the SQL value on the right of the operator.

## The&#160; &#160;-&#160; &#160;operator

**Purpose:** Remove key-value pairs from an _object_ or a single value from an _array_.

**Signature:**

```
input values:       jsonb - [int | text]
return value:       jsonb
```

**Notes:** There is no `json` overload.

To remove a single key-value pair:

```plpgsql
do $body$
declare
  j_left constant jsonb := '{"a": "x", "b": "y"}';
  key constant text := 'a';
  j_expected constant jsonb := '{"b": "y"}';
begin
  assert
    j_left - key = j_expected,
 'unexpected';
end;
$body$;
```

To remove several key-value pairs:

```plpgsql
do $body$
declare
  j_left constant jsonb := '{"a": "p", "b": "q", "c": "r"}';
  key_list constant text[] := array['a', 'c'];
  j_expected constant jsonb := '{"b": "q"}';
begin
  assert
    j_left - key_list = j_expected,
 'unexpected';
end;
$body$;
```

To remove a single value from an _array_:

```plpgsql
do $body$
declare
  j_left constant jsonb := '[1, 2, 3, 4]';
  idx constant int := 0;
  j_expected constant jsonb := '[2, 3, 4]';
begin
  assert
    j_left - idx = j_expected,
 'unexpected';
end;
$body$;
```

There is no direct way to remove several values from an _array_ at a list of indexes, analogous to the ability to remove several key-value pairs from an _object_ with a list of pair keys. The obvious attempt fails with this error:

```
operator does not exist: jsonb - integer[]
```

You can achieve the result thus:

```plpgsql
do $body$
declare
  j_left constant jsonb := '[1, 2, 3, 4, 5, 7]';
  idx constant int := 0;
  j_expected constant jsonb := '[4, 5, 7]';
begin
  assert
    ((j_left - idx) - idx) - idx = j_expected,
 'unexpected';
end;
$body$;
```

## The&#160; &#160;#-&#160; &#160;operator

**Purpose:** Remove a single key-value pair from an _object_ or a single value from an _array_ at the specified path.

**Signature:**

```
input values:       jsonb - text[]
return value:       jsonb
```

**Notes:** There is no `json` overload.

```plpgsql
do $body$
declare
  j_left constant jsonb := '["a", {"b":17, "c": ["dog", "cat"]}]';
  path constant text[] := array['1', 'c', '0'];
  j_expected constant jsonb := '["a", {"b": 17, "c": ["cat"]}]';
begin
  assert
    j_left #- path = j_expected,
 'assert failed';
end;
$body$;
```

Just as with the [`#>` and `#>>` operators](../subvalue-operators/), array index values are presented as convertible `text` values. Notice that the address of each JSON array element along the path is specified JSON-style, where the index starts at zero.