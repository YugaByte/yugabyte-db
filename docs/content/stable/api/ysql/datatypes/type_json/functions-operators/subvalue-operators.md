---
title: "-> and ->> and #> and #>> (JSON subvalue operators)"
headerTitle: "-> and ->> and #> and #>> (JSON subvalue operators)"
linkTitle: "->, ->>, #>, #>> (JSON subvalues)"
description: Read a JSON value at a specified path.
block_indexing: true
menu:
  stable:
    identifier: subvalue-operators
    parent: json-functions-operators
    weight: 12
isTocNested: true
showAsideToc: true
---

**Purpose:** Read a JSON value at a specified path. The `>` variants return a `json` or `jsonb` value, according to the data type of the input. And the `>>` variants return a `text` value. The `#>` and `#>>` variants differ from `->` and `->>` variants in how the path is specified.

## The&#160; &#160;->&#160; &#160;operator

**Purpose:** Read the value specified by a one-step path returning it as a `json` or `jsonb` value.

**Signature** For the `jsonb` overload:

```
input values:       jsonb -> [int | text] [ -> [int | text] ]*
return value:       jsonb
```

**Notes:** The `->` operator requires that the JSON value is an _object_ or an _array_. _Key_ is a SQL value. When _key_ is a SQL `text` value, it reads the JSON value of the key-value pair with that key from an _object_. When _key_ is a SQL `integer` value, it reads the JSON value at that index key from an _array_. If the input JSON value is `json`, then the output JSON value is `json`, and correspondingly if the input JSON value is `jsonb`.

Reading a key value:

```plpgsql
do $body$
declare
  j  constant jsonb := '{"a": 1, "b": {"x": 1, "y": 19}, "c": true}';
  jsub constant jsonb := j  -> 'b';
  expected_jsub constant jsonb := '{"x": 1, "y": 19}';
begin
  assert
    jsub = expected_jsub,
  'unexpected';
end;
$body$;
```

Reading an _array_ value. (The first value in an _array_ has the index `0`.)

```plpgsql
do $body$
declare
  j  constant jsonb := '["a", "b", "c", "d"]';
  j_first constant jsonb := j -> 0;
  expected_first constant jsonb := '"a"';
begin
  assert
    j_first = expected_first,
  'unexpected';
end;
$body$;
```

## The&#160; &#160;#>&#160; &#160;operator

**Purpose:** Read the value specified by a multi-step path returning it as a `json` or `jsonb` value.

**Signature** for the `jsonb` overload:

```
input value:        jsonb #> text[]
return value:       jsonb
```

**Notes:** An arbitrarily deeply located JSON subvalue is identified by its path from the topmost JSON value. In general, a path is specified by a mixture of keys for _object_ subvalues and index values for _array_ subvalues.

Consider this JSON value:

```
[
  1,
  {
    "x": [
      1,
      true,
      {"a": "cat", "b": "dog"},
      3.14159
    ],
    "y": true
  },
  42
]
```

- At the topmost level of decomposition, it's an _array_ of three subvalues.

- At the second level of decomposition, the second _array_ subvalue (i.e. the value with the index of `1`) is an _object_ with two key-value pairs called _"x"_ and _"y"_.

- At the third level of decomposition, the subvalue for the key _"x"_ is an _array_ of subvalues.

- At the fourth level of decomposition, the third _array_ subvalue  (i.e. the value with the index of `2`) is an _object_ with two key-value pairs called _"a"_ and _"b"_.

- And at the fifth level of decomposition, the subvalue for key _"b"_ is the primitive _string_ value _"dog"_.

This, therefore, is the path to the primitive JSON _string_ value _"dog"_:

```
-> 1 -> 'x' -> 2 -> 'b'
```

(Recall that _array_ value indexing starts at _zero_.) 

The `#>` operator is a convenient syntax sugar shorthand for specifying a long path compactly, thus:

```
#> array['1', 'x', '2', 'b']::text[]
```

