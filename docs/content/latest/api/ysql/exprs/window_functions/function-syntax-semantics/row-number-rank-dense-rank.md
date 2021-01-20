---
title: row_number(), rank() and dense_rank()
linkTitle: row_number(), rank() and dense_rank()
headerTitle: row_number(), rank() and dense_rank()
description: Describes the functionality of the YSQL window functions row_number(), rank() and dense_rank().
menu:
  latest:
    identifier: row-number-rank-dense-rank
    parent: window-function-syntax-semantics
    weight: 10
isTocNested: true
showAsideToc: true
---
These three window functions bear a strong family resemblance to each other. If the values that the expression that the window `ORDER BY` clause specifies are unique within a [_window_](../../invocation-syntax-semantics/#the-window-definition-rule), then all three functions return the same value for each row. They return different values only when the ordering results in ties. The fact that this family has three members reflects the three possible ways to handle ties.

## row_number()

**Signature:**
```
input value:       <no formal parameter>
return value:      bigint
```
**Purpose:** Return a unique integer for each row in a [_window_](../../invocation-syntax-semantics/#the-window-definition-rule), from a dense series that starts with _1_, according to the emergent order that the window `ORDER BY` clause specifies. For the two or more rows in a tie group, the unique values are assigned randomly.

## rank()

**Signature:**
```
input value:       <no formal parameter>
return value:      bigint
```
**Purpose:** Return the integer ordinal rank of each row according to the emergent order that the window `ORDER BY` clause specifies. The series of values starts with _1_ but, when the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) contains ties, the series is not dense.

The "ordinal rank" notion is familiar from sporting events. If three runners reach the finish line at the same time, within the limits of timing accuracy, then they are all deemed to have tied for first place. The runner who finishes next after these is deemed to have come in in fourth place because three runners came in before this finisher.

## dense_rank()

**Signature:**
```
input value:       <no formal parameter>
return value:      bigint
```
**Purpose:** Return the integer ordinal rank of the distinct value of each row according to what the window `ORDER BY` clause specifies. The series of values starts with _1_ and, even when the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) contains ties, the series is dense.

The "dense rank" notion reflects the ordering of _distinct values_ of the list of expressions that the window `ORDER BY` clause specifies. In the running race example, the three runners who tied for first place would get a dense rank of _1_. And the runner who finished next after these would get a dense rank of _2_, because this finisher got the second fastest distinct finish time.

## Example

{{< note title=" " >}}
If you haven't yet installed the tables that the code examples use, then go to the section [The data sets used by the code examples](../data-sets/).
{{< /note >}}

This example highlights the semantic difference between `row_number()`, `rank()`, and `dense_rank()`. Create a data set using the `ysqlsh` script that [table t2](../data-sets/table-t2/) presents. Then do this:
```plpgsql
select
  class,
  k,
  score,
  (row_number() over w),
  (rank()       over w),
  (dense_rank() over w)
from t2
window w as (partition by class order by score)
order by 1, 2;
```
Here is a typical result. The exact ordering or the `row_number()` values within tie groups changes (exactly according to how the table is populated for maximum pedagogic effect) each time you re-create table _"t2"_. To make it easier to see the pattern, several blank lines have been manually inserted here between each successive set of rows with the same value for _"class"_. And in the second set, which has ties, one blank line has been inserted between each tie group.
```
 class | k  | score | row_number | rank | dense_rank 
-------+----+-------+------------+------+------------
     1 |  1 |     1 |          1 |    1 |          1
     1 |  2 |     2 |          2 |    2 |          2
     1 |  3 |     3 |          3 |    3 |          3
     1 |  4 |     4 |          4 |    4 |          4
     1 |  5 |     5 |          5 |    5 |          5
     1 |  6 |     6 |          6 |    6 |          6
     1 |  7 |     7 |          7 |    7 |          7
     1 |  8 |     8 |          8 |    8 |          8
     1 |  9 |     9 |          9 |    9 |          9
     
     
     
     2 | 10 |     2 |          2 |    1 |          1
     2 | 11 |     2 |          3 |    1 |          1
     2 | 12 |     2 |          1 |    1 |          1
     
     2 | 13 |     4 |          4 |    4 |          2
     
     2 | 14 |     5 |          5 |    5 |          3
     
     2 | 15 |     6 |          6 |    6 |          4
     
     2 | 16 |     7 |          7 |    7 |          5
     2 | 17 |     7 |          8 |    7 |          5
     
     2 | 18 |     9 |          9 |    9 |          6
```

Notice that, as promised:

- In the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) with _"k=1"_, the result for each row, for each of `row_number()`, `rank()`, and `dense_rank()`, is the same.
- In the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) with _"k=2"_, the results for each of these functions don't all have the same value as each other.
- The values that `row_number()` returns within tie groups don't follow the ordering followed by the values of column _"k"_.