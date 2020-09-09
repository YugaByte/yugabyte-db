---
title: jsonb_each() and json_each() [JSON]
headerTitle: jsonb_each() and json_each()
linkTitle: jsonb_each()
description: Create a row set with columns "key" (as a SQL text) and "value" (as a SQL jsonb) from a JSON object.
menu:
  latest:
    identifier: jsonb-each
    parent: json-functions-operators
    weight: 110
isTocNested: true
showAsideToc: true
---

**Purpose:** Create a row set with columns _"key"_ (as a SQL `text`) and _"value"_ (as a SQL `jsonb`) from a JSON _object_.

**Signature** For the `jsonb` variant:

```
input value:       jsonb
return value:      SETOF (text, jsonb)
```

Use this `ysqlsh` script to create the required type _"t"_ and then to execute the `ASSERT`.

```plpgsql
create type t as (k text, v jsonb);

do $body$
declare
  object constant jsonb :=
    '{"a": 17, "b": "dog", "c": true, "d": {"a": 17, "b": "cat"}}';

  k_v_pairs t[] := null;
  expected_k_v_pairs constant t[] := 
    array[
      ('a', '17'::jsonb),
      ('b', '"dog"'::jsonb),
      ('c', 'true'::jsonb),
      ('d', '{"a": 17, "b": "cat"}'::jsonb)
    ];

  k_v_pair t;
  n int := 0;
begin
  for k_v_pair in (
    select key, value from jsonb_each(object)
    )
  loop
    n := n + 1;
    k_v_pairs[n] := k_v_pair;
  end loop;

  assert
    k_v_pairs = expected_k_v_pairs,
  'unexpected';
end;
$body$;
```
