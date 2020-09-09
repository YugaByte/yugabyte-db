---
title: jsonb_array_elements() and  json_array_elements()
linkTitle: jsonb_array_elements()
headerTitle: jsonb_array_elements() and json_array_elements()
description: Transform JSON values of a JSON array into a SQL table of jsonb values using jsonb_array_elements() and  json_array_elements().
block_indexing: true
menu:
  stable:
    identifier: jsonb-array-elements
    parent: json-functions-operators
    weight: 60
isTocNested: true
showAsideToc: true
---
**Purpose:** Transform the JSON values of a JSON _array_ into a SQL table of (i.e., `SETOF`) `jsonb` values.

**Signature:** For the `jsonb` variant:

```
input value:       jsonb
return value:      SETOF jsonb
```

**Notes:** Each function in this pair requires that the supplied JSON value is an _array_. They are the counterparts, for an _array_, to [`jsonb_populate_recordset()`](../jsonb-populate-recordset) for a JSON _object_.

Notice that the JSON value _null_ becomes a genuine SQL `NULL`. However, SQL array comparison and `record` comparison use `IS NOT DISTINCT FROM` semantics, and not the semantics that the comparison of scalars uses. So the simple `ASSERT` that `elements = expected_elements` is `TRUE` is sufficient. See the section [Operators for comparing two arrays](../../..//type_array/functions-operators/comparison/).

```plpgsql
do $body$
declare
  j_array constant jsonb := '["cat", "dog house", 42, true, {"x": 17}, null]';
  j jsonb;

  elements jsonb[];
  expected_elements constant jsonb[] :=
    array[
      '"cat"'::jsonb,
      '"dog house"'::jsonb,
      '42'::jsonb,
      'true'::jsonb,
      '{"x": 17}'::jsonb,
      'null'::jsonb
    ];

  n int := 0;
begin
  for j in (select jsonb_array_elements(j_array)) loop
    n := n + 1;
    elements[n] := j;
  end loop;

  assert
    elements = expected_elements,
  'unexpected';
end;
$body$;
```
