---
title: The array[] value constructor
linkTitle: array[] constructor
headerTitle: The array[] value constructor
description: The array[] value constructor
block_indexing: true
menu:
  v2.1:
    identifier: array-constructor
    parent: api-ysql-datatypes-array
    weight: 10
isTocNested: true
showAsideToc: true
---

The `array[]` value constructor is a special variadic function. Uniquely among all the functions described in this _"Array data types and functionality"_ major section, it uses square brackets (`[]`) to surround its list of actual arguments.

## Purpose and signature

**Purpose:** Create an array value from scratch using an expression for each of the array's values. Such an expression can itself use the `array[]` constructor or an [array literal](../literals/). 

**Signature** 
```
input value:       [anyarray | [ anyelement, [anyelement]* ]
return value:      anyarray
```
**Note:** You can meet the goal of creating an array from directly specified values, instead, by using an [array literal](../literals/).

These thee ordinary functions also create an array value from scratch:

- The [`array_fill()`](../functions-operators/array-fill/) function creates a "blank canvas" array of the specified shape with all values set the same to what you want.
- The [`array_agg()`](../functions-operators/array-agg-unnest/#array-agg) function creates an array (of, in general, an implied _"row"_ type) from a SQL subquery.
- The [`text_to_array()`](../functions-operators/string-to-array/) function creates a `text[]`array from a single `text` value that uses a a specifiable delimiter to beak it into individual values.

**Example:**
```postgresql
create type rt as (f1 int, f2 text);
select array[(1, 'a')::rt, (2, 'b')::rt, (3, 'dog \ house')::rt]::rt[] as arr;
```
This is the result:
```
                    arr                     
--------------------------------------------
 {"(1,a)","(2,b)","(3,\"dog \\\\ house\")"}
```
Whenever an array value is shown in `ysqlsh`, it is implicitly `::text` typecasted. And this `text` value can be used immediately, by enquoting it and typecasting it to the appropriate array data type, to recreate the starting value. The YSQL documentation refers to this form of the literal as its _canonical form_. It is characterized by its complete lack of whitespace except within `text` scalar values and within date-time scalar values. This term is defined formally in [Defining the canonical form of a literal](../literals/text-typecasting-and-literals/#defining-the-canonical-form-of-a-literal).

To learn why you see four consecutive backslashes, see [Statement of the rules](../literals/array-of-rows/#statement-of-the-rules).

Users who are familiar with the rules that are described in that section often find it expedient, for example when prototyping code that builds an array literal, to create an example value first, _ad hoc_, using the `array[]` constructor, like the code above does, to see an example of the syntax that their code must create programmatically.

## Using the array[] constructor in PL/pgSQL code

The example below attempts to make many teaching points in one piece of code.

- The actual syntax, when the expressions that the `array[]` constructor uses are all literals, is far simpler than the syntax that governs how to construct an array literal.
- You can use all of YSQL's array functionality in PL/pgSQL code, just as you can in SQL statements. The code creates and invokes a table function, and not just a `DO` block, to emphasize this interoperability point.
- Array-like functionality is essential in any programming language.
- The `array[]` constructor is most valuable when the expressions that it uses are composed using declared variables, and especially formal parameters, that are used to build whatever values are intended. In this example, the values have the user-defined data type _"rt"_. In other words, the `array[]` constructor is particularly valuable when you build an array programmatically from scalar values that you know first at run time.
- It vividly demonstrates the semantic effect of the `array[]` constructor like this:
```
declare
  r     rt[];
  two_d rt[];
begin
  ...
  assert (array_dims(r) = '[1:3]'), 'assert failed';
  one_d_1 := array[r[1], r[2], r[3]];
  assert (one_d_1 = r), 'assert failed';
```
The [`array_dims()`](../functions-operators/properties/#array-dims) function is documented in the _"Functions for reporting the geometric properties of an array"_ section.

Run this to create the required user-defined _"row"_ type and the table function and then to invoke it.

```postgresql
-- Don't create "type rt" if it's still there following the previous example.
create type rt as (f1 int, f2 text);

create function some_arrays()
  returns table(arr text)
  language plpgsql
as $body$
declare
  i1 constant int := 1;
  t1 constant text := 'a';
  r1 constant rt := (i1, t1);

  i2 constant int := 2;
  t2 constant text := 'b';
  r2 constant rt := (i2, t2);

  i3 constant int := 3;
  t3 constant text := 'dog \ house';
  r3 constant rt := (i3, t3);

  a1 constant rt[] := array[r1, r2, r3];
begin
  arr := a1::text;
  return next;

  declare
    r rt[];
    one_d_1 rt[];
    one_d_2 rt[];
    one_d_3 rt[];
    two_d   rt[];
    n int not null := 0;
  begin
    ----------------------------------------------
    -- Show how arrays are useful, in the classic
    -- sense, as what EVERY programming language
    -- needs to handle a number of items when the
    -- number isn't known until run time.
    for j in 1..3 loop
      n := j + 100;
      r[j] := (n, chr(n));
    end loop;

    -- This further demonstrates the semantics
    -- of the array[] constructor.
    assert (array_dims(r) = '[1:3]'), 'assert failed';
    one_d_1 := array[r[1], r[2], r[3]];
    assert (one_d_1 = r), 'assert failed';
    ----------------------------------------------

    one_d_2 := array[(104, chr(104)), (105, chr(105)), (106, chr(106))];
    one_d_3 := array[(107, chr(107)), (108, chr(108)), (109, chr(109))];

    -- Show how the expressions that define the outcome
    -- of the array[] constructor can themselves be arrays.
    two_d := array[one_d_1, one_d_2, one_d_3];
    arr := two_d::text;
    return next;
  end;
  
end;
$body$;

select arr from some_arrays();
```
It produces two rows. This is the first:

```
                    arr                     
--------------------------------------------
 {"(1,a)","(2,b)","(3,\"dog \\\\ house\")"}
```

And this is the second row. The readability was improved by adding some whitespace manually:

```
{
  {"(101,e)","(102,f)","(103,g)"},
  {"(104,h)","(105,i)","(106,j)"},
  {"(107,k)","(108,l)","(109,m)"}
}
```

## Using the array[] constructor in a prepared statement

This example emphasizes the value of using the `array[]` constructor over using an array literal because it lets you use expressions like `chr()` within it.
```postgresql
-- Don't create "type rt" if it's still there followng the previous examples.
create type rt as (f1 int, f2 text);
create table t(k serial primary key, arr rt[]);

prepare stmt(rt[]) as insert into t(arr) values($1);

-- It's essential to typecast the individual "rt" values.
execute stmt(array[(104, chr(104))::rt, (105, chr(105))::rt, (106, chr(106))::rt]);
```
This execution of the prepared statement, using an array literal as the actual argument, is semantically equivalent:
```postgresql
execute stmt('{"(104,h)","(105,i)","(106,j)"}');
```
But here, of course, you just have to know in advance that `chr(104)` is `h`, and so on. Prove that the results of the two executions of the prepared statement are identical thus:

```postgresql
select
  (
    (select arr from t where k = 1)
    =
    (select arr from t where k = 2)
  )::text as result;
```

It shows this:

```
 result 
--------
 true
```
