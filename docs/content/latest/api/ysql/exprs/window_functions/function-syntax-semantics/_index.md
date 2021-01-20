---
title: YSQL window functions signature and purpose
linkTitle: Per function signature and purpose
headerTitle: Signature and purpose of each window function
description: This section summarizes the signature and purpose of each of the YSQL window functions and links to their individual accounts.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: window-function-syntax-semantics
    parent: window-functions
    weight: 30
aliases:
  - /latest/api/ysql/exprs/window_functions
isTocNested: true
showAsideToc: true
---
**On this page**<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[Aggregate function variants](./#aggregate-function-variants)<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[`frame_clause` sensitivity](./#frame-clause-sensitivity)<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[Window functions that return an `int` or `double precision` value as a "classifier" of the position of the row within the window](./#window-functions-that-return-an-int-or-double-precision-value-as-a-classifier-of-the-rank-of-the-row-within-its-window)<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[Window functions that return columns of another row within the window](./#window-functions-that-return-column-s-of-another-row-within-the-window)

The two tables at the end classify the eleven built-in window functions into two groups according to their general common characteristics.

**Note:** The navigation bar lists these window functions in four functional groups. The members in each group bear a strong family resemblance to each other. The first two groups list functions from the [first table](./#window-functions-that-return-an-int-or-double-precision-value-as-a-classifier-of-the-rank-of-the-row-within-its-window) below. And the second two groups list functions from the [second table](./#window-functions-that-return-column-s-of-another-row-within-the-window).

### Aggregate function variants

A few of these also have an aggregate function variant. This can be seen with the `\df` metacommand. For example, `df lag` shows this:

```
 Result data type |          Argument data types           |  Type
------------------+----------------------------------------+--------
 bigint           |                                        | window
 bigint           | VARIADIC "any" ORDER BY VARIADIC "any" | agg
```
This property is marked by _"Y"_ in the column _"agg?"_ in the following tables; a blank in this column means that the entry has only a window function variant.

{{< note title="Functions with both a 'window' and an 'aggregate' variant" >}}

The definitive description of the use, as an aggregate function, of a window function that has such a variant, is described within the [Aggregate functions](../../aggregate_functions) major section in the section [Within-group hypothetical-set aggregate functions
](../../aggregate_functions/function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/).

{{< /note >}}

### Frame_clause sensitivity

The results of all of the window functions depend upon what the window `ORDER BY` clause and the optional `PARTITION BY` clause say. Though you don't get an error if you omit the window `ORDER BY` clause, its omission brings unpredictable and therefore meaningless results.

The results of a few of the window functions _are_ sensitive to what the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) says. This is marked by _"Y"_ in the column _"frame?"_ in the following tables; a blank in this column means that the entry is _not_ sensitive to what the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) says.

#### Frame_clause-insensitive window functions

All of the window functions listed in the first table, and `lag()` and `lead()` from the second table, are insensitive to whatever the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) might say. You can easily show this by trying a few variants like, for example, this:

```
-- Counter example. Don't use this for window functions
-- that aren't frame_clause-sensitive.
range between 1 preceding and 1 following exclude current row
```

It says "consider only the row immediately before and the row immediately after the current row". You'll see that including this, or any other variant, in the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) brings the identical result to what including only the window `ORDER BY` clause brings for each of the window functions that aren't sensitive to the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause).

Yugabyte recommends that you never include a [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) in the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) that you use when you invoke a window function that isn't sensitive to the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause).

#### Frame_clause-sensitive window functions

