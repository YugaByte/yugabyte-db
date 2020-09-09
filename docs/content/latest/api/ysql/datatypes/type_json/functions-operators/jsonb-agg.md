---
title: jsonb_agg()
headerTitle: jsonb_agg()
linkTitle: jsonb_agg()
description: Aggregate a SETOF values into a JSON array.
menu:
  latest:
    identifier: jsonb-agg
    parent: json-functions-operators
    weight: 57
isTocNested: true
showAsideToc: true
---

**Purpose:** This is an aggregate function. (Aggregate functions compute a single result from a `SETOF` input SQL values.) It creates a JSON _array_ whose values are the JSON representations of the aggregated SQL values.

**Signature:**

```
input value:       SETOF anyelement
return value:      jsonb
```

**Notes:** The syntax _"order by column1 nulls first"_ within the parentheses of the aggregate function is not specific to `jsonb_agg()`. Rather, it is a generic feature of aggregate functions. The same syntax is used with the [array_agg()`](../../../type_array/functions-operators/array-agg-unnest/#array-agg) function for SQL arrays.

```plpgsql
create type rt as (key1 int, key2 text);

do $body$
declare
  agg jsonb;

  expected_agg constant jsonb not null := '[
      {"key1": null, "key2": "ant"},
      {"key1": 1,    "key2": "dog"},
      {"key1": 2,    "key2": "cat"},
      {"key1": 4,    "key2": null },
      {"key1": 5,    "key2": "ant"}
    ]'::jsonb;
begin
  with tab as (
    values
      (5::int,    'ant'::text),
      (2::int,    'cat'::text),
      (null::int, 'ant'::text),
      (1::int,    'dog'::text),
      (4::int,     null::text))
  select
  json_agg((column1, column2)::rt order by column1 nulls first)
  into strict agg
  from tab;

  assert (agg = expected_agg), 'unexpected';
end;
$body$;
```
You can also aggregate SQL arrays into a ragged JSON _array_ of JSON _arrays_ like this:
```plpgsql
do $body$
declare
  agg jsonb not null := '"?"';

  expected_agg constant jsonb not null := '[
      ["a", "b", "c"],
      ["d"          ],
      ["e", "f"     ]
    ]'::jsonb;
begin
  with tab as (
    values
      (array['a', 'b', 'c']::text[]),
      (array['d'          ]::text[]),
      (array['e', 'f'     ]::text[]))
  select
  json_agg((column1)::text[] order by column1 nulls first)
  into strict agg
  from tab;

  assert (agg = expected_agg), 'unexpected';
end;
$body$;
```
