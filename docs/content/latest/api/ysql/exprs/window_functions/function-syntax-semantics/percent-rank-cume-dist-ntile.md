---
title: percent_rank(), cume_dist() and ntile()
linkTitle: percent_rank(), cume_dist() and ntile()
headerTitle: percent_rank(), cume_dist() and ntile()
description: Describes the functionality of the YSQL window functions percent_rank(), cume_dist() and ntile().
menu:
  latest:
    identifier: percent-rank-cume-dist-ntile
    parent: window-function-syntax-semantics
    weight: 20
isTocNested: true
showAsideToc: true
---

These three window functions bear a strong family resemblance to each other. The explanation of what they achieve rests on the notion of a _centile_. A centile is a measure used in statistics to denote the value below which a given fraction of the values in a set falls. The term  _percentile_ is often preferred by those who like to express fractions as percentages. For example, the 70th percentile is the value below which 70% of the values fall.

Briefly:

- `percent_rank()` assigns the centile fraction to each value in the input population
- `cume_dist()` function returns, for a specified value within a population, the number of values that are less than or equal to the specified value divided by the total number of values—in other words, the relative position of a value within the population
- `ntile` assigns the bucket number, in an equiheight histogram (see below) to each value in the population by specifying the required number of buckets as the function's actual argument.

Suppose that you want to divide a set of values into N subsets, where each subset contains the same number of values. The result is referred to as an _equiheight_ histogram. This implies unequal value-axis bucket widths. Compare this with the `width_bucket()` regular built-in function that produces an _equiwidth_ histogram—that is it marks off the value axis into equal width buckets that, in general each contains a different number of values.

If the required number of unequal width buckets for the equal height histogram is _5_, then your goal is equivalently stated by saying that you want to split the population at the values that fall at the 20th, 40th, 60th, and 80th percentiles. In this use case, and if and only if the values are unique and the population size is an exact multiple of the required number of buckets, then any one of `percent_rank()`, `cume_dist()` or `ntile()` can be used to meet the goal. But using `ntile()` needs noticeably simpler SQL than using the other two because it's tailored for this, but only this, purpose. Moreover, if the set contains duplicate values, then _only_ `ntile()` meets the goal (to the extent that this is feasible) because it randomly assigns ties at the boundaries between the buckets so that some go to the higher-value set and some go to the lower-value set. The dedicated section [Analyzing a normal distribution with percent_rank(), cume_dist() and ntile()](../../analyzing-a-normal-distribution/) demonstrates this with two large populations of approximately normally distributed values—one with no duplicates and one with lots of duplicates. And it shows that if the population has ties, or when the population size is _not_ an exact multiple of the required number of buckets, then `percent_rank()` and `cume_dist()` produce different results from each other, and each divides the population differently than `ntile()`, because they handle ties differently and because of other subtle differences.

If you want to split the population into _N_ subsets of unequal population sizes, then you must use one of `percent_rank()` or `cume_dist()`.

This documentation assumes that you already understand the subtle difference between the _percentile_ notion and the _cumulative distribution_ notion, and know how to define your goal with respect to splitting populations of values into subsets—and that you will therefore know how to choose which of the functions `percent_rank()`, `cume_dist()` or `ntile()` that you should use.

## percent_rank()

**Signature:**

```
input value:       <no formal parameter>
return value:      double precision
```
**Purpose:** Return the percentile rank of each row within the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule), with respect to the argument of the [`window_definition`](../../../../syntax_resources/grammar_diagrams/#window-definition)'s window `ORDER BY` clause. The value _p_ returned by `percent_rank()` is a number in the range _0 <= p <= 1_. It is calculated like this:

```
percentile_rank = (rank - 1) / ("no. of rows in window" - 1)
```
Arguably, then, this function is wrongly named because to deserve its name it would return a value in the range _0_ through _100_.

The lowest value of `percent_rank()` within the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) will always be _0.0_, even when there is a tie between the lowest-ranking rows. The highest possible value of `percent_rank()` within the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) is _1.0_, but this value is seen only when the highest-ranking row has no ties. If the highest-ranking row does have ties, then the highest vale of `percent_rank()` within the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) will be correspondingly less than _1.0_ according to how many rows ties for the top rank.

