---
title: jsonb_array_length() and json_array_length()
linkTitle: jsonb_array_length()
headerTitle: jsonb_array_length() and json_array_length()
description: Return the count of values in an array using jsonb_array_length() and json_array_length().
menu:
  latest:
    identifier: jsonb-array-length
    parent: json-functions-operators
    weight: 80
isTocNested: true
showAsideToc: true
---

**Purpose:** Return the count of values (primitive or compound) in the array. You can use this to iterate over the elements of a JSON _array_ using the [`->`](../subvalue-operators/) operator.

**Signature** For the `jsonb` variant:

```
input value:       jsonb
return value:      integer
```

**Notes:** Each function in this pair requires that the supplied JSON value is an _array_.

```plpgsql
do $body$
declare
  j constant jsonb := '["a", 42, true, null]';
  last_idx constant int := (jsonb_array_length(j) - 1);

  expected_typeof constant text[] :=
    array['string', 'number', 'boolean', 'null'];
begin
  for n in 0..last_idx loop
    assert
      jsonb_typeof(j -> n) = expected_typeof[n + 1],
    'unexpected';
  end loop;
end;
$body$;
```

This example uses the [`jsonb_typeof()`](../jsonb-typeof) function.

Reading the values themselves would need to use a `case` statement that tests the emergent JSON data type and that selects the leg whose assignment target has the right SQL data type. This is straightforward only for primitive JSON values. If a compound JSON value is encountered, then it must be decomposed, recursively, until the ultimate JSON primitive value leaves are reached.

This complexity reflects the underlying impedance mismatch between the JSON type system and the SQL type system. Introspecting a JSON value when you have no _a priori_ understanding of its structure is tricky.
