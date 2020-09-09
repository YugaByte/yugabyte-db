---
title: jsonb_populate_record() and json_populate_record() 
headerTitle: jsonb_populate_record() and json_populate_record()
linkTitle: jsonb_populate_record()
description: Convert a JSON object into the equivalent SQL record.
menu:
  latest:
    identifier: jsonb-populate-record
    parent: json-functions-operators
    weight: 180
isTocNested: true
showAsideToc: true
---

**Purpose:** Convert a JSON _object_ into the equivalent SQL `record`.

**Signature** For the `jsonb` variant:

```
input value:       anyelement, jsonb
return value:      anyelement
```

**Notes:** Each function in this pair requires that the supplied JSON value is an _object_. The data type of the `record` must be defined as a schema-level `type` whose name is passed via the function's first formal parameter using the locution `NULL:type_identifier`. The JSON value is passed using the second formal parameter.

Use this `ysqlsh` script to create the required types _"t1"_ and _"t2"_, and then to execute the `ASSERT`.

```
create type t1 as ( d int, e text);
create type t2 as (a int, b text[], c t1);

do $body$
declare
  nested_object constant jsonb :=
    '{"a": 42, "b": ["cat", "dog"], "c": {"d": 17, "e": "frog"}}';

  result constant t2 := jsonb_populate_record(null::t2, nested_object);

  expected_a constant int := 42;
  expected_b constant text[] := array['cat', 'dog'];
  expected_c constant t1 := (17, 'frog');
  expected_result constant t2 := (expected_a, expected_b, expected_c);
begin
  assert
    result = expected_result,
  'unexpected';
end;
$body$;
```