Notice that with the `->` operator, integers must be presented as such (so that `'1'` rather than `1` would silently read out `NULL`. However, with the `#>` operator, integers must be presented as convertible `text` values because all the values in a SQL array must have the same data type.

The PL/pgSQL `ASSERT` confirms that both the `->` path specification and the `#>` path specification produce the same result, thus:

```plpgsql
do $body$
declare
  j constant jsonb := '
    [
      1,
      {
        "x": [
          1,
          true,
          {"a": "cat", "b": "dog"},
          3.14159
        ],
        "y": true
      },
      42
    ]';

  one constant int := 1;
  two constant int := 2;
  x constant text := 'x';
  b constant text := 'b';
  one_t constant text := '1';
  two_t constant text := '2';

  jsub_1 constant jsonb := j -> one -> x -> two -> b;
  jsub_2 constant jsonb := j #> array[one_t, x, two_t, b];

  expected_jsub constant jsonb := '"dog"';
begin
  assert
    (jsub_1 = expected_jsub) and
    (jsub_2 = expected_jsub),
  'unexpected';
end;
$body$;
```

The paths are written using PL/pgSQL variables so that, as a pedagogic device, the data types are explicit.

## The&#160; &#160;->>&#160; &#160;and&#160; &#160;#>>&#160; &#160;operators

**Purpose:** Read the specified JSON value as a `text` value.

**Signatures** For the `jsonb` overloads:

```
input values:       jsonb ->> [int | text] [ -> [int | text] ]*
return value:       text
```
and:
```
input value:        jsonb #>> text[]
return value:       text
```

**Notes:** The `->` operator returns a JSON object. When the targeted value is compound, the `->>` operator returns the `::text` typecast of the value. But when the targeted value is primitive, the `->>` operator returns the value itself, typecast to a `text` value. In particular; a JSON _number_ value is returned as the `::text` typecast of that value (for example `'4.2'`), allowing it to be trivially `::numeric` typecasted back to what it actually is; a JSON _boolean_ value is returned as the `::text` typecast of that value (`'TRUE'` or `'FALSE'`), allowing it to be trivially `::boolean` typecasted back to what it actually is; a JSON _string_ value is return as is as a `text` value; and a JSON _null_ value is returned as a genuine SQL `NULL` so that the `IS NULL` test is `TRUE`.

The difference in semantics between the `->` operator and the `->>` operator is vividly illustrated (as promised above) by targeting this primitive JSON _string_ subvalue:

```
"\"First line\"\n\"second line\""
```
from the JSON value in which it is embedded. For example, here it is the value of the key _"a"_ in a JSON _object_:

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": "\"First line\"\n\"second line\""}'::jsonb;

  a_value_j constant jsonb := j ->  'a';
  a_value_t constant text  := j ->> 'a';

  expected_a_value_j constant jsonb :=
    '"\"First line\"\n\"second line\""'::jsonb;
  
  expected_a_value_t constant text := '"First line"
"second line"';

begin
  assert
    (a_value_j = expected_a_value_j) and
    (a_value_t = expected_a_value_t),
  'unexpected';
end;
$body$;
```

Understanding the difference between the `->` operator and the `->>` operator completely informs the understanding of the difference between the `#>` operator and the `#>>` operator.

The `>>` variant (both for `->` _vs_ `->>` and for `#>` _vs_ `#>>`) is interesting mainly when the denoted subvalue is a primitive value (as the example above showed for a primitive _string_ value). If you read such a subvalue, it's most likely that you'll want to cast it to a value of the appropriate SQL data type, `numeric`, `text`, or `boolean`.  You might use your understanding of a value's purpose (for example _"quantity ordered"_ or _"product SKU"_) to cast it to, say, an `int` value or a constrained text type like `varchar(30)`. In contrast, if you read a compound subvalue, it's most likely that you'll want it as a genuine `json` or `jsonb` value.

## Summary:&#160; &#160;->&#160; &#160;versus&#160; &#160;->>&#160; &#160;and&#160; &#160;#>&#160; &#160;versus&#160; &#160;#>>

- Each of the `->` and `#>` operators returns a genuine JSON value. When the input is `json`, the return value is `json`. And when the input is `jsonb`, the return value is `jsonb`.
- Each of the `->>` and `#>>` operators returns a genuine `text` value, both when the input is `json`, and when the input is `jsonb`.
- If the value that the path denotes is a compound JSON value, then the` >>` variant returns the `text` representation of the JSON value, as specified by RFC 7159. (The designers of the PostgreSQL functionality had no other feasible choice.) But if the JSON value that the path denotes is primitive, then the >> variant produces the text representation of the value itself. It turns out that the text representation of a primitive JSON value and the text representation of the value itself differ only for a JSON string—exemplified by _"a"_ versus _a_.

The following `ASSERT` tests all these rules:

```plpgsql
do $body$
declare
  ta constant text     := 'a';
  tn constant numeric  := -1.7;
  ti constant numeric  := 42;
  tb constant boolean  := true;
  t2 constant text  := '["a", -1.7, 42, true, null]';

  j1 constant jsonb := '"a"';
  j2 constant jsonb := t2::jsonb;
  j3 constant jsonb := '{"p": 1, "q": ["a", -1.7, 42, true, null]}';
begin
  assert
    (j2 ->  0)               = j1 and
    (j2 ->> 0)               = ta and
    (j2 ->> 1)::numeric      = tn and
    (j2 ->> 2)::int          = ti and
    (j2 ->> 3)::boolean      = tb and
    (j2 ->> 4)            is null and

    (j3 #>  array['q', '0']) = j1 and
    (j3 #>> array['q', '0']) = ta and

    (j3 ->  'q')             = j2 and
    (j3 ->> 'q')             = t2
    ,
    'unexpected';
end;
$body$;
```
