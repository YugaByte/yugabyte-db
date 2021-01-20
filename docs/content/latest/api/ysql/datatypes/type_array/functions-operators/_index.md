---
title: Array functions and operators
linkTitle: Functions and operators
headerTitle: Array functions and operators
description: Array functions and operators
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: array-functions-operators
    parent: api-ysql-datatypes-array
    weight: 90
isTocNested: true
showAsideToc: true
---

**On this page**<br>
&#160;&#160;&#160;&#160;[Functions for creating arrays from scratch](./#functions-for-creating-arrays-from-scratch)<br>
&#160;&#160;&#160;&#160;[Functions for reporting the geometric properties of an array](./#functions-for-reporting-the-geometric-properties-of-an-array)<br>
&#160;&#160;&#160;&#160;[Functions to find a value in an array](./#functions-to-find-a-value-in-an-array)<br>
&#160;&#160;&#160;&#160;[Operators to test whether a value is in an array](./#operators-to-test-whether-a-value-is-in-an-array)<br>
&#160;&#160;&#160;&#160;[Operators for comparing two arrays](./#operators-for-comparing-two-arrays)<br>
&#160;&#160;&#160;&#160;[The slice operator](./#the-slice-operator)<br>
&#160;&#160;&#160;&#160;[Functions and operators for concatenating an array with an array or an element](./#functions-and-operators-for-concatenating-an-array-with-an-array-or-an-element)<br>
&#160;&#160;&#160;&#160;[Functions and operators to change values in an array](./#functions-and-operators-to-change-values-in-an-array)<br>
&#160;&#160;&#160;&#160;[Function to convert an array to a text value](./#function-to-convert-an-array-to-a-text-value)<br>
&#160;&#160;&#160;&#160;[Table function to transform an array into a `SETOF anyelement`](./#table-function-to-transform-an-array-into-a-setof-anyelement)

**Note:** For an alphabetical listing of the array functions and operators, see the listing in the navigation bar.

Most of the functions and operators listed here can use an array of any dimensionality, but four of the functions accept, or produce, only a one-dimensional array. This property is called out by the second column _"1-d only?"_ in the tables that follow. The restricted status is indicated by _"1-d"_ in that function's row. When the field is blank, there is no dimensionality restriction.

## Functions for creating arrays from scratch

The `array[]` constructor, and the three functions, create an array from scratch.

