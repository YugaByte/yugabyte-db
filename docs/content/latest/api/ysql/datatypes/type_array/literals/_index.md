---
title: Creating an array value using a literal
linkTitle: Literals
headerTitle: Creating an array value using a literal
description: Creating an array value using a literal
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: array-literals
    parent: api-ysql-datatypes-array
    weight: 20
isTocNested: true
showAsideToc: true
---

This section introduces array literals informally with a few examples. Its subsections, listed below, explain formally how you construct syntactically correct array literals that establish the values that you intend.

An array literal starts with a left curly brace. This is followed by some number of comma-separated literal representations for the array's values. Sometimes, the value representations need not be double-quoted—but _may_ be. And sometimes the value representations must be double-quoted. The array literal then ends with a right curly brace. Depending on the array's data type, its values might be scalar, or they might be composite. For example, they might be _"row"_ type values; or they might be arrays. The literal for a multidimensional array is written as an array of arrays of arrays... and so on. They might even be values of a user-defined `DOMAIN` which is based on an array data type. This powerful notion is discussed in the dedicated section [Using an array of `DOMAIN` values](../array-of-domains/).

To use such a literal in SQL or in PL/pgSQL it must be enquoted in the same way as is an ordinary `text` literal. You can enquote an array literal using dollar quotes, if this suits your purpose, just as you can for a `text` literal. You sometimes need to follow the closing quote with a suitable typecast operator for the array data type that you intend. And sometimes the context of use uniquely determines the literal's data type. It's never wrong to write the typecast explicitly—and it's a good practice always to do this.

Here, in use in a SQL `SELECT` statement, is the literal for a one-dimensional array of primitive `int` values:
```plpgsql
\t on
select '{1, 2, 3}'::int[];
```
The `\t on` metacommand suppresses column headers and the rule-off under these. Unless the headers are important for understanding, query output from `ysqlsh` will be shown, throughout the present "arrays" major section, without these.

This is the output that the first example produces:

```
 {1,2,3}
```
The second example surrounds the values that the array literal defines with double quotes:
```plpgsql
select '{"1", "2", "3"}'::int[];
```
It produces the identical output to the first example, where no double quotes were used.

The third example defines a two-dimensional array of `int` values:

```plpgsql
select '
   {
      {11, 12, 13},
      {21, 22, 23}
    }
  '::int[];
```

It produces this result:

```
 {{11,12,13},{21,22,23}}
```

The fourth example defines an array whose values are instances of a _"row"_ type:

```plpgsql
create type rt as (f1 int, f2 text);

select '
  {
    "(1,a1 a2)",
    "(2,b1 b2)",
    "(3,c1 v2)"
  }
'::rt[];
```
It produces this output:
```
 {"(1,\"a1 a2\")","(2,\"b1 b2\")","(3,\"c1 v2\")"}
```
All whitespace (except, of course, within the text values) has been removed. The double quotes around the representation of each _"row"_ type value are retained. This suggests that they are significant. (Test this by removing them. It causes the _"22P02: malformed row literal"_ error.) Most noticeably, there are clearly rules at work in connection with the representation of each `text` value within the representation of each _"row"_ type value.

The following sections present the rules carefully and, when the rules allow some freedom, give recommendations.

[The text typecast of a value, the literal for that value, and how they are related](./text-typecasting-and-literals/) establishes the important notions that allow you to distinguish between a _literal_ and the _text of the literal_. It's the _text_ of an array literal that, by following specific grammar rules for this class of literal, actually defines the intended value. The literal, as a whole, enquotes this bare text and typecasts it to the desired target array data type.

[The literal for an array of primitive values](./array-of-primitive-values/) gives the rules for array literals whose values are scalars (for example, are of primitive data types).

[The literal for a _"row"_ type value](./row/) gives the rules for the literal for a value of a _"row"_ type. These rules are essential to the understanding of the next section.

[The literal for an array of _"row"_ type values](./array-of-rows/) gives the rules for array literals whose values are composite (that is, a _"row"_ type).

These rules are covered in the following sections of the PostgreSQL documentation:

- [8.15. Arrays](https://www.postgresql.org/docs/11/arrays.html)

- [8.16. Composite Types](https://www.postgresql.org/docs/11/rowtypes.html)