The names of the other window functions that the second table lists, `first_value()`, `nth_value()` and `last_value()`, tell you that the output of each makes obvious sense when the scope within which the specified row is found is the entire [_window_](../invocation-syntax-semantics/#the-window-definition-rule). The results of these three functions certainly _are_ sensitive to the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause). This is the default for the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause):

```
 -- This specification is used if you omit the frame_clause.
 -- You probably don't want the results that this specifies.
 between unbounded preceding and current row
```

See the section [Window function invocation—SQL syntax and semantics](../invocation-syntax-semantics). But the default does _not_ specify the entire [_window_](../invocation-syntax-semantics/#the-window-definition-rule). To do this, use this variant:

```plpgsql
-- You must specify this explicitly unless you are sure
-- that you want a different specification.
range between unbounded preceding and unbounded following
```

Yugabyte recommends, therefore, that you include [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) in the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) that you use when you invoke a window function that is sensitive to the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause), unless you have one of the very rare use cases where the output that you want is produced by a different [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause).

**Note:** The [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause)'s many variants are useful when an aggregate function is invoked using the `OVER` clause. The section [Using the aggregate function `avg()` to compute a moving average](./#using-the-aggregate-function-avg-to-compute-a-moving-average) shows an example.

### Window functions that return an "int" or "double precision" value as a "classifier" of the rank of the row within its window

The only information, in the input row set, that the functions in this group depend upon is the emergent order as determined by the window `ORDER BY` clause. The actual order depends in the usual way on what this clause says. With the one exception of `ntile()`, these functions have no formal parameters. The `ntile()` function has a single, mandatory `int` formal parameter. It specifies the number of subsets into which the input row set should be classified. This means that it reflects only the invoker's intention and does not reflect the shape of the input.

