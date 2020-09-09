---
title: jsonb_object()  and json_object() [JSON]
headerTitle: jsonb_object() and json_object()
linkTitle: jsonb_object() 
description: Create a JSON object from SQL arrays that specify keys with their values of SQL data type text.
menu:
  latest:
    identifier: jsonb-object
    parent: json-functions-operators
    weight: 150
isTocNested: true
showAsideToc: true
---

**Purpose:** Create a JSON _object_ from SQL _arrays_ that specify keys with their values of SQL data type `text`.

**Signature** For the `jsonb` variant:

```
input value:       [ text[] ]  |  [ text[][] ]  |  [ text[], text[] ]
return value:      jsonb
```

**Notes**: The `jsonb_object()` function achieves a similar effect to [`jsonb_build_object()`](../jsonb-build-object) but with significantly less verbose syntax.

Precisely because you present a single `text` actual, you can avoid the fuss of dynamic invocation and of dealing with interior single quotes that this brings in its train. However, it has the limitation that the primitive values in the resulting JSON value can only be _string_. It has three overloads.

The first overload has a single `text[]` formal whose actual text expresses the variadic intention conventionally: the alternating _comma_ separated items are the respectively the key and the value of a key-value pair.

```plpgsql
do $body$
declare
  array_values constant text[] :=
    array['a', '17', 'b', $$'Hello', you$$, 'c', 'true'];

  j constant jsonb :=  jsonb_object(array_values);
  expected_j constant jsonb := 
    $${"a": "17", "b": "'Hello', you", "c": "true"}$$;
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```

Compare this result with the result from supplying the same primitive SQL values to the [`jsonb_build_object()`](../jsonb-build-object) function. There, the data types of the SQL values are properly honored: The _numeric_ `17` and the _boolean_ `TRUE` are represented by the proper JSON primitive types. But with `jsonb_object()` it is not possible to express that `17` should be taken as a JSON _number_ value and `TRUE` should be taken as a JSON _boolean_ value.

The potential loss of data type fidelity brought by `jsonb_object()` is a high price to pay for the reduction in verbosity. On the other hand, `jsonb_object()` has the distinct advantage over `jsonb_build_object()` that you don't need to know statically how many key-value pairs the target JSON _object_ is to have.

If you think that it improves the clarity, you can use the second overload. This has a single `text[][]` formal—in other words an array of arrays.

```plpgsql
do $body$
declare
  array_values constant text[][] :=
    array[
      array['a',  '17'],
      array['b',  $$'Hello', you$$],
      array['c',  'true']
    ];

  j constant jsonb :=  jsonb_object(array_values);
  expected_j constant jsonb := 
    $${"a": "17", "b": "'Hello', you", "c": "true"}$$;
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```

This produces the identical result to that produced by the example for the first overload.

Again, if you think that it improves the clarity, you can use the third overload. This has a two `text[]` formals. The first expresses the list keys of the key-values pairs. And the second expresses the list values of the key-values pairs. The items must correspond pairwise, and clearly each array must have the same number of items. For example:

```plpgsql
do $body$
declare
  array_keys constant text[] :=
    array['a',  'b',              'c'   ];
  array_values constant text[] :=
    array['17', $$'Hello', you$$, 'true'];

  j constant jsonb :=  jsonb_object(array_keys, array_values);
  expected_j constant jsonb := 
    $${"a": "17", "b": "'Hello', you", "c": "true"}$$;
begin
  assert
    j = expected_j,
  'unexpected';
end;
$body$;
```

This, too, produces the identical result to that produced by the example for the first overload.
