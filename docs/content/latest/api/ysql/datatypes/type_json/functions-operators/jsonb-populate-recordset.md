---
title: jsonb_populate_recordset() and json_populate_recordset()
headerTitle: jsonb_populate_recordset() and json_populate_recordset()
linkTitle: jsonb_populate_recordset()
description: Convert a homogeneous JSON array of JSON objects into the equivalent set of SQL records.
menu:
  latest:
    identifier: jsonb-populate-recordset
    parent: json-functions-operators
    weight: 190
isTocNested: true
showAsideToc: true
---

**Purpose:** Convert a homogeneous JSON _array_ of JSON _objects_ into the equivalent set of SQL _records_.

**Signature** For the `jsonb` variant:

```
input value:       anyelement, jsonb
return value:      SETOF anyelement
```

**Notes:** The functions in this pair and are a natural extension of the functionality of [`jsonb_populate_record()`](../jsonb-populate-record).

Each requires that the supplied JSON value is an _array_, each of whose values is an _object_ which is compatible with the specified SQL `record` which is defined as a `type` whose name is passed via the function's first formal parameter using the locution `NULL:type_identifier`. The JSON value is passed via the second formal parameter. The result is a set (i.e. a table) of `record`s of the specified type.

Use this `ysqlsh` script to create the  type _"t"_, and then to execute the `ASSERT`.

{{< note title="Record and array comparison" >}}
Notice the because the result is a table, it must be materialized in a `cursor for loop`. Each selected row is accumulated in an array of type `t[]`. The expected result is also established in an array of type `t[]`. The input JSON _array_ has been contrived, by sometimes not having a key `"a"` or a key `"b"` so that the resulting records sometimes have `NULL` fields. Record comparison, and array comparison, both use `IS NOT DISTINCT FROM` semantics—unlike is the case for scalar comparison. This means that the `ASSERT` can use a simple equality test to compare _"rows"_ and _"expected_rows"_. See the section [Operators for comparing two arrays](../../..//type_array/functions-operators/comparison/).
{{< /note >}}

```plpgsql
create type t as (a int, b int);

do $body$
declare
  array_of_objects constant jsonb :=
    '[
      {"a": 1, "b": 2},
      {"b": 4, "a": 3},
      {"a": 5, "c": 6},
      {"b": 7, "d": 8},
      {"c": 9, "d": 0}
    ]';

  rows t[];
  expected_rows constant t[] :=
    array[
      (   1,    2)::t,
      (   3,    4)::t,
      (   5, null)::t,
      (null,    7)::t,
      (null, null)::t
    ];
  row t;
  n int := 0;
begin
  for row in (
    select a, b
    from jsonb_populate_recordset(null::t, array_of_objects)
    )
  loop
    n := n + 1;
    rows[n] := row;
  end loop;

  assert
    (rows = expected_rows),
  'unexpected';
end;
$body$;
```
