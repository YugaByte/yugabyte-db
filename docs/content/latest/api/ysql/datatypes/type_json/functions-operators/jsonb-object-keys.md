---
title: jsonb_object_keys() and json_object_keys() [JSON]
headerTitle: jsonb_object_keys() and json_object_keys()
linkTitle: jsonb_object_keys() 
description: Transform the list of key names in the supplied JSON object into a set (that is, table) of text values.
menu:
  latest:
    identifier: jsonb-object-keys
    parent: json-functions-operators
    weight: 160
isTocNested: true
showAsideToc: true
---

**Purpose:** Transform the list of key names in the supplied JSON _object_ into a set (that is, table) of `text` values.

**Signature** For the `jsonb` variant:

```
input value:       jsonb
return value:      SETOF text
```

**Notes:** Each function in this pair requires that the supplied JSON value is an _object_. The returned keys are ordered alphabetically.

```plpgsql
do $body$
declare
  object constant jsonb :=
    '{"b": 1, "c": true, "a": {"p":1, "q": 2}}';

  keys text[] := null;

  expected_keys constant text[] :=
    array['a', 'b', 'c'];

  k text;
  n int := 0;
begin
  for k in (
    select jsonb_object_keys(object)
   )
  loop
    n := n + 1;
    keys[n] := k;
  end loop;

  assert
    keys = expected_keys,
  'unexpected';
end;
$body$;
```
