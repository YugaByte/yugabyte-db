---
title: text typecast of a value, literal for that value, and how they are related
linkTitle: Text typecasting and literals
headerTitle: The text typecast of a value, the literal for that value, and how they are related
description: The text typecast of a value, the literal for that value, and how they are related
block_indexing: true
menu:
  v2.1:
    identifier: text-typecasting-and-literals
    parent: array-literals
    weight: 5
isTocNested: true
showAsideToc: true
---

This section establishes some basic notions that have a much broader scope of applicability than just arrays. But, because using array literals rests on these notions, they are summarized here.

## The non-lossy round trip: value to text typecast and back to value

Consider this pattern:
```
do $body$
declare
  original   constant <some data type>  not null := <some value>;
  text_cast  constant text              not null := original::text;
  recreated  constant <some data type>  not null := text_cast::<some data type>;
begin
  assert
    (recreated = original),
  'assert failed';
end;
$body$;
```

It demonstrates a universal rule that YSQL inherits from PostgreSQL:

- Any value of any data type, primitive or composite, can be `::text` typecasted. Similarly, there always exists a `text` value that, when properly spelled, can be typecasted to a value of any desired data type, primitive or composite.
- If you `::text` typecast a value of any data type and then typecast that `text` value to the original value's data type, then the value that you get is identical to the original value.

The following `DO` block applies the pattern using a representative range of both primitive and composite data types. (The data type `text`, as the degenerate case, is not included.) It also displays the value of the `::text` typecast for each data type.

Notice that the last test uses an array whose data type is the user-created `DOMAIN` _"int_arr_t"_. The section [Using an array of `DOMAIN` values](../../array-of-domains/) explains this notion. This is a real stress-tests of the rule.

