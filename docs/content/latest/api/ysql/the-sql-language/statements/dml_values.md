---
title: VALUES statement [YSQL]
headerTitle: VALUES
linkTitle: VALUES
description: Use the VALUES statement to generate a row set specified as an explicitly written set of explictly written tuples.
menu:
  latest:
    identifier: dml_values
    parent: statements
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `VALUES` statement to generate a row set specified as an explicitly written set of explicitly written tuples.

## Syntax

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link active" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <i class="fas fa-file-alt" aria-hidden="true"></i>
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <i class="fas fa-project-diagram" aria-hidden="true"></i>
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="grammar" class="tab-pane fade show active" role="tabpanel" aria-labelledby="grammar-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/values,expression_list.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/values,expression_list.diagram.md" /%}}
  </div>
</div>

## Semantics

### expression_list

A comma separated list of parenthesized expression lists. The degenerate form is just a single constant, thus:

```plpgsql
values ('dog'::text);
```
This is the result:

```
 column1 
---------
 dog
```

The result has as many columns named _"column1"_, _"column2"_, ... _"columnN"_ as there are expressions in the expression list, thus:

```plpgsql
values
  (1::int, '2019-06-25 12:05:30'::timestamp, 'dog'::text),
  (2::int, '2020-07-30 13:10:45'::timestamp, 'cat'::text);
```
This is the result:

```
 column1 |       column2       | column3 
---------+---------------------+---------
       1 | 2019-06-25 12:05:30 | dog
       2 | 2020-07-30 13:10:45 | cat
```
If an expression is written without a typecast, then its data type is inferred. For example, _'dog'_ is inferred to have datatype `text` and _4.2_ is inferred to have data type `numeric`.

Each successive parenthesized expression list must specify the same number of expressions with the same data types. Try this counter example:

```plpgsql
values
  (1::int, '2019-06-25 12:05:30'::timestamp, 'dog'::text),
  (2::int, '2020-07-30 13:10:45'::timestamp, 'cat'::text);
```

It causes this error:

```
42601: VALUES lists must all be the same length
```

And try this counter example:

```plpgsql
values (1::int), ('x'::text);
```
It causes this error:

```
42804: VALUES types integer and text cannot be matched
```

### The ORDER BY, LIMIT, OFFSET, and FETCH clauses

These clauses have the same semantics when they are used in a `VALUES` statement as they do when they are used in a `SELECT` statement.

## Example

A `VALUES` statement can be used as a subquery by surrounding it with parentheses, and giving this an alias, in just the same way that a `SELECT` statement can be so surrounded and so used. Try this first:

```plpgsql
select chr(v) as c from (
  select * from generate_series(97, 101)
  ) as t(v);
```

This is the result:

```
 c 
---
 a
 b
 c
 d
 e
```

Now use a `VALUES` statement (on line #2) within the parentheses instead of the `SELECT` statement:

```plpgsql
select chr(v) as c from (
  values (100), (111), (103)
  ) as t(v);
```

This is the result:

```
 c 
---
 d
 o
 g
```





