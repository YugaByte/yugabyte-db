---
title: JSON literals
headerTitle: JSON literals
linkTitle: JSON literals
description: JSON literals.
menu:
  latest:
    identifier: json-literals
    parent: api-ysql-datatypes-json
    weight: 10
isTocNested: true
showAsideToc: true
---
This section shows that the literal for both a `jsonb` value and a `json` value, as these are used both in SQL statements and in PL/pgSQL code, is the enquoted and appropriately typecasted RFC 7159-compliant `text` value that represents the JSON value,

The mutual relationship between a JSON value and its `::text` typecast is an instance of the general rule that governs the mutual relationship between a value of _any_ data type and its `::text` typecast. This general rule is explained in the section [The text typecast of a value, the literal for that value, and how they are related](../../type_array/literals/text-typecasting-and-literals/).

This `DO` block follows, for `jsonb`,  the same pattern that is used in that section for a variety of different data types. See also the section [::jsonb and ::json and ::text (typecast)](../functions-operators/typecast-operators/).

```plpgsql
create type t as (f1 int, f2 text, f3 boolean, f4 text[]);

do $body$
declare
  v1 constant int     := 42;
  v2 constant text    := 'a';
  v3 constant boolean := true;
  v4 constant text[]  := array['x', 'y', 'z'];

  v5 constant t := (v1, v2, v3, v4); 

  original   constant jsonb  not null := to_jsonb(v5);
  text_cast  constant text   not null := original::text;
  recreated  constant jsonb  not null := text_cast::jsonb;
begin
  assert
    (recreated = original),
  'assert failed';
  raise info 'jsonb: %', text_cast;
end;
$body$;
```
See the account of the [to_jsonb()](../functions-operators/to-jsonb) function. The `DO` block produces this output (after manually stripping the _"INFO:"_ prompt):
```
jsonb: {"f1": 42, "f2": "a", "f3": true, "f4": ["x", "y", "z"]}
```
It shows that the _"non-lossy round trip rule"_ holds here too:

- `jsonb` value to text typecast and back to `jsonb` value

And it shows that the `::text` typecast of a `jsonb` value that has been constructed bottom-up from SQL values conforms to RFC 7159.

The next block follows the same pattern for `json`. However, the `ASSERT` must be written differently because the [`=` operator](../functions-operators/equality-operator/) has no overload for `json`.

```plpgsql
do $body$
declare
  v1 constant int     := 42;
  v2 constant text    := 'a';
  v3 constant boolean := true;
  v4 constant text[]  := array['x', 'y', 'z'];

  v5 constant t := (v1, v2, v3, v4); 

  original   constant json  not null := to_json(v5)::jsonb;
  text_cast  constant text  not null := original::text;
  recreated  constant json  not null := text_cast::json;
begin
  assert
    (recreated::jsonb = original::jsonb),
  'assert failed';
  raise info 'json: %', text_cast;
end;
$body$;
```
The output is the same as the `DO` block for `jsonb` produces. It shows that the same _"non-lossy round trip"_ rule holds for a `json` value too.

Recall (see the [Synopsis](../../type_json/) section) that a `json` value is stored as a `text` value, annotated with the fact of what the data type is, and that whitespace in such a value is semantically insignificant. (The `jsonb` data type stores a parsed representation of the document hierarchy of subvalues in an appropriate internal format.) This has no effect on the reliability of the _"non-lossy round trip"_ rule. However, it means that a round trip `json` value to `jsonb` value to `json` value _might_ be lossy because, unless the `json` value happens to use whitespace just as the `::text` typecast of a `jsonb` value does, the recreated `json` value will have different whitespace from the original `json` value.