```postgresql
-- Needed by the '1-d array of "row" type values' test.
create type rt as (n numeric, s text, t timestamp, b boolean);

-- Needed by the 'Ragged array' test.
create domain int_arr_t as int[];

do $body$
begin
  -- numeric
  declare
    original   constant numeric  not null := 42.1763;
    text_cast  constant text     not null := original::text;
    recreated  constant numeric  not null := text_cast::numeric;
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info 'numeric:              %', text_cast;
  end;

  -- timestamp
  declare
    original   constant timestamp  not null := now()::timestamp;
    text_cast  constant text       not null := original::text;
    recreated  constant timestamp  not null := text_cast::timestamp;
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info 'timestamp:            %', text_cast;
  end;

  -- timestamp with timezone
  declare
    original   constant timestamptz  not null := now()::timestamptz;
    text_cast  constant text         not null := original::text;
    recreated  constant timestamptz  not null := text_cast::timestamp;
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info 'timestamptz:          %', text_cast;
  end;

  -- boolean
  declare
    original   constant boolean  not null := true;
    text_cast  constant text     not null := original::text;
    recreated  constant boolean  not null := text_cast::boolean;
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info 'boolean:              %', text_cast;
  end;

  -- "row" type
  declare
    original   constant rt    not null := row(42.1763, 'dog house', now(), true);
    text_cast  constant text  not null := original::text;
    recreated  constant rt    not null := text_cast::rt;
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info '"row" type:           %', text_cast;
  end;

  -- 2-d array
  declare
    original   constant int[]  not null := array[array[1, 2], array[3, 4]];
    text_cast  constant text   not null := original::text;
    recreated  constant int[]  not null := text_cast::int[];
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info '2-d array             %', text_cast;
  end;

  -- 1-d array of "row" type values
  declare
    original   constant rt[]  not null :=
      array[
        row(42.1763, 'dog house', now(),                    true),
        row(19.8651, 'cat flap',  now() + interval '1' day, false)
      ];
    text_cast  constant text  not null := original::text;
    recreated  constant rt[]  not null := text_cast::rt[];
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info 'array of "row" type:  %', text_cast;
  end;

  -- Ragged array: 1-d array of 1-da arrays of different lengths.
  declare
    arr_1      constant int_arr_t    not null := array[1, 2];
    arr_2      constant int_arr_t    not null := array[3, 4, 5];
    original   constant int_arr_t[]  not null := array[arr_1, arr_2];
    text_cast  constant text         not null := original::text;
    recreated  constant int_arr_t[]  not null := text_cast::int_arr_t[];
  begin
    assert
      (recreated = original),
    'assert failed';
    raise info 'array of arrays:      %', text_cast;
  end;
end;
$body$;
```
It produces this result (after manually removing the _"INFO:"_ prompt on each output line.
```
numeric:              42.1763
timestamp:            2020-05-03 22:25:42.932771
timestamptz:          2020-05-03 22:25:42.932771-07
boolean:              true
"row" type:           (42.1763,"dog house","2020-05-03 22:25:42.932771",t)
2-d array             {{1,2},{3,4}}
array of "row" type:  {"(42.1763,\"dog house\",\"2020-05-03 22:25:42.932771\",t)","(19.8651,\"cat flap\",\"2020-05-04 22:25:42.932771\",f)"}
array of arrays:      {"{1,2}","{3,4,5}"}
```
The section [Multidimensional array of `int` values](../array-of-primitive-values/#multidimensional-array-of-int-values) explains the syntax of the _2-d array_ `text` value.

The section [The literal for a _"row"_ type value](../row/) explains the syntax of the _"row" type_ `text` value.

And the section [The literal for an array of "row" type values](../array-of-rows/) explains the syntax of the value: array of _"row" type_ `text` value.

Notice how the syntax for the _array of arrays_ `text` value compares with the syntax for the _2-d array_ `text` value. Because the _array of arrays_ is ragged, the two inner `{}` pairs contain respectively two and three values. To distinguish between this case and the ordinary rectilinear case, the inner `{}` pairs are surrounded by double quotes.

## boolean values show special text forms in ysqlsh

Try this:
```postgresql
select true as "bare true", true::text as "true::text";
```
This is the result:
```
 bare true | true::text 
-----------+------------
 t         | true
```
For all but `boolean` values, the string of characters that `ysqlsh` uses to display any value is the `::text` typecast of that value. (After all, the only feasible means of display is strings of characters.) But uniquely for the two `boolean` values denoted by the keywords `TRUE` and `FALSE` it uses the single characters `t` and `f` rather than their `::text` typecasts—unless you explicitly write the typecast. 

This behavior is inherited from `psql`.

You saw above that even when you explicitly `::text` typecast a composite value, `TRUE` and `FALSE` are represented as `t` and `f`. You can't influence this outcome because it has to do with the rules for deriving the `text` of the typecast and _not_ with the convention that `ysqlsh` uses. This asymmetry was established many years ago, and it will not change.

## The relationship between the text typecast of a value and the literal that creates that value

Try this in `ysqlsh`:
```postgresql
select
  42.932771::numeric          as n,
  'cat'::text                 as t1,
  $$dog's breakfast$$::text   as t2,
  array[1, 2, 3]::int[]       as "int array";
```
It shows the result:
```
     n     | t1  |       t2        | int array 
-----------+-----+-----------------+-----------
 42.932771 | cat | dog's breakfast | {1,2,3}}
```
 You won't be surprised by this. But you need to establish the proper terms of art that allow you to describe what's going on precisely and correctly. The remaining sections in [Creating an array value using a literal](../../literals/) rely on this.

Consider this first:

```
42.932771::numeric
```

This is the literal that the SQL language (at least in the YSQL and PostgreSQL dialects) uses to establish the corresponding strongly-typed `numeric` value. (PL/pgSQL uses the same form for the same purpose.) But, to state the obvious, a SQL statement and a PL/pgSQL source are nothing but strings of characters. That means that, in the present context, this:

```
42.932771
```

is the _text_ of the literal.

Now consider these two:

```
'cat'::text          $$dog's breakfast$$::text
```

The parsing rules of both SQL and PL/pgSQL (or more properly stated, the definitions of the grammars of these two languages) require that `text` literals are enquoted. Moreover, there are two syntactic mechanisms for doing this: the ordinary single quote; and so-called dollar quotes, where `$$` is a special case of the more general `$anything_you_want$`. You might think that the `::text` typecast is redundant here. But don't forget that the text of these literals might be used to establish `varchar` or `char` values.

You see already, then, that the rules for composing a `numeric` literal and a `text` literal are different:

- You compose a `numeric` literal by following the bare text that specifies the intended value with the `::numeric` typecast operator.

- You compose a `text` literal by enquoting the bare text that specifies the intended value (however you choose to do the quoting) and by then following this with the `::text` typecast operator.

(If you did enquote the bare text in a `numeric` literal, then you would _not_ see an error. Rather, you would get implicit but undesirable behavior: first, a genuine `text` value would be generated internally, and then, this, in turn, would be typecasted to the `numeric` value.)

You've already seen, informally, some examples of array literals. Here is the rule:

- You compose the bare text that specifies the intended value by writing an utterance in a dedicated grammar that starts with the left curly brace and ends with the right curly brace. (This grammar is the focus of the remainder of the [Creating an array value using a literal](../../literals/) section.) Then you enquote this bare text (however you choose to do the quoting) and then typecast it to the desired target array data type.

These are three special cases of a more general rule. In some cases (for example in the literal for a _"row"_ type value) the enquoting mechanism might be optional (depending on the intended value) and, when written uses _double quote_ as the enquoting character. But here, too, the general rule is the same. The bare text that specifies the intended value can always be correctly written as the `::text` typecast of that value.

## Stating the general rule

Here is the general rule.

- The literal for a value of any data type is the possibly enquoted bare text that specifies the intended value, followed by the typecast operator to the desired target data type.
- This rule is applied recursively, for the literal for a composite value, but with different actual rules at different levels of nesting. For example, the literal for an array value as a whole must be typecasted. But, because the data type of every value in the array is already determined, the bare text that specifies these values is _not_ typecasted.
- The `::text` typecast of any value can always be used as the bare text of the literal that will recreate that value.

You can see examples of the text of the literal that creates an array value by creating the value using the constructor and then inspecting its `::text` typecast. But the safe way to create the text of a literal for an intended value is to understand the syntax and semantics that govern its composition.

When this difference is important, the _"Array data types and functionality"_ major section distinguishes between:

- (1) the _literal_ for the intended value (that is, the whole thing with the enquoting and the typecast, when one or both of these are needed);
- and (2) the _text of the literal_ for the intended value (that is, the bare text that specifies this value).

**Note:** Often, the data type of the assignment target (for example, a field in a column in a schema-level table or a variable in a PL/pgSQL program) is sufficient implicitly to specify the intended typecast without writing the operator. But it's never harmful to write it. Moreover, in some cases, omitting the explicit typecast can bring performance costs. For this reason, Yugabyte recommends that you always write the explicit typecast unless you are certain beyond all doubt that omitting it brings no penalty.

## Defining the "canonical form of a literal"

The term _"canonical form"_ applies specifically to the _text of a literal_ rather than to the _literal as a whole_. But when the text of a literal is in canonical form, the literal as a whole, too, is in canonical form.

The canonical form of the text of a literal that produces a specific value, of any data type, is the `::text` typecast of that value.

Many of the examples in this _"Array data types and functionality"_ major section show that many spellings of the text of an array literal, in addition to the canonical form, will produce a particular intended target value. The differences are due to how whitespace, punctuation, and escape characters are used.
