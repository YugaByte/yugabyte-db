---
title: to_jsonb() and to_json()
headerTitle: to_jsonb()
linkTitle: to_jsonb()
description: Convert a single SQL value of any primitive or compound data type, that allows a JSON representation, to a semantically equivalent jsonb value.
menu:
  latest:
    identifier: to-jsonb
    parent: json-functions-operators
isTocNested: true
showAsideToc: true
---

**Purpose:** Convert a single SQL value of any primitive or compound data type, that allows a JSON representation, to a semantically equivalent `jsonb` value.

**Signature** For the `jsonb` variant:

```
input value:       anyelement
return value:      jsonb
```

Use this `ysqlsh` script to create types _"t1"_ and _"t2"_ and then to execute the `DO` block that asserts that the behavior is as expected. For an arbitrary nest of SQL `record` and SQL array values, readability is improved by building the compound value from the bottom up.

```plpgsql
create type t1 as(a int, b text);
create type t2 as(x text, y boolean, z t1[]);

do $body$
declare
  j1_dog  constant jsonb := to_jsonb('In the'||Chr(10)||'"dog house"'::text);
  j2_dog  constant jsonb := '"In the\n\"dog house\""';

  j1_42   constant jsonb := to_jsonb(42::numeric);
  j2_42   constant jsonb := '42';

  j1_true   constant jsonb := to_jsonb(true::boolean);
  j2_true   constant jsonb := 'true';

  j1_null   constant jsonb := to_jsonb(null::boolean);
  j2_null   constant jsonb := 'null';

  j1_array constant jsonb := to_jsonb(array['a', 42, true]::text[]);
  j2_array  constant jsonb := '["a", "42", "true"]';

  v1 t1   := (17::int, 'dog'::text);
  v2 t1   := (42::int, 'cat'::text);
  v3 t1[] := array[v1, v2];
  v4 t2   := ('frog', true, v3);
  j1_object constant jsonb := to_jsonb(v4::t2);
  j2_object constant jsonb :=
    '{"x": "frog",
      "y": true,
      "z": [{"a": 17, "b": "dog"}, {"a": 42, "b": "cat"}]}';
begin
assert
    j1_dog    = j2_dog   and
    j1_42     = j2_42    and
    j1_true   = j2_true  and
    j1_array  = j2_array and
    j1_object = j2_object,
  'unexpected';
end;
$body$;
```
