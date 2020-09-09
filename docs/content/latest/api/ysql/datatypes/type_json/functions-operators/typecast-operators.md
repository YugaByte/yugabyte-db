---
title: "::jsonb and ::json and ::text (JSON typecast)"
headerTitle: "::jsonb and ::json and ::text (typecast)"
linkTitle: "::jsonb, ::json, ::text (typecast)"
description: Typecast between any pair of text, json, and jsonb values.
menu:
  latest:
    identifier: typecast-operators
    parent: json-functions-operators
    weight: 10
isTocNested: true
showAsideToc: true
---

**Purpose:** Typecast between any pair of `text`, `json`, and `jsonb` values in both directions.

**Signature for the `jsonb` overload of `::text`:**

```
input value:       jsonb
return value:      text
```

**Notes:** You can use the `::text` operator on both a `jsonb` value and a `json` value;
you can use the `::jsonb` operator on both a `text` value and a `json` value; and
you can use the `::json` operator on both a `jsonb` value and a `text` value.

Consider this round trip:

```
new_rfc_7159_text := (orig_rfc_7159_text::jsonb)::text
```

The round trip is, in general, literally, but not semantically, lossy because _"orig_rfc_7159_text"_ can contain arbitrary occurrences of whitespace characters but _"new_rfc_7159_text"_ has conventionally defined whitespace use: there are no newlines; and single spaces are used according to a rule.

```plpgsql
do $body$
declare
  orig_rfc_7159_text constant text := '
    {
      "a": 1,
      "b": {"x": 1, "y": 19},
      "c": true
    }';

  expected_new_rfc_7159_text constant text :=
    '{"a": 1, "b": {"x": 1, "y": 19}, "c": true}';

  j constant jsonb := orig_rfc_7159_text::jsonb;
  new_rfc_7159_text constant text := j::text;
begin
  assert
    new_rfc_7159_text = expected_new_rfc_7159_text,
 'unexpected';
end;
$body$;
```

In contrast, this:

```
new_rfc_7159_text := (orig_rfc_7159_text::json)::text
```

is literally non-lossy because `json` stores the actual Unicode text, as is, that defines the JSON value.

```plpgsql
do $body$
declare
  orig_rfc_7159_text constant text := '
    {
      "a": 1,
      "b": {"x": 1, "y": 19},
      "c": true
    }';

  j constant json := orig_rfc_7159_text::json;
  new_rfc_7159_text constant text := j::text;

begin
  assert
    new_rfc_7159_text = orig_rfc_7159_text,
 'unexpected';
end;
$body$;
```

This example illustrates the point dramatically:

```plpgsql
do $body$
declare
  orig_rfc_7159_text constant text := '{"a": 42, "b": 17, "a": 99}';

  j_json  constant json  := orig_rfc_7159_text::json;
  j_jsonb constant jsonb := orig_rfc_7159_text::jsonb;

  text_from_json  constant text := j_json::text;
  text_from_jsonb constant text := j_jsonb::text;

  expected_text_from_jsonb constant text := '{"a": 99, "b": 17}';
begin
  assert
    (text_from_json = orig_rfc_7159_text) and
    (text_from_jsonb = expected_text_from_jsonb),
  'unexpected';
end;
$body$;
```

The `jsonb` representation is semantics-aware, and so it applies the "rightmost mention wins" rule to overwrite the first value establishment for the key _"a"_ (the JSON _number 42_) with the second value establishment for that key (the JSON _number 92_). But the `json` representation is _not_ semantics-aware; it merely applies a syntax-check before accepting the content.

This last example shows that the round trip:

```
new_json_value := (json_value::jsonb)::json;
```

is lossy because the `json` representation stores the text definition of the JSON value.

```plpgsql
do $body$
declare
  orig_json constant json := '
    {
      "a": 1,
      "b": {"x": 1, "y": 19},
      "c": true
    }'::json;

  jsonb_from_json constant jsonb := orig_json::jsonb;
  json_from_jsonb constant json := jsonb_from_json::json;
begin
  assert
    not (json_from_jsonb::text = orig_json::text),
  'unexpected';
end;
$body$;
```
The predicate for the assert has to use `::text` operator on both sides of the `=` operator because this has a `jsonb` overload but not a `json` overload.