| Function | agg? | frame? | Description |
| ---- | ---- | ---- | ---- |
| [`row_number()`](./row-number-rank-dense-rank/#row-number) |  |  | Returns a unique integer for each row in a [_window_](../invocation-syntax-semantics/#the-window-definition-rule), from a dense series that starts with _1_, according to the emergent order that the window `ORDER BY` clause specifies. For the two or more rows in a tie group, the unique values are assigned randomly. |
| [`rank()`](./row-number-rank-dense-rank/#rank) | Y |   | Returns the integer ordinal rank of each row according to the emergent order that the window `ORDER BY` clause specifies. The series of values starts with 1 but, when the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) contains ties, the series is not dense. The "ordinal rank" notion is familiar from sporting events. If three runners reach the finish line at the same time, then they are all deemed to have tied for first place. The runner who finishes next after these is deemed to have come in in fourth place because three runners came in before this finisher. |
| [`dense_rank()`](./row-number-rank-dense-rank/#dense-rank) | Y |  | Returns the integer ordinal rank of the distinct value of each row according to what the window `ORDER BY` clause specifies. The series of values starts with _1_ and, even when the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) contains ties, the series is dense. The "dense rank" notion reflects the ordering of distinct values of the list of expressions that the window `ORDER BY` clause specifies. In the running race example, the three runners who tied for first place would get a dense rank of 1. And the runner who finished next after these would get a dense rank of 2, because this finisher got the second fastest distinct finish time. |
| [`percent_rank()`](./percent-rank-cume-dist-ntile/#percent-rank) | Y |  | Returns the percentile rank of each row within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule), with respect to the argument of the [`window_definition`](./#the-window-definition-rule)'s window `ORDER BY` clause, as a number in the range _0.0_ through _1.0_. The lowest value of `percent_rank()` within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) will always be _0.0_, even when there is a tie between the lowest-ranking rows. The highest possible value of `percent_rank()` within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) is _1.0_, but this value is seen only when the highest-ranking row has no ties. If the highest-ranking row does have ties, then the highest value of `percent_rank()` within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) will be correspondingly less than _1.0_ according to how many rows tie for the top rank. The notion is well-known from statistics. More details are given in this function's dedicated account. |
| [`cume_dist()`](./percent-rank-cume-dist-ntile/#cume-dist) | Y |  | Returns a value that represents the number of rows with values less than or equal to the current row's value divided by the total number of rows—in other words, the relative position of a value in a set of values. The graph of all values of `cume_dist()` within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) is known as the cumulative distribution of the argument of the [`window_definition`](./#the-window-definition-rule)'s window `ORDER BY` clause. The value _c_ returned by `cume_dist()` is a number in the range _0 < c <= 1_. You can use `cume_dist()` to answer questions like this: Show me the rows whose score is within the top "x%" of the [_window_](../invocation-syntax-semantics/#the-window-definition-rule)'s population, ranked by score. The notion is well-known from statistics. More details are given in this function's dedicated account. |
| [`ntile()`](./percent-rank-cume-dist-ntile/#ntile) |  |  | Returns an integer value for each row that maps it to a corresponding percentile. For example, if you wanted to mark the boundaries between the highest-ranking 20% of rows, the next-ranking 20% of rows, and so on, then you would use `ntile(5)`. The top 20% of rows would be marked with _1_, the next-to-top 20% of rows would be marked with _2_, and so on, so that the bottom 20% of rows would be marked with _5_. If the number of rows in the [_window_](../invocation-syntax-semantics/#the-window-definition-rule), _N_, is a multiple of the actual value with which you invoke `ntile()`, `n`, then each percentile set would have exactly _N/n_ rows. This is achieved, if there are ties right at the boundary between two percentile sets, by randomly assigning some to one set and some to the other. If _N_ is not a multiple of _n_, then `ntile()` assigns the rows to the percentile sets so that the numbers assigned to each are as close as possible to being the same. |

### Window functions that return columns of another row within the window

The functions in this group depend, in the same way as those in the first group do, upon the emergent order as determined by the window `ORDER BY` clause. Each has, at least, a single, mandatory, `anyelement` formal parameter that specifies which value to fetch from the designated other row in the [_window_](../invocation-syntax-semantics/#the-window-definition-rule). If you want to fetch values from more than one column, you must combine them into a scalar value. The most obvious way to do this is to list the columns in the constructor for a user-defined _"row"_ type. The `nth_value()` function has a mandatory second `int` formal parameter that specifies the value of _"N"_ to give _"Nth"_ its meaning. The other functions in this group have just one formal parameter.

| Function | agg? | frame? | Description |
| ---- | ---- | ---- | ---- |
| [`first_value()`](./first-value-nth-value-last-value/#first-value) |  | Y | Returns the specified value from the first row, in the specified sort order, in the current [_window frame_](../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions). If you specify the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) to start at a fixed offset before the current row, then `first_value()` would produce the same result as would the correspondingly parameterized `lag()`. If this is your aim, then you should use `lag()` for clarity. |
| [`nth_value()`](./first-value-nth-value-last-value/#nth-value) |  | Y | Returns the specified value from the "_Nth"_ row, in the specified sort order, in the current [_window frame_](../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions). The second, mandatory, parameter specifies _"N"_ in _"Nth"_. |
| [`last_value()`](./first-value-nth-value-last-value/#last-value) |  | Y | Returns the specified value from the last row, in the specified sort order, in the current [_window frame_](../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions). |
| [`lag()`](./lag-lead/#lag) |  |  | Returns, for the current row, the designated value from the row in the ordered input set that is _"lag_distance "_ rows before it. The data type of the _return value_ matches that of the _input value_. `NULL` is returned when the value of _"lag_distance"_ places the earlier row before the start of the [_window_](../invocation-syntax-semantics/#the-window-definition-rule). Use the optional last parameter to specify the value to be returned, instead of `NULL`, when the looked-up row falls outside of the current [_window_](../invocation-syntax-semantics/#the-window-definition-rule). |
| [`lead()`](./lag-lead/#lead) |  |  | Returns, for the current row, the designated value from the row in the ordered input set that is _"lead_distance"_ rows after it. The data type of the _return value_ matches that of the _input value_. `NULL` is returned when the value of _"lead_distance"_ places the later row after the start of the [_window_](../invocation-syntax-semantics/#the-window-definition-rule). Use the optional last parameter to specify the value to be returned, instead of `NULL`, when the looked-up row falls outside of the current [_window_](../invocation-syntax-semantics/#the-window-definition-rule). |
