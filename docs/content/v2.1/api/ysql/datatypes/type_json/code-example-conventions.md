---
title: Code example conventions
linkTitle: Code example conventions
summary: Code example conventions
description: Code example conventions for JSON functions and operators.
block_indexing: true
menu:
  v2.1:
    identifier: code-example-conventions
    parent: api-ysql-datatypes-json
    weight: 30
isTocNested: true
showAsideToc: true
---

## Note about the code examples

The functionality of each operator and function is illustrated by a code example of this form:

```
[function or operator]
  acts on
[argument (list)]
  to produce
[result value]
```

It's insufficient to present such examples in the style "run this SQL at the `ysqlsh` prompt" followed by "see this result". A SQL-only demonstration has these disadvantages:

- It's hard to establish and advertise data type information without inserting into appropriately defined table columns. This brings distracting verbosity.
- The only way to build an expression from subexpressions, in pursuit of clarity, is to use scalar subqueries, named in a `with` clause. This, again, is so verbose that it obscures, rather than helps, clarity.
- This insufficiency is especially bothersome when the aim is to show how the output of a particular function depends upon the choice of value for an optional `boolean` parameter that conditions is behavior using, therefore, the same input JSON value in two function invocations.
- The result is a non-negotiable typecast to `text` to print to the screen, with additional distracting conventions like, for example, inserting a space at the start of each printed line, showing the `boolean` value `TRUE` as the `text` value `t`, showing `NULL` as just an absence, and showing a newline as the `text` value `+`.

For these reasons, each code example is presented as a `DO` block with this pattern:

- Each input value is declared using the appropriate SQL data type (sometimes building such values bottom-up from declared simpler values).
- The output of the operator or expression is assigned to a variable of the appropriate data type.
- The expected output is declared as a value of the same data type.
- An `ASSERT` is used to show that the produced value is equal to the expected value, using an `IS NULL` comparison where appropriate.

## Note about SQL array literals

RFC 7159 defines the syntax for a JSON _array_ as a comma-separated list of items surrounded by `[]` and  the syntax for a JSON _object_ as a comma-separated list of key-value pairs surrounded by `{}`. The literal for a SQL array is a `text` value with an inner syntax, typecasted the the array's data type: the value starts with `{` and ends with `}` and contains a comma-separated list whose items are not themselves single-quoted but are all taken to be values of the array's data type. So this SQL array value:

```
array['a', 'b', 'c']::text[]
```

can also be written thus:

```
'{a, b, c}'::text[]
```

See the section [Creating an array value using a literal](../../../type_array/literals) for more information on this topic. This dramatic context-sensitive difference in meaning of `'{...}'` might confuse the reader. Therefore, in the major section _"JSON data types and functionality"_, the `array[...]` constructor form will be used for a SQL array value—and the use of the `'{...}'` SQL array literal will be avoided.

The fact that a JSON _array_ can have subvalues of mixed data type but a SQL array can have only elements of the same data type means that special steps have to be taken when the goal is to construct a JSON _array_ mixed subvalue data type from SQL values.