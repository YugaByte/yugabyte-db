---
title: jsonb_array_elements_text() and json_array_elements_text()
headerTitle: jsonb_array_elements_text() and json_array_elements_text()
linkTitle: jsonb_array_elements_text()
description: Transform JSON values of an JSON array into a SQL table of text values using jsonb_array_elements_text() and json_array_elements_text().
block_indexing: true
menu:
  stable:
    identifier: jsonb-array-elements-text
    parent: json-functions-operators
    weight: 70
isTocNested: true
showAsideToc: true
---

**Purpose:** Transform the JSON values of JSON _array_ into a SQL table of (i.e., `SETOF`) `text` values.

**Signature:** For the `jsonb` variant:

```
input value:       jsonb
return value:      SETOF text
```

**Notes:** The function `jsonb_array_elements_text()` bears the same relationship to [`jsonb_array_elements()`](../jsonb-array-elements) that the other `*text()` functions bear to their plain counterparts: it's the same relationship that the [`->>` and `#>>`operators](../subvalue-operators) bear, respectively to [`->` and `#>` operators](../subvalue-operators). (Compound values become the RFC 7159 text of the value; primitive values become the `::text` representation of the SQL value that the JSON primitive value corresponds to.)

This example uses the same JSON _array_ input that was used to illustrate `jsonb_array_elements()`.

Notice that the JSON value _null_ becomes a genuine SQL `NULL`. However, SQL array comparison uses `IS NOT DISTINCT FROM` semantics, and not the semantics that the comparison of scalars uses. So the simple `ASSERT` that `elements = expected_elements` is `TRUE` is sufficient. See the section [Operators for comparing two arrays](../../..//type_array/functions-operators/comparison/).

```plpgsql
do $body$
declare
  j_array constant jsonb := '["cat", "dog house", 42, true, {"x": 17}, null]';
  t text;

  elements text[];
  expected_elements constant text[] :=
    array[
      'cat',
      'dog house',
      '42',
      'true',
      '{"x": 17}',
      null
    ];

  n int := 0;
begin
  for t in (select jsonb_array_elements_text(j_array)) loop
    n := n + 1;
    elements[n] := t;
  end loop;

  assert
    elements = expected_elements,
  'unexpected';
end;
$body$;
```

This highlights the fact that the resulting values are the `::text` typecasts of the equivalent SQL primitive values, rather than the RFC 7159 representations of the actual JSON values. In particular, `42` is the two characters `4` and `2` and `true` is the four characters `t` , `r`, `u`, and `e`.

This example emphasizes the impedance mismatch between a JSON _array_ and a SQL array: the former allows values of heterogeneous data types; but the latter allows only values of the same data type—as was used to declare the array.

If you have prior knowledge of the convention to which the input JSON document adheres, you can cast the output of `jsonb_array_elements_text()` to, say, `integer` or `boolean`. For example, this:

```plpgsql
create domain length as numeric
  not null
  check (value > 0);

select value::length
from jsonb_array_elements_text(
  '[17, 19, 42, 47, 53]'::jsonb
  );
```

generates a table of genuine `integer` values that honor the constraints that the `domain` defines. If you make one of the input elements negative, then you get this error:

```
value for domain length violates check constraint "length_check"
```

And if you set one of the input elements to the JSON _null_, like this:
```plpgsql
select value::length
from jsonb_array_elements_text(
  '[17, null, 42, 47, 53]'::jsonb
  );
```
then you get this error:

```
domain length does not allow NULL values
```

Here's the same idea for `boolean` values:

```plpgsql
create domain truth as boolean
  not null;

select (value::truth)::text
from jsonb_array_elements_text(
  '[true, false, true, false, false]'::jsonb
  );
```

It produces this output in `ysqlsh`:

```
 value 
-------
 true
 false
 true
 false
 false
```
