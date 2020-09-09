---
title: jsonb_each_text() and json_each_text() [JSON]
headerTitle: jsonb_each_text() and json_each_text()
linkTitle: jsonb_each_text()
description: Create a row set with columns "key" (as a SQL text) and "value" (as a SQL text) from a JSON object. Useful when the results are primitive values.
menu:
  latest:
    identifier: jsonb-each-text
    parent: json-functions-operators
    weight: 120
isTocNested: true
showAsideToc: true
---

**Purpose:** Create a row set with columns _"key"_ (as a SQL `text`) and _"value"_ (as a SQL `text`) from a JSON _object_.

**Signature** For the `jsonb` variant:

```
input value:       jsonb
return value:      SETOF (text, text)
```

**Notes:** The result of `jsonb_each_text()` bears the same relationship to the result of [`jsonb_each()`](../jsonb-each) as does the result of the [`->>`](../subvalue-operators/) operator to that of the [`->`](../subvalue-operators/) operator. For that reason, `jsonb_each_text()` is useful when the results are primitive values.

Use this `ysqlsh` script to create the required type _"t"_ and then to execute the `ASSERT`.

```plpgsql
create type t as (k text, v text);

do $body$
declare
  object constant jsonb :=
    '{"a": 17, "b": "dog", "c": true, "d": {"a": 17, "b": "cat"}}';

  k_v_pairs t[] := null;

  expected_k_v_pairs constant t[] := 
    array[
      ('a', '17'::text),
      ('b', 'dog'::text),
      ('c', 'true'::text),
      ('d', '{"a": 17, "b": "cat"}'::text)
    ];

  k_v_pair t;
  n int := 0;
begin
  for k_v_pair in (
    select key, value from jsonb_each_text(object)
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

Notice that even here, `17` and `true` are SQL `text` values and not, respectively, a SQL `numeric` value and a SQL `boolean` value. Notice too that the `::text` typecast of the underlying `jsonb` value _dog_ is _"dog"_ (as is produced by [`jsonb_each()`](../jsonb-each)) while when it is read as a SQL `text` value by `jsonb_each_text` the result is _dog_.