For example, the query computes the percentile rank for a table _"t"_ with primary key _"k"_ and _"score"_ column, over all the rows:
```
select
  k,
  score,
  100*(percent_rank() over (order by score)) as "percent_rank()"
from t;
```
When `percent_rank()` for a particular row has a value of, say,`0.42`, it means this score is higher than 42% of the other rows.

## cume_dist()

**Signature:**

```
input value:       <no formal parameter>
return value:      double precision
```
**Purpose:** Return a value that represents the number of rows with values less than or equal to the current row’s value divided by the total number of rows—in other words, the relative position of a value in a set of values. The graph of all values of `cume_dist()` within the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) is known as the cumulative distribution of the argument of the [`window_definition`](../../../../syntax_resources/grammar_diagrams/#window-definition)'s window `ORDER BY` clause. The value _c_ returned by `cume_dist()` is a number in the range _0 <= c <= 1_. It is calculated like this:

```
cume_dist() =
  "no of rows with a value <= the current row's value" /
  "no. of rows in window"
```

**Note:** the formula suggests that The return value _c_ will lie in the open-closed range _0 < c <= 1_. But the results shown in the study described in the section [Analyzing a normal distribution with `percent_rank()`, `cume_dist()` and `ntile()`](../../analyzing-a-normal-distribution/) show that the range is indeed the closed-closed _0 <= c <= 1_. This is doubtless due to rounding errors in the function's implementation.

You can use `cume_dist()` to answer questions like this:

- Show me the rows whose score is within the top _x%_ of the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule)'s population, ranked by score.

  ```
  prepare top_few_percent(double precision) as
  with v as (
    select
      score,
      100*cume_dist() over (order by dp_score) as cd,
      k
    from t)
  select
    score,
    to_char(cd, '999.9') as "cume_dist()",
    k
  from v
  where cd > $1
  order by cd desc, k;
  ```

## ntile()

**Signature:**

```
input value:       no_of_buckets int
return value:      int
```
**Purpose:** Return an integer value for each row that maps it to a corresponding percentile. For example, if you wanted to mark the boundaries between the highest-ranking 20% of rows, the next-ranking 20% of rows, and so on, then you would use `ntile(5)`. The top 20% of rows would be marked with _1_, the next-to-top 20% of rows would be marked with _2_, and so on so that the bottom 20% of rows would be marked with _5_.

**Note:** If the number of rows in the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule), _N_, is a multiple of the actual value with which you invoke `ntile()`, `n`, then each percentile set would have exactly _N/n_ rows. This is achieved, if there are ties right at the boundary between two percentile sets, by randomly assigning some to one set and some to the other. If _N_ is not a multiple of _n_, then `ntile()` assigns the rows to the percentile sets so that the numbers assigned to each are as close as possible to being the same.


## Comparing the effect of percent_rank(), cume_dist(), and ntile() on the same input

{{< note title=" " >}}
If you haven't yet installed the tables that the code examples use, then go to the section [The data sets used by the code examples](../data-sets/).
{{< /note >}}

The query that this section presents shows that the results produced by `percent_rank()` and `cume_dist()` are consistent with the formulas for these values that are given in the accounts, above, of these two functions. 

Create a data set using the `ysqlsh` script that [table t2](../data-sets/table-t2/) presents. This has been designed:

- so that the scores for the rows with _"class=1"_ are unique so that the ordering produces no ties
- and so that the scores for the rows with _"class=2"_ have duplicate values so that the ordering _does_ produce ties. The first tie group has three rows; and the second has two rows.

Now do this:

