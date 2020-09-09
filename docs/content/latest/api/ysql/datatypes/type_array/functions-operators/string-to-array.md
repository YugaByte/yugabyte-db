---
title: string_to_array()
linkTitle: string_to_array()
headerTitle: string_to_array()
description: string_to_array()
menu:
  latest:
    identifier: string-to-array
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---
**Purpose:** Return a one-dimensional `text[]` array by splitting the input `text` value into subvalues using the specified `text` value as the _"delimiter"_. Optionally, allows a specified `text` value to be interpreted as `NULL`.

**Signature:** 

```
input value:       text, text [, text]
return value:      text[]
```
**Example:**

```plpgsql
select string_to_array(
  'a|b|?|c', -- the to-be-split string
  '|',       -- the character(s) to be taken as the delimiter
  '?'        -- the character(s) to be taken to the null indicator
) as "string_to_array result";
```

It produces thus result:

```
 string_to_array result 
------------------------
 {a,b,NULL,c}
```

**Semantics:**

The interpretation of the _"delimiter"_ `text` value and the _"null indicator"_ `text` uses this priority rule:

- _First_, the _"delimiter"_ `text` value is consumed

- _and only then_ is the _"null indicator"_ `text` value consumed.

However, this rule matters only when these two critical values are defined by more than one character and when one starts with a sequence that the other ends with.

Yugabyte recommends, therefore, that when you can choose the `text` values for the _"delimiter"_ and for the _"null indicator"_, you choose two different single characters. This is what the example, above, does. Of course, you must be sure that neither occurs in any of the `text` values that you want to convert into `text[]` arrays. (There is no escaping mechanism.)

Predicting the outcome, when unfortunate choices for these two values were made, will require some mental effort. The `DO` block below demonstrates an example of this:

- The _"delimiter"_ is `' !'::text`.

- The _"null indicator"_ is `'~ '::text`.

And the input contains this sequence:

&#160;&#160;&#160; &#60;tilda&#62;&#60;space&#62;&#60;exclamationPoint&#62;

The troublesome sequence is shown in typewriter font here:

&#160;&#160;&#160; dog house !~  !x! `~ !` cat flap !  !

These considerations, together with the fact that it can produce only a `text[]` output, mean that the `string_to_array()` function has limited usefulness.

```plpgsql
do $body$
declare
  delim_text  constant text := ' !';
  null_text   constant text := '~ ';

  input_text  constant text := 'dog house !~  !x! ~ ! cat flap !  !';

  result constant text[] :=
    string_to_array(input_text, delim_text, null_text);

  good_delim_text constant text := '|';
  good_null_text  constant text := '?';

  delim_first_text constant text :=
    replace(replace(
      input_text,
      delim_text, good_delim_text),
      null_text,  good_null_text);

  null_first_text constant text := 
    replace(replace(
      input_text,
      null_text,  good_null_text),
      delim_text, good_delim_text);

  delim_first_result constant text[] :=
    string_to_array(delim_first_text, good_delim_text, good_null_text);

  null_first_result constant text[] :=
    string_to_array(null_first_text, good_delim_text, good_null_text);

  -- Notice that one of the special characters, "!", remains in
  -- both expected_result and unexpected_result.
  -- If 
  expected_result constant text[] :=
    '{"dog house",NULL,"x! ~"," cat flap"," ",""}';
  unexpected_result constant text[] :=
    '{"dog house",NULL,"x! ?! cat flap"," ",""}';

begin
  assert
  (result             =  expected_result)    and
  (delim_first_result =  expected_result)    and
  (null_first_result  <> delim_first_result) and
  (null_first_result  =  unexpected_result)  and
    true,
  'unexpected';
end;
$body$;
```
