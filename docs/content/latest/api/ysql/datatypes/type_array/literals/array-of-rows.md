---
title: The literal for an array of rows
linkTitle: Array of rows
headerTitle: The literal for an array of "row" type values
description: The literal for an array of "row" type values
menu:
  latest:
    identifier: array-of-rows
    parent: array-literals
    weight: 40
isTocNested: true
showAsideToc: true
---

You now combine the understanding of how to write the literal for an array of primitive values with that of how to write the literal for a _"row"_ type value.

This section uses the same approach as these sections: [The literal for an array of primitive values](../array-of-primitive-values/) and [The literal for a _"row"_ type value](../row/). First, it states the rules, and then it illustrates these with examples.

## Statement of the rules

Just as in [Statement of the rules](../array-of-primitive-values/#statement-of-the-rules) that stated the rules for literals for an array of primitive values, the statement of these rules depends on understanding the notion of the canonical form of a literal.

If you follow the rules that are stated here and illustrated in the demonstration below, then you will always produce a syntactically valid literal which expresses the semantics that you intend. There are many other legal variants—especially because of the freedoms for `text[]` values. This can also produce the result that you intend. However, these rules will not be documented because it is always sufficient to create your literals in canonical form.

The sufficient set of rules can be stated tersely:

- Start off with the opening left curly brace.

- First, prepare the literal for each _"row"_ type value according to the rules set out in [The literal for a _"row"_ type value](../row/).

- Then, understand that when these are used within the literal for _"row"_ type value within the literal for an array, the _"row"_ must itself be surrounded with double quotes, just like is the rule for, say, `timestamp` values or `text` values that include spaces or other troublesome characters.

- Then understand that this implies that any occurrences of double quotes and backslashes within the surrounding parentheses of the _"row"_ type literal must be escaped a second time: _double-quote_ becomes _backslash-double-quote_; and _backslash_ becomes _backslash-backslash_.

- Therefore, to avoid wrongly escaping the double quotes that will surround the parentheses,

  - _first_, do the inner escaping

  - _and only then_, surround the complete representation for the _"row"_ type value with unescaped double quotes.

- Finish off with the closing right curly brace.

These rules are presented in [Pseudocode for generating the literal for a one-dimensional array of "row" type values](./#pseudocode-for-generating-the-literal-for-a-one-dimensional-array-of-row-type-values).

## Example to illustrate the rules

The example uses a _"row"_ type with four fields: an `int` field; a `text` field; a `timestamp` field; and a `boolean` field. This is enough to illustrate all of the rules. These "challenging" characters need particular care:
```
     <space>     ,     (     )     "     \
```
First, create the _"row"_ type:
```plpgsql
create type rt as (n int, s text, t timestamp, b boolean);
```
Next, you create a table with a column with data type _"rt"_ so that you can populate it with six rows that jointly, in their `text` fields, use all of the "challenging" characters listed above:
```plpgsql
create table t1(k int primary key, v rt);
```
Finally, you populate the table by building the _"row"_ type values bottom-up using appropriately typed PL/pgSQL variables in a `DO` block and inspect the result. This technique allows the actual primitive values that were chosen for this demonstration so be seen individually as the ordinary SQL literals that each data type requires. This makes the code more readable and more understandable than any other approach. In other words, it shows that, for humanly written code, the usability of a value constructor for any composite value is much greater than that of the literal that produces the same value. Of course, this benefit is of no consequence for a programmatically constructed literal.
```plpgsql
do $body$
declare
  n1 constant int := 1;
  s1 constant text := ' ';
  t1 constant timestamp := '2091-01-20 12:10:05';
  b1 constant boolean := true;

  n2 constant int := 2;
  s2 constant text := ',';
  t2 constant timestamp := '2002-01-20 12:10:05';
  b2 constant boolean := false;

  n3 constant int := 3;
  s3 constant text := '(';
  t3 constant timestamp := '2003-01-20 12:10:05';
  b3 constant boolean := null;

  n4 constant int:= 4;
  s4 constant text := ')';
  t4 constant timestamp := '2004-01-20 12:10:05';
  b4 constant boolean := true;

  n5 constant int:= 5;
  s5 constant text := '"';
  t5 constant timestamp := '2005-01-20 12:10:05';
  b5 constant boolean := false;

  n6 constant int:= 6;
  s6 constant text := '\';
  t6 constant timestamp := '2006-01-20 12:10:05';
  b6 constant boolean := null;
begin
  insert into t1(k, v) values
    (1, (n1, s1, t1, b1)),
    (2, (n2, s2, t2, b2)),
    (3, (n3, s3, t3, b3)),
    (4, (n4, s4, t4, b4)),
    (5, (n5, s5, t5, b5)),
    (6, (n6, s6, t6, b6));
end;
$body$;

select v::text as lit from t1 order by k;
```

This is the result:
```
               lit                
----------------------------------
 (1," ","2091-01-20 12:10:05",t)
 (2,",","2002-01-20 12:10:05",f)
 (3,"(","2003-01-20 12:10:05",)
 (4,")","2004-01-20 12:10:05",t)
 (5,"""","2005-01-20 12:10:05",f)
 (6,"\\","2006-01-20 12:10:05",)
```

The `int` field and the `timestamp` field are unremarkable given only that you understand that the representation of the `timestamp` values, in order to meet the canonical form requirement, must be double-quoted. The `boolean` fields are unremarkable, too, as long as you remember that `NULL` is represented by leaving no space between the delimiters that surround that field. This leaves just the `text` fields for consideration. Here are the field representations themselves, without the clutter of the delimiters:
```
     " "     ","     "("     ")"     """"     "\\"
```

The first four are unremarkable, as long as you remember that each of these four single characters, as shown at the start, must be ordinarily surrounded by double quotes. That leaves just the last two:

- The single double quote occurrence, in the source data, must be doubled up and then surrounded by double quotes.
- The single backslash occurrence, in the source data, must be doubled up and then surrounded by double quotes.

Next, you concatenate these six _"row"_ type values into an array value by using the `array_agg()` function (described in [`array_agg()`](../../functions-operators/array-agg-unnest/#array-agg)), like this:
```plpgsql
select array_agg(v order by k) from t1;
```

The demonstration is best served by inserting this value into a new table, like this:
```plpgsql
create table t2(k int primary key, v1 rt[], v1_text_typecast text, v2 rt[]);
insert into t2(k, v1)
select 1, array_agg(v order by k) from t1;
```

The `\get` technique that you used in the earlier sections is not viable here because there's an upper limit on its size. So, instead insert the literal that you produce by `text` typecasting _"t2.v1"_ into the companion _"v1&#95;text&#95;typecast"_ field in the same table, like this:


```plpgsql
update t2 set v1_text_typecast =
(select v1::text from t2 where k = 1);
```
Finally, use this array literal to recreate the original value and check that it's identical to what you started with, thus:
```plpgsql
update t2 set v2 = 
(select v1_text_typecast from t2 where k = 1)::rt[];

select (v1 = v2)::text as "v1 = v2" from t2 where k = 1;
```
As promised, the canonical form of the array literal does indeed recreate the identical value that the `array_agg()` function created:

```
 v1 = v2 
---------
 true
```

You haven't yet looked at the literal for the array of _"row"_ type values. Now is the moment to do so, thus:
```plpgsql
select v1_text_typecast from t2 where k = 1;
```
The result that's produced is too hard to read without some manual introduction of whitespace. But this is allowed around the commas that delimit successive values within an array literal, thus:

```
{
  "(1,\"a \",\"2091-01-20 12:10:05\",t)",
  "(2,\", \",\"2002-01-20 12:10:05\",f)",
  "(3,\"( \",\"2003-01-20 12:10:05\",)",
  "(4,\" )\",\"2004-01-20 12:10:05\",t)",
  "(5,\"\"\"\",\"2005-01-20 12:10:05\",f)",
  "(6,\"\\\\\",\"2006-01-20 12:10:05\",)"
}
```

With some effort, you'll see that this is indeed the properly formed canonical representation for the literal for an array of _"row"_ type values that the rules set out above specify.

## Multidimensional array of "row" type values

You can work out the rules for a multidimensional array of _"row"_ type values, should you need these, by straightforward induction from what has already been explained this enclosing section.

## Pseudocode for generating the literal for a one-dimensional array of "row" type values

This pseudocode shows how to create an array literal of _"row"_ type values that have the same shape as _"type rt"_ in the example above. The input is a succession of an arbitrary number of _"(n, s, t, b)"_ tuples. The text below was derived by straightforward manual massage from actual working, and tested, Python code. The code was written as an exercise to verify the correctness of the algorithm.

The pseudocode does retain Python locutions, but don't be distracted by this. The meaning is clear enough to allow the algorithm to be described. The various special characters were all set up as manifest constants with self-describing names.

Notice that the algorithm inserts a newline after the opening curly brace, between the pairs of representations of each _"row"_ type value, and before the closing curly brace. While, strictly speaking, this means that the literal it produces is not in canonical form, this has no effect (as has been shown many times by example throughout this _"Array data types and functionality"_ major section).

```
"Start a new array literal":
  wip_literal = lft_crly_brace + nl

"For each next (n, s, t, b) tuple that defines a "row" type value":
  curr_rec = dbl_quote + lft_parens

  # Field "n" maps to a SQL numeric
  if n is None:
    curr_rec += comma
  else:
    curr_rec += (str(n) + comma)

  # Field "s" maps to a SQL text.
  if s is None:
    curr_rec += comma
  else:
    # First, do the escaping needed for any stringy value
    # as field in record literal value.
    s = s.replace(bk_slash, two_bk_slashes)
    s = s.replace(dbl_quote, two_dbl_quotes)
    s = dbl_quote + s + dbl_quote

    # Next, do the escaping to fix the bare record representation
    # for use as a array element.
    s = s.replace(bk_slash, two_bk_slashes)
    s = s.replace(dbl_quote, bk_slash_dbl_quote)
    curr_rec += (s + comma)

    # Field "t" maps to a SQL timestamp.
    if t is None:
      curr_rec += comma
    else:
      curr_rec += (bk_slash_dbl_quote + t + bk_slash_dbl_quote + comma)

    # Field "b" maps to a SQL boolean.
    # It's the last field, do nothing if it's neither "t" nor "f"
    if (b == "t" or b == "f"):
      curr_rec += b

  # Now there are no more inout tuples.
  curr_rec = curr_rec + rgt_parens + dbl_quote
  wip_literal = wip_literal + curr_rec + comma + nl

# Now there are no more input tuples.
"Finish off":
  # Remove the final (comma + nl), put the nl back,
  # and add the closing curly brace.
  wip_literal = wip_literal[:-2] + nl + rgt_crly_brace
```