```plpgsql
with
  v1 as (
    select
      class,
      k,
      score,
      (rank()         over w1) as r,
      (percent_rank() over w1) as pr,
      (cume_dist()    over w1) as cd,
      (ntile(4)       over w1) as nt,
      (count(*)       over w2) as n_tot,
      (count(*)       over w3) as n_thru_curr
    from t2
    window
      w1 as (partition by class order by score),
      w2 as (w1 range between unbounded preceding and unbounded following),
      w3 as (w1 range between unbounded preceding and current row)),
  v2 as (
    select
      class,
      k,
      score,
      n_tot,
      n_thru_curr,
      r,
      (r::numeric - 1.0)/(n_tot::numeric - 1.0) as pr_calc,
      (n_thru_curr::numeric)/(n_tot::numeric)   as cd_calc,
      pr,
      cd,
      nt
    from v1)
select
  class,
  k,
  score,
  n_tot,
  n_thru_curr,
  (pr = pr_calc)::text     as "pr check",
  (cd = cd_calc)::text     as "cd check",
  r                        as "rank()",
  to_char(pr*100, '990.9') as "percent_rank()",
  to_char(cd*100, '990.9') as "cume_dist",
  nt                       as "ntile()"
from v2
order by class, "rank()", k;
```
This is the result. To make it easier to see the pattern, several blank lines have been manually inserted here between each successive set of rows with the same value for _"class"_. And in the second set, which has ties, one blank line has been inserted between each tie group.
```
 class | k  | score | n_tot | n_thru_curr | pr check | cd check | rank() | percent_rank() | cume_dist | ntile() 
-------+----+-------+-------+-------------+----------+----------+--------+----------------+-----------+---------
     1 |  1 |     1 |     9 |           1 | true     | true     |      1 |    0.0         |   11.1    |       1
     1 |  2 |     2 |     9 |           2 | true     | true     |      2 |   12.5         |   22.2    |       1
     1 |  3 |     3 |     9 |           3 | true     | true     |      3 |   25.0         |   33.3    |       1
     1 |  4 |     4 |     9 |           4 | true     | true     |      4 |   37.5         |   44.4    |       2
     1 |  5 |     5 |     9 |           5 | true     | true     |      5 |   50.0         |   55.6    |       2
     1 |  6 |     6 |     9 |           6 | true     | true     |      6 |   62.5         |   66.7    |       3
     1 |  7 |     7 |     9 |           7 | true     | true     |      7 |   75.0         |   77.8    |       3
     1 |  8 |     8 |     9 |           8 | true     | true     |      8 |   87.5         |   88.9    |       4
     1 |  9 |     9 |     9 |           9 | true     | true     |      9 |  100.0         |  100.0    |       4

     2 | 10 |     2 |     9 |           3 | true     | true     |      1 |    0.0         |   33.3    |       1
     2 | 11 |     2 |     9 |           3 | true     | true     |      1 |    0.0         |   33.3    |       1
     2 | 12 |     2 |     9 |           3 | true     | true     |      1 |    0.0         |   33.3    |       1

     2 | 13 |     4 |     9 |           4 | true     | true     |      4 |   37.5         |   44.4    |       2

     2 | 14 |     5 |     9 |           5 | true     | true     |      5 |   50.0         |   55.6    |       2

     2 | 15 |     6 |     9 |           6 | true     | true     |      6 |   62.5         |   66.7    |       3

     2 | 16 |     7 |     9 |           8 | true     | true     |      7 |   75.0         |   88.9    |       4
     2 | 17 |     7 |     9 |           8 | true     | true     |      7 |   75.0         |   88.9    |       3

     2 | 18 |     9 |     9 |           9 | true     | true     |      9 |  100.0         |  100.0    |       4
```
Notice that in this example, the number of rows per [_window_](../../invocation-syntax-semantics/#the-window-definition-rule), _9_, is not a multiple of the actual value, _4_ with which the function is invoked. This means that the number of rows assigned to each bucket can't be the same. Here, as promised, `ntile()` makes a best effort to get the numbers as close to each other as is possible. You can confirm, visually, that the populations are _three_ for one of the four buckets and _two_ for the other three.

Notice, too, what the outcomes are for the tie groups. Each of `percent_rank()` and `cume_dist()` produces a different value for each row where _"class=1"_, which has no ties. For _"class=2"_, when there are ties, these functions each produce the same value for all the rows in each of the two tie groups. In contrast, `ntile()` assigns the two rows in the second tie group (with _"score=7"_) to different percentile groups.

