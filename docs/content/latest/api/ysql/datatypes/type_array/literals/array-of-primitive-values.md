---
title: The literal for an array of primitive values
linkTitle: Array of primitive values
headerTitle: The literal for an array of primitive values
description: The literal for an array of primitive values
menu:
  latest:
    identifier: array-of-primitive-values
    parent: array-literals
    weight: 10
isTocNested: true
showAsideToc: true
---

This section states a sufficient subset of the rules that allow you to write a syntactically correct array literal that expresses any set of values, for arrays of any scalar data type, that you could want to create. The full set of rules allows more flexibility than do just those that are stated here. But because these are sufficient, the full, and rather complex, set is not documented here. The explanations in this section will certainly allow you to interpret the `::text` typecast of any array value that you might see, for example in `ysqlsh`.

Then Yugabyte's recommendation in this space is stated. And then the rules are illustrate with examples.

## Statement of the rules

The statement of these rules depends on understanding the notion of the canonical form of a literal. [Defining the "canonical form of a literal](../text-typecasting-and-literals/#defining-the-canonical-form-of-a-literal) explained that the `::text` typecast of any kind of array shows you that this form of the literal (more carefully stated, the _text_ of this literal) can be used to recreate the value.

In fact, this definition, and the property that the canonical form of the literal is sufficient to recreate the value, hold for values of _all_ data types.

Recall that every value within an array necessarily has the same data type. If you follow the rules that are stated here, and illustrated in the demonstrations below, you will always produce a syntactically valid literal which expresses the semantics that you intend. It turns out that many other variants, especially for `text[]` arrays, are legal and can produce the values that you intend. However, the rules that govern these exotic uses will not documented because it is always sufficient to create your literals in canonical form.

Here is the sufficient set of rules.

- The commas that delimit successive values, the curly braces that enclose the entire literal, and the inner curly braces that are used in the literals for multidimensional arrays, can be surrounded by arbitrary amounts of whitespace. If you want strictly to adhere to canonical form, then you ought not to do this. But doing so can improve the readability of _ad hoc_ manually typed literals. It can also make it easier to read trace output in a program that constructs array literals programmatically.
- In numeric and `boolean` array literals, do _not_ surround the individual values with double quotes.
- In the literal for a `timestamp[]` array, _do_ surround the individual values with double quotes—even though this is not strictly necessary.
- In the literal for a `text[]` array, _do_ surround every individual value with double quotes, even though this is not always necessary. It _is_ necessary for any value that itself contains, as ordinary text, any whitespace or any of the characters that have syntactic significance within the outermost curly brace pair. This is the list:
```
   <space>   {   }   ,   "   \
```
- It's sufficient to write the curly braces and the comma ordinarily within the enclosing double quotes. But each of the double quote character and the backslash character must be escaped with an immediately preceding single backslash.


## Always write array literals in canonical form

Bear in mind that you will rarely manually type literals in the way that this section does to demonstrate the rules. You'll do this only when teaching yourself, when prototyping new code, or when debugging. Rather, you'll typically create the literals programmatically—often in a client-side program that parses out the data values from, for example, an XML text file or, these days, probably a JSON text file. In these scenarios, the target array is likely to have the data type _"some_user_defined_row_type[]"_. And when you create literals programmatically, you want to use the simplest rules that work and you have no need at all to omit arguably unnecessary double quote characters.

**Yugabyte recommends that the array literals that you generate programmatically are always spelled using the canonical representations**

You can relax this recommendation, to make tracing or debugging your code easier (as mentioned above), by using a newline between each successive encoded value in the array—at least when the values themselves use a lot of characters, as they might for _"row"_ type values.

**Note:** You can hope that the client side programming language that you use, together with the driver that you use to issue SQL to YugabyteDB and to retrieve results, will allow the direct use of data types that your language defines that map directly to the YSQL array and _"row"_ type, just as they have scalar data types that map to `int`, `text`, `timestamp`, and `boolean`. For example Python has _"list"_ that maps to array and _"tuple"_ that maps to _"row"_ type. And the _"psycopg2"_ driver that you use for YugabyteDB can map values of these data types to, for example, a `PREPARE` statement like the one shown below.

**Note**: YSQL has support for converting a JSON array (and this includes a JSON array of JSON objects) directly into the corresponding YSQL array values.

The rules for constructing literals for arrays of _"row"_ type values are described in [literal for an array of "row" type values](../array-of-rows/) section.

Your program will parse the input and create the required literals as ordinary text strings that you'll then provide as the actual argument to a `PREPARE` statement execution, leaving the typecast of the `text` actual argument, to the appropriate array data type, to the prepared `INSERT` or `UPDATE` statement like this:
```
prepare stmt(text) as insert into t(rs) values($1::rt[]);
```
Assume that in, this example, _"rt"_ is some particular user-define _"row"_ type.

## Examples to illustrate the rules

Here are some examples of kinds array of primitive values:

- array of numeric values (like `int` and `numeric`)
- array of stringy values (like `text`, `varchar`, and `char`)
- array of date-time values (like `timestamp`)
- array of `boolean` values.

In order to illustrate the rules that govern the construction of an array literal, it is sufficient to consider only these.

You'll use the `array[]` constructor to create representative values of each kind and inspect its `::text` typecast.

### One-dimensional array of int values

This example demonstrates the principle:

```plpgsql
create table t(k serial primary key, v1 int[], v2 int[]);
insert into t(v1) values (array[1, 2, 3]);
select v1::text as text_typecast from t where k = 1
\gset result_
\echo :result_text_typecast
```
The `\gset` metacommand was used first in this _"Array data types and functionality"_ major section in [`array_agg()` and `unnest()`](../../functions-operators/array-agg-unnest). 

Notice that, in this example, the `SELECT` statement is terminated by the `\gset` metacommand on the next line rather than by the usual semicolon. The `\gset` metacommand is silent. The `\echo` metacommand shows this:

```
{1,2,3}
```
You can see the general form already:

- The (_text_ of) an array literal starts with the left curly brace and ends with the right curly brace.

- The items within the braces are delimited by commas, and there is no space between one item, the comma, and the next item. Nor is there any space between the left curly brace and the first item or between the last item and the right curly brace.

[One-dimensional array of `text` values](./#one-dimensional-array-of-text-values) shows that more needs to be said. But the two rules that you've already noticed always hold.

To use the literal that you produced to create a value, you must enquote it and typecast it. Do this with the `\set` metacommand:

```plpgsql
\set canonical_literal '\'':result_text_typecast'\'::int[]'
\echo :canonical_literal
```
. The `\echo` metacommand now shows this:
```
'{1,2,3}'::int[]
```
Next, use the canonical literal that was have produced to update _"t.v2"_ to confirm that the value that the row constructor created was recreated:
```plpgsql
update t set v2 = :canonical_literal where k = 1;
select (v1 = v2)::text as "v1 = v2" from t where k = 1;
```
It shows this:
```
 v1 = v2 
---------
 true
```
As promised, the canonical form of the array literal does indeed recreate the identical value that the `array[]` constructor created.

**Note:**

Try this:
```plpgsql
select 12512454.872::text;
```
The result is the canonical form, `12512454.872`. So this (though you rarely see it):
```plpgsql
select 12512454.872::numeric;
```
runs without error. Now try this:

```plpgsql
select to_number('12,512,454.872', '999G999G999D999999')::text;
```
This, too, runs without error because it uses the `to_number()` built-in function. The result here, too, is the canonical form, `12512454.872`—with no commas. Now try this:

```plpgsql
select '12,512,454.872'::numeric;
```
This causes the _"22P02: invalid input syntax for type numeric"_ error. In other words, _only_ a `numeric` value in canonical form can be directly typecast using `::numeric`.

Here, using an array literal, is an informal first look at what follows. For now, take its syntax to mean what you'd intuitively expect. You must spell the representations for the values in a `numeric[]` array in canonical form. Try this:

```plpgsql
select ('{123.456, -456.789}'::numeric[])::text;
```
It shows this:

```
 {123.456,-456.789}
```

Now try this:
```plpgsql
select ('{9,123.456, -8,456.789}'::numeric[])::text;
```
It silently produces this presumably unintended result (an array of _four_ numeric values) because the commas are taken as delimiters and not as part of the representation of a single `numeric` value:
```
 {9,123.456,-8,456.789}
```
In an array literal (or in a _"row"_ type value literal), there is no way to accommodate forms that cannot be directly typecast. (The same holds for `timestamp` values as for `numeric` values.) YSQL inherits this limitation from PostgreSQL. It is the user's responsibility to work around this when preparing the literal because, of course, functions like _"to_number()"_ cannot be used within literals. Functions can, however, be used in a value constructor as [`array[]` value constructor](../../array-constructor/) shows.

### One-dimensional array of text values

Use [One-dimensional array of `int` values](./#one-dimensional-array-of-int-values) as a template for this and the subsequent sections. The example sets array values each of which, apart from the single character `a`, needs some discussion. These are the characters (or, in one case, character sequence), listed here "bare" and with ten spaces between each:

```
     a          a b          ()          ,          '          "          \
```

```plpgsql
create table t(k serial primary key, v1 text[], v2 text[]);
insert into t(v1) values (array['a', 'a b', '()', ',', '{}', $$'$$, '"', '\']);
select v1::text as text_typecast from t where k = 1
\gset result_
\echo :result_text_typecast
```
For ordinary reasons, something special is needed to establish the single quote within the surrounding array literal which itself must be enquoted for using in SQL. Dollar quotes are a convenient choice. The `\echo` metacommand shows this:

```
{a,"a b",(),",","{}",',"\"","\\"}
```
This is rather hard (for the human) to parse. To make the rules easier to see, this list doesn't show the left and right curly braces. And the syntactically significant commas are surrounded with four spaces on each side:
```
     a    ,    "a b"    ,    ()    ,    ","    ,    "{}"    '    ,    "\""    ,    "\\"
```
In addition to the first two rules, notice the following.

- Double quotes are used to surround a value that includes any spaces. (Though the example doesn't show it, this applies to leading and trailing spaces too.)
- The left and right parentheses are _not_ surrounded with double quotes. Though these have syntactic significance in other parsing contexts, they are insignificant within the curly braces of an array literal.
- The comma _has_ been surrounded by double quotes. This is because it _does_ have syntactic significance, as the value delimiter, within the curly braces of an array literal.
- The curly braces _have_ been surrounded by double quotes. This is because interior curly braces _do_ have syntactic significance, as you'll see below, in the array literal for a multidimensional array.
- The single quote is _not_ surrounded with double quotes. Though it has syntactic significance in other parsing contexts, it is insignificant within the curly braces of an array literal. This holds, also, for all sorts of other punctuation characters like `;` and `:` and `[` and `]` and so on.
- The double quote has been escaped with a single backslash and this has been then surrounded with double quotes. This is because it _does_ have syntactic significance, as the (one and only) quoting mechanism, within the curly braces of an array literal.
- The backslash has also been escaped with another single backslash and this has been then surrounded with double quotes. This is because it _does_ have syntactic significance, as the escape character, within the curly braces of an array literal.

There's another rule that the present example does not show. Though not every comma-separated value was surrounded by double quotes, it's _never harmful_ to do this. You can confirm this with your own test, Yugabyte recommends that, for consistency, you always surround every `text` value within the curly braces for a `text[]` array literal with double quotes.

To use the text of the literal that was produced above to recreate the value, you must enquote it and typecast it. Do this, as you did for the `int[]` example above, with the `\set` metacommand. But you must use dollar quotes because the literal itself has an interior single quote.

```plpgsql
\set canonical_literal '$$':result_text_typecast'$$'::text[]
\echo :canonical_literal
```
The `\echo` metacommand now shows this:
```
$${a,"a b",(),",",',"\"","\\"}$$::text[]
```
Next, use the canonical literal to update _"t.v2"_ to confirm that the value that the row constructor created was recreated:
```plpgsql
update t set v2 = :canonical_literal where k = 1;
select (v1 = v2)::text as "v1 = v2" from t where k = 1;
```
Again, it shows this:
```
 v1 = v2 
---------
 true
```
So, again as promised, the canonical form of the array literal does indeed recreate the identical value that the `array[]` constructor created.

### One-dimensional array of timestamp values

This example demonstrates the principle:

```plpgsql
create table t(k serial primary key, v1 timestamp[], v2 timestamp[]);
insert into t(v1) values (array[
    '2019-01-27 11:48:33'::timestamp,
    '2020-03-30 14:19:21'::timestamp
  ]);
select v1::text as text_typecast from t where k = 1
\gset result_
\echo :result_text_typecast
```
The `\echo` metacommand shows this:

```
{"2019-01-27 11:48:33","2020-03-30 14:19:21"}
```
You learn one further rule from this:

- The `::timestamp` typecastable strings within the curly braces are tightly surrounded with double quotes.

To use the text of the literal that was produced to create a value, you must enquote it and typecast it. Do this with the `\set` metacommand:

```plpgsql
\set canonical_literal '\'':result_text_typecast'\'::timestamp[]'
\echo :canonical_literal
```
. The `\echo` metacommand now shows this:
```
'{"2019-01-27 11:48:33","2020-03-30 14:19:21"}'::timestamp[]
```
Next, use the canonical literal to update _"t.v2"_ to confirm that the value that the row constructor created was recreated:
```plpgsql
update t set v2 = :canonical_literal where k = 1;
select (v1 = v2)::text as "v1 = v2" from t where k = 1;
```
It shows this:
```
 v1 = v2 
---------
 true
```
Once again, as promised, the canonical form of the array literal does indeed recreate the identical value that the `array[]` constructor created.

### One-dimensional array of boolean values (and NULL in general)

This example demonstrates the principle:

```plpgsql
create table t(k serial primary key, v1 boolean[], v2 boolean[]);
insert into t(v1) values (array[
    true,
    false,
    null
  ]);
select v1::text as text_typecast from t where k = 1
\gset result_
\echo :result_text_typecast
```
The `\echo` metacommand shows this:

```
{t,f,NULL}
```
You learn two further rules from this:

- The canonical representations of `TRUE` and `FALSE` within the curly braces for a `boolean[]` array are `t` and `f`. They are not surrounded by double quotes.
- To specify `NULL`, the canonical form uses upper case `NULL` and does not surround this with double quotes.

Though the example doesn't show this, `NULL` is not case-sensitive. But to compose a literal that adheres to canonical form, you ought to spell it using upper case. And this is how you specify `NULL` within the array literal for _any_ data type. (A different rule applies for fields within the literal for _"row"_ type value).

**Note:** If you surrounded `NULL` within a literal for a `text[]` array, then it would be silently interpreted as an ordinary `text` value that just happens to be spelled that way.

To use the literal that that was produced to create a value, you must enquote it and typecast it. Do this with the `\set` metacommand:

```plpgsql
\set canonical_literal '\'':result_text_typecast'\'::boolean[]'
\echo :canonical_literal
```
. The `\echo` metacommand now shows this:
```
'{t,f,NULL}'::boolean[]
```
Next use the canonical literal to update _"t.v2"_ to can confirm that the value that the row constructor created has been recreated :
```plpgsql
update t set v2 = :canonical_literal where k = 1;
select (v1 = v2)::text as "v1 = v2" from t where k = 1;
```
It shows this:
```
 v1 = v2 
---------
 true
```
Yet again, as promised, the canonical form of the array literal does indeed recreate the identical value that the `array[]` constructor created.

### Multidimensional array of int values

```plpgsql
create table t(k serial primary key, v int[]);

-- Insert a 1-dimensional int[] value.
insert into t(v) values('
    {1,  2}
  '::int[]);

-- Insert a 2-dimensional int[] value.
insert into t(v) values('
    {
      {1,  2},
      {3,  4}
    }
  '::int[]);

-- Insert a 3-dimensional int[] value.
insert into t(v) values('
    {
      {
        {1,  2}, {3,  4}
      },
      {
        {5,  6}, {7,  8}
      }
    }
  '::int[]);

-- Insert a 3-dimensional int[] value, specifying
-- the lower and upper bounds along each dimension.
insert into t(v) values('
    [3:4][5:6][7:8]=
    {
      {
        {1,  2}, {3,  4}
      },
      {
        {5,  6}, {7,  8}
      }
    }
  '::int[]);

select k, array_ndims(v) as "ndims", v::text as "v::text" from t order by k;
```
Notice that the three different `INSERT` statements define arrays with different dimensionality, as the comments state. This illustrates what was explained in [Synopsis](../#synopsis): the column "_t.v"_ can hold array values of _any_ dimensionality.

Here is the `SELECT` result:

```
 k | ndims |                    v::text                    
---+-------+-----------------------------------------------
 1 |     1 | {1,2}
 2 |     2 | {{1,2},{3,4}}
 3 |     3 | {{{1,2},{3,4}},{{5,6},{7,8}}}
 4 |     3 | [3:4][5:6][7:8]={{{1,2},{3,4}},{{5,6},{7,8}}}
```
Again, whitespace in the inserted literals for numeric values is insignificant, and the `text` typecasts use whitespace (actually, the lack thereof) conventionally.

The literal for a multidimensional array has nested `{}` pairs, according to the dimensionality, and the innermost pair contains the literals for the primitive values.

Notice the spelling of the array literal for the row with _"k = 4"_. The optional syntax `[3:4][5:6][7:8]` specifies the lower and upper bounds, respectively, for the first, the second, and the third dimension. This is the same syntax that you use to specify a slice of a array. ([array slice operator](../../functions-operators/slice-operator)) is described in its own section.) When the freedom to specify the bounds is not exercised, then they are assumed all to start at `1`, and then the canonical form of the literal does not show the bounds.

When the freedom is exercised, the bounds for _every_ dimension must be specified. Specifying the bounds gives you, of course, an opportunity for error. If the length along each axis that you (implicitly) specify doesn't agree with the lengths that emerge from the actual values listed between the surrounding outer `{}` pair, then you get the _"22P02 invalid_text_representation"_ error with this prose explanation:

```
malformed array literal...
Specified array dimensions do not match array contents.
```
