---
title: = (equality operator) [JSON]
headerTitle: = (equality operator)
linkTitle: = (equality)
description: Test if two jsonb values are equal using the JSON equality operator (=).
menu:
  latest:
    identifier: equality-operator
    parent: json-functions-operators
    weight: 15
isTocNested: true
showAsideToc: true
---

**Purpose:** Test if two `jsonb` values are equal.

**Signature:**

```
input values:       jsonb = jsonb
return value:       boolean
```

**Notes:** There is no `json` overload. If you want to test that two `json` values are equal, express the predicate thus:

```
lhs_json_value::text = rhs_json_value::text
```

Example:

```plpgsql
do $body$
declare
  j1 constant jsonb := '["a","b","c"]';
  j2 constant jsonb := '
    [
      "a","b","c"
    ]';
begin
  assert
    j1 = j2,
  'unexpected';
end;
$body$;
```

Notice that the text definitions of the to-be-compared JSON values may differ in whitespace. Because `jsonb` holds a fully parsed representation of the value, whitespace (except within primitive JSON _string_ values and key names) has no meaning.

If you need to test two `json` values for equality, then you must `::text` typecast each.

See the account of the `::text` operator when the input is a `json` value. The `json` representation preserves semantically insignificant whitespace and repeats occurrences of the same keys in an _object_. This implies that the equality comparison of two `json` values would in general be unpredictable and therefore meaningless. This is why the `=` operator doesn't have a `json` overload and is is another reason to prefer consistently to choose to use `jsonb`.

```plpgsql 
do $body$
declare
  j1 constant json := '["a","b","c"]';
  j2 constant json := '["a","b","c"]';
  j3 constant json := '["a","b", "c"]';

begin
  assert
    (j1::text = j2::text) and
    not (j1::text = j3::text),
  'unexpected';
end;
$body$;
```