| Function or operator | 1-d only? | Description |
| ---- | ---- | ---- |
| [`array[]`](./../array-constructor/) | | The array[] value constructor is a special variadic function that creates an array value from scratch using an expression for each of the array's values. Such an expression can itself use the `array[]` constructor or an [array literal](../literals/). |
| [`array_fill()`](./array-fill/) | | Returns a new "blank canvas" array of the specified shape with all cells set to the same specified value. |
| [`array_agg()`](./array-agg-unnest/#array-agg) | | Returns an array (of an implied _"row"_ type) from a SQL subquery. |
| [`string_to_array()`](./string-to-array/) | 1-d | Returns a one-dimensional `text[]` array by splitting the input `text` value into subvalues using the specified `text` value as the delimiter. Optionally, allows a specified `text` value to be interpreted as `NULL`. |

## Functions for reporting the geometric properties of an array

| Function | 1-d only? | Description |
| ---- | ---- | ---- |
| [`array_ndims()`](./properties/#array-ndims) | | Returns the dimensionality of the specified array. |
| [`array_lower()`](./properties/#array-lower) | | Returns the lower bound of the specified array along the specified dimension. |
| [`array_upper()`](./properties/#array-upper) | | Returns the upper bound of the specified array along the specified dimension. |
| [`array_length()`](./properties/#array-length) | | Returns the length of the specified array along the specified dimension. |
| [`cardinality()`](./properties/#cardinality) | | Returns the total number of values in the specified array. |
| [`array_dims()`](./properties/#array-dims) | | Returns a text representation of the same information as `array_lower()` and `array_length()`, for all dimensions, in a single text value. |

## Functions to find a value in an array

| Function | 1-d only? | Description |
| ---- | ---- | ---- |
| [`array_position()`](./array-position/#array-position) | 1-d | Returns the index, in the supplied array, of the specified value. Optionally starts searching at the specified index. |
| [`array_positions()`](./array-position/#array-positions) | 1-d | Returns the indexes, in the supplied array, of all occurrences the specified value. |

## Operators to test whether a value is in an array

These operators require that the [LHS](https://en.wikipedia.org/wiki/Sides_of_an_equation) is a scalar and that
the [RHS](https://en.wikipedia.org/wiki/Sides_of_an_equation) is an array of that LHS's data type.

| Operator | 1-d only? | Description |
| ---- | ---- | ---- |
| [`ANY`](./any-all/) | | Returns `TRUE` if _at least one_ of the specified inequality tests between the LHS element and each of the RHS array's elements evaluates to `TRUE`. |
| [`ALL`](./any-all/) | | Returns `TRUE` if _every one_ of the specified inequality tests between the LHS element and each of the RHS array's elements evaluates to `TRUE`. |

## Operators for comparing two arrays

These operators require that the [LHS and RHS](https://en.wikipedia.org/wiki/Sides_of_an_equation) arrays have the same data type.

| Operator | 1-d only? | Description |
| ---- | ---- | ---- |
| [`=`](./comparison/#the-160-160-160-160-and-160-160-160-160-operators) | | Returns `TRUE` if the LHS and RHS arrays are equal. |
| [`<>`](./comparison/#the-160-160-160-160-and-160-160-160-160-operators) | | Returns `TRUE` if the LHS and RHS arrays are not equal. |
| [`>`](./comparison/#the-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-operators) | | Returns `TRUE` if the LHS array is greater than the RHS array. |
| [`>=`](./comparison/#the-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-operators) | | Returns `TRUE` if the LHS array is greater than or equal to the RHS array. |
| [`<=`](./comparison/#the-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-operators) | | Returns `TRUE` if the LHS array is less than or equal to the RHS array. |
| [`<`](./comparison/#the-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-and-160-160-160-160-operators) | | Returns `TRUE` if the LHS array is less than the RHS array. |
| [`@>`](./comparison/#the-160-160-160-160-and-160-160-160-160-operators-1) | | Returns `TRUE` if the LHS array contains the RHS array—that is, if every distinct value in the RHS array is found among the LHS array's distinct values. |
| [`<@`](./comparison/#the-160-160-160-160-and-160-160-160-160-operators-1) | | Returns `TRUE` if the LHS array is contained by the RHS array—that is, if every distinct value in the LHS array is found among the RHS array's distinct values. |
| [`&&`](./comparison/#the-160-160-160-160-operator) | | Returns `TRUE` if the LHS and RHS arrays overlap—that is, if they have at least one value in common. |


## The slice operator

| Operator | 1-d only? | Description |
| ---- | ---- | ---- |
|[`[lb1:ub1]...[lbN:ubN]`](./slice-operator/) | | Returns a new array whose length is defined by specifying the slice's lower and upper bound along each dimension. These specified slicing bounds must not exceed the source array's bounds. The new array has the same dimensionality as the source array and its lower bound is `1` on each axis. |

## Functions and operators for concatenating an array with an array or an element

These functions require that the two arrays have the same data type and compatible dimensionality.

| Function or operator | 1-d only? | Description |
| ---- | ---- | ---- |
| [`||`](./concatenation/#the-160-160-160-160-operator) | | Returns the concatenation of any number of compatible `anyarray` and `anyelement` values. |
| [`array_cat()`](./concatenation/#array-cat) | | Returns the concatenation of two compatible `anyarray` values. |
| [`array_append()`](./concatenation/#array-append) | | Returns an array that results from appending a scalar value to (that is, _after_) an array value. |
| [`array_prepend()`](./concatenation/#array-prepend) | | Returns an array that results from prepending a scalar value to (that is, _before_) an array value. |

## Functions and operators to change values in an array

| Function or operator | 1-d only? | Description |
| ---- | ---- | ---- |
| [`array_replace()`](./replace-a-value/#array-replace) | | Returns a new array where every occurrence of the specified value in the input array has been replaced by the specified new value. |
| [`arr[idx_1]...[idx_N] := val`](./replace-a-value/#setting-an-array-value-explicitly-and-in-place) | | Update a value in an array "in place". |
| [`array_remove()`](./array-remove) | 1-d | Returns a new array where _every_ occurrence of the specified value has been removed from the specified input array. |

## Function to convert an array to a text value

| Function | 1-d only? | Description |
| ---- | ---- | ---- |
| [`array_to_string()`](./array-to-string) | | Returns a `text` value computed by representing each array value, traversing these in row-major order, by its `::text` typecast, using the supplied delimiter between each such representation. (The result, therefore, loses all information about the arrays geometric properties.) Optionally, represent `NULL` by the supplied `text` value. |

## Table function to transform an array into a SETOF anyelement

| Function | 1-d only? | Description |
| ---- | ---- | ---- |
| [`unnest()`](./array-agg-unnest/#unnest) | | Use in the `FROM` clause of a `SELECT` statement. The simple overload accepts a single `anyarray` value and returns a `SETOF anyelement`. The exotic overload accepts a variadic list of `anyarray` values and returns a `SETOF` with many columns where each, in turn, has the output of the corresponding simple overload. |

## Table function to transform an array into a SETOF index values

| Function | 1-d only? | Description |
| ---- | ---- | ---- |
| [`generate_subscripts()`](./array-agg-unnest/#generate-subscripts) | | Use in the `FROM` clause of a `SELECT` statement. Returns the values of the indexes along the specified dimension of the specified array. |
