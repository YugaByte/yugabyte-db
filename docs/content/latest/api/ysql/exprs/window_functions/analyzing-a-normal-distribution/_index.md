---
title: case study—compare percent_rank(), cume_dist(), and ntile() on a normal distribution
linkTitle: case study—analyzing a normal distribution
headerTitle: Case study—analyzing a normal distribution with percent_rank(), cume_dist(), and ntile()
description: Case study to compare and contrast the window functions percent_rank(), cume_dist(), and ntile() on large sets of normally distributed values.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: analyzing-a-normal-distribution
    parent: window-functions
    weight: 40
isTocNested: true
showAsideToc: true
---
**On this page**<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[Introduction](./#introduction)<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[How to make best use of the code examples](./#how-to-make-best-use-of-the-code-examples)<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[Step through the code](./#step-through-the-code)
<br>
&#160;&#160;&#160;&#160;&#160;&#160;&#160;[Conclusion](./#conclusion)

## Introduction

This section describes an empirical approach to answering the following question:

- If you want to allocate a row set into buckets where each contains the same number of rows, based on your ordering rule, is there any difference between the result produced by using, in turn, each of the three functions [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank), [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist), or [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile)?

The answer is, of course, "Yes"—why else would the three functions all be supported? The [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile) function is specified exactly to meet the stated goal. From the dedicated [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile) section:

> **Purpose:** Return an integer value for each row that maps it to a corresponding percentile. For example, if you wanted to mark the boundaries between the highest-ranking 20% of rows, the next-ranking 20% of rows, and so on, then you would use `ntile(5)`. The top 20% of rows would be marked with _1_, the next-to-top 20% of rows would be marked with _2_, and so on so that the bottom 20% of rows would be marked with _5_.

The other two functions implement more fine grained measures. Here's what the [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) documentation says:

> **Purpose:** Return the percentile rank of each row within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule), with respect to the argument of the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition)'s window `ORDER BY` clause. The value _p_ returned by `percent_rank()` is a number in the range _0 <= p <= 1_. It is calculated like this:
```
percentile_rank = (rank - 1) / ("no. of rows in window" - 1)
```
And here's what the [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) documentation says:

> **Purpose:** Return a value that represents the number of rows with values less than or equal to the current row’s value divided by the total number of rows—in other words, the relative position of a value in a set of values. The graph of all values of `cume_dist()` within the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) is known as the cumulative distribution of the argument of the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition)'s window `ORDER BY` clause. The value _c_ returned by `cume_dist()` is a number in the range _0 < c <= 1_. It is calculated like this:
```
cume_dist() =
  "no of rows with a value <= the current row's value" /
  "no. of rows in window"
```
The algorithm that the [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) uses is different from the one that  [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) uses. And the algorithm that `ntile()` uses to produce its bucket allocations directly is unspecified. 

However, the answer "Yes" to the question "is there any difference between the result produced by  each of the three functions?" is a qualified "Yes" because when certain conditions hold, there is no difference.

The pedagogic value of this empirical study is brought by the fact that it aims to meet the stated high-level goal rather than to demonstrate the basic use of low-level primitives. This means that it necessarily combines the basic use of the window functions under study with all sorts of other generic SQL techniques (and even stored procedure techniques) because these are needed to meet the goal. This kind of bigger-picture use typically goes hand-in-hand with any serious use of window functions.

Here is the problem statement at the next level of detail:

- A row set with _N_ rows has only unique values of the column list that the OVER clause specifies for ordering the rows in the [_window_](../invocation-syntax-semantics/#the-window-definition-rule). (In other words, there are no ties.)
- The aim is to allocate the rows into _n_ buckets that each have the same number of rows.
- _N_ is an integral multiple of _n_.

The study shows that if these special qualifications are met, then the bucket allocations are identical. It shows, too, that if either of those special qualifications isn't met (in other words, either if there are ties, or if _N_ is not an integral multiple of _n_), then the bucket allocations produced by each approach differ.

## How to make best use of the code examples
Before starting, create the data set that the code relies on using the `ysqlsh` script that [table t4](../function-syntax-semantics/data-sets/table-t4/) presents. It takes only a few seconds to run, and then you can simply leave _"t4"_ untouched as you run, and re-run, the code examples. Notice that the table is populated by values that are created by a generator that picks, pseudorandomly, from an ideal normal distribution. This means that the values in table _"t4"_ will be different each time that you drop and re-create it. The code examples will therefore always show the same patterns. And the invariants that it illustrates will hold reliably. But the details will change with each new re-creation of _"t4"_. Your experience will be best if, as you step through the code examples, you run them all against fixed content in in table _"t4"_.

The steps that are described in the next section should all be run at the `ysqlsh` prompt. They do a mixture of things:

-  They use `.sql` scripts to drop and re-create schema objects of these kinds:
  -  views through which to access the `double precision` column, and then the `int` column, in table _"t4"_ through a uniform interface, so that the same-spelled queries can run in turn against each of these two columns
  -  tables to hold results from different queries so that later queries can compare these
  -  views through which to populate the separate results tables for the `double precision` column, and then the `int` column, through a uniform interface, so that the same-spelled reports can run in turn against each of these two columns
  - PL/pgSQL procedures to populate the results tables and PL/pgSQL functions to encapsulate prepared queries.
- They use `.sql` scripts to run various queries, and execute various anonymous PL/pgSQL blocks (also known as `DO` blocks) that are too small to warrant encapsulation in functions and are best encapsulated in dedicated `.sql` scripts,
- And they invoke the functions and the procedures, and execute the `.sql` scripts, using small anonymous scripts that are simply presented in line in the account.

It's best to create a dedicated user for the purpose of running this code that owns no other objects.

Each of the `.sql` scripts is presented on a dedicated page. (You can see all of these in the order in which they are to be run in the navigation bar.) Each page starts with a sentence like this:

> Save this script as `<some name>.sql`.

Before starting, you should create a directory (it doesn't matter what you call it) on which to save all of these scripts. Be sure to save each successive file as instructed. The names are critical because the last script, [`do_demo.sql`](./do-demo/), invokes each of these in the proper order.

When you go through everything manually, you'll appreciate what each step does, and see the result (when it does more than create a schema object) immediately. That way, you'll be able to compare your results with the typical results that the following section presents and appreciate how the results vary in inessential ways each time that table _"t4"_ is dropped and re-created.

The directory you create to hold the saved scripts must have a subdirectory called `reports`. This is where the master script, [`do_demo.sql`](./do-demo/), spools its output. The set of scripts are written so that they can simply be run time and again without reporting any errors. This, together with the fact the master script spools all results, means that it will finish silently.

When you've got this far, you can save the spooled files to the side and then run the script to drop, re-create, and re-populate table _"t4"_. Then you can _diff_ the old results with the new results to see what changes and what stays the same. You could also change the number of rows with which _"t4"_ is populated so the it is _not_ an integral multiple of the number of buckets that you request for the allocations. (You can change this number too, of course.) Then you'll see how dramatically the results change.

## Step through the code

### Step ZERO

Drop and re-create the results tables using [`do_clean_start.sql`](./do-clean-start/).

### Step ONE

To confirm that the outcome is sensible, first create the table function `show_t4()` with [this script](./cr-show-t4/). It reports some useful overall measures of _"t4"_.  Then execute it like this:
```plpgsql
select t as "Some useful overall measures of t4."
from show_t4();
```
Because of the pseudorandom behavior, the actual values of the mean and standard deviation will change with each successive re-population of _"t4"_. These results are typical:
```
   Some useful overall measures of t4.    
------------------------------------------
 count(*)                          100000
 
 avg(%score)                         52.4
 stddev(%score)                      11.6
```
Now get a sense of how similar the values returned by [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) are if (and only if) the values that the window `ORDER BY` uses are unique in the [_window_](../invocation-syntax-semantics/#the-window-definition-rule).

- Use [`cr_dp_views.sql`](./cr-dp-views/) to create a view to present _"t4.dp_score"_ as _"score"_ .
- Use [`cr_pr_cd_equality_report.sql`](./cr-pr-cd-equality-report) to create the function `pr_cd_equality_report()` . The name means 'compare the extent to which _"pr"_ (the value produced by [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank)) is similar to _"cr"_ (the value produced by [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist))' for each of the _"score"_ values. The values are compared as a ratio (expressed as a percent). The reporting function is parameterized by the absolute difference, _"delta_threshold"_ that this ratio might have from 100%. And it reports the count, the maximum score, and the maximum ratio over the scores where _"delta_" is greater than _"delta_threshold"_.

Run the function with five different values for _"delta_threshold"_ like this:

```plpgsql
select * from pr_cd_equality_report(0.50);
select * from pr_cd_equality_report(0.10);
select * from pr_cd_equality_report(0.05);
select * from pr_cd_equality_report(0.01);
```
Here is a summary of the results:
```
 count(*) | max_score | max_ratio 
----------+-----------+-----------
      199 |   19.19   |   99.50
      990 |   25.53   |   99.90
     1960 |   28.55   |   99.95
     9090 |   36.99   |   99.99
```
The results show that:

- once you have passed the lowest _10,000_ or so values (that is about _10%_ of the total _100,000_ number of rows), the ratio of the two measures is never more than _0.01%_ away from _100%_.
- and once you have passed the lowest _200_ or so values (that is about _0.2%_ of the total _100,000_ number of rows), the ratio of the two measures is never more than _0.5%_ away from _100%_.

In other words, the two measures give remarkably similar answers over most of the high end of the range of values.

Now repeat the measurement for the values in the _"t4.int_score"_ column—where each possible value as about _1,000_ duplicates. Use [`cr_int_views.sql`](./cr-int-views/) to do this. And then repeat the five function executions. Now the results are very different:

```
 count(*) | max_score | max_ratio 
----------+-----------+-----------
    97717 |   75.00   |   99.46
    99541 |   82.00   |   99.86
    99782 |   85.00   |   99.95
    99972 |   91.00   |   99.99
```
This shouldn't be surprising. Statisticians who use these functions will know when their analysis needs one, or the other, of  [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank)) and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist)).

### Step TWO

Create a function to visualize the data as a histogram. This relies on allocating the values in the column _"t4.dp_score"_ into equal width buckets. The same allocation scheme will be later needed for the output values of [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist). This turns out to be a somewhat challenging problem. It is discussed in the section [The bucket allocation scheme](./bucket-allocation).

Now that you've understood the challenge, create the `bucket()` function using either [`cr_bucket_using_width_bucket.sql`](./cr-bucket-using-width-bucket/) or [`cr_bucket_dedicated_code.sql`](./cr-bucket-dedicated-code/)—it doesn't matter which because they behave identically.

Test that the behavior is correct with [`do_assert_bucket_ok`](./do-assert-bucket-ok/).

Next, create the `language sql` function that creates the histogram output, [cr_histogram.sql](./cr-histogram/). Notice that this is functionally equivalent to this

```
prepare do_histogram(
  int,     -- No. of buckets
  numeric  -- Scale factor for histogram height
) as
select ... ;
```
but it simply uses a different header
```
create or replace function do_histogram(
  no_of_bukets in int,
  scale_factor in numeric)
  returns SETOF text
  language sql
as $body$
```
to encapsulate the identical SQL text. But it has the advantage that it can be done once when the application's database artifacts are installed rather than at the start of every session by each session that needs it. It also as the additional benefit that you can use  named formal parameters to make the SQL more readable.

Now generate the histogram like this:
```plpgsql
select * from histogram(50, 100);
```
You can see typical results here: [Output from running `histogram()` on _"t4.dp_score"_](./reports/histogram-report/).

### Step THREE

- Compare the bucket allocation produced by [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile), [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank), and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) acting on the `double precision` column _"dp_score"_.

Create the following three functions:

- `do_ntile()` using [`cr_do_ntile.sql`](./cr-do-ntile/)
- `do_do_percent_rank()` using [`cr_do_percent_rank.sql`](./cr-do-percent-rank/)
- `do_cume_dist()` using [`cr_do_cume_dist.sql`](./cr-do-cume-dist/).

Each computes the bucket allocation by accessing the data in table _"t4"_ through the view _"t4_view"_.

First run [`cr_dp_views.sql`](./cr-dp-views/). This creates the view _"t4_view"_ to select _"t4.dp_score"_ as _"score"_ and the view _"results"_ over the table _"dp_results"_.

Each of the window functions under test will now do this:
- Compute the bucket allocation for each _"t4.dp_score"_ value.
- Group these by the bucket number and compute the count, the minimum score, and the maximum score for each group.
- Insert this summary information into the table _"dp_results"_, annotating each row with the name of the window function that was used.

Do this using [`do_populate_results.sql`](./do-populate-results/).

Now report the results using [`do_report_results.sql`](./do-report-results/).

You can see typical results here: [Output from running `do_ntile()`, `do_percent_rank()`, and `do_cume_dist()` on _"t4.dp_score"_](./reports/dp-results/).

You can see at a glance that the bucket allocations produced by each method seem to be the same. It's certainly clear that the population of each bucket is, as requested, `100,000/20 = 5,000` for each bucket and for each method. However, it's too hard to check that the minimum and maximum score values for each of the _60_ buckets is the same. Use the script [`do_compare_dp_results.sql`](./do-compare-dp-results/) to check this.

You can see the results here: [Output from running `do_compare_dp_results.sql` on _"dp_results_"](./reports/compare-dp-results/). These results are reproducible, no matter what the pseudorandomly generated values in table _"t4"_ are.

The property of the three functions under test that this study aimed to investigate has been seen to hold. You can illustrate this by repeating the whole test by regenerating the source data using the script that [table t4](../function-syntax-semantics/data-sets/table-t4/) presents. When you do this, you can change the number of rows that are generated. And you can change the number of buckets that you request by editing [`do_populate_results.sql`](./do-populate-results/). Just be sure that the number of rows in table _"t4"_ is an exact multiple of the number of buckets that you request.

Then simply run the master script, [`do_demo.sql`](./do-demo/) and inspect the spooled output.

Of course, this isn't a _proof_ that the test will always have this outcome. But it is sufficient to show that the three functions do, at least, produced very similar results for this particular, and rather special use case.

### Step FOUR

Finally, repeat the test when one of the special conditions of the particular use case doesn't hold—when the values that the three functions operate on have duplicates:

- Compare the bucket allocation produced by [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile), [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank), and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) acting on the `int` column _"int_score"_.

Simply run [`cr_int_views.sql`](./cr-int-views/) to create the view _"t4_view"_ to select _"t4.int_score"_ as _"score"_ and the view _"results"_ over the table _"int_results"_. Now re-run the remaining steps as above:

- [`do_populate_results.sql`](./do-populate-results/)
- [`do_report_results.sql`](./do-report-results/)

You can see typical results here: [Output from running `do_ntile()`, `do_percent_rank()`, and `do_cume_dist()` on _"t4.int_score"_](./reports/int-results/).

It's immediately obvious that the three functions under test produce dramatically different results on this, more general, kind of input.

You can illustrate that the three functions under test produce different output when the special conditions do not hold by, for example, repeating the whole test and after setting the number of buckets that you request, by editing [`do_populate_results.sql`](./do-populate-results/), so that the number of rows in table _"t4"_ is _not_ an exact multiple of the number of buckets.

Then simply run the master script, [`do_demo.sql`](./do-demo/) and inspect the spooled output. Notice now you see different results from the three functions even when they operate on _"t4.dp_score"_—which as only unique values.

## Conclusion

This study has shown that if, and only if, these special conditions hold:

- a row set with _N_ rows has only unique values of the column list that the OVER clause specifies for ordering the rows in the [_window_](../invocation-syntax-semantics/#the-window-definition-rule)—In other words, there are no ties
- the aim is to allocate the rows into _n_ buckets that each have the same number of rows
- _N_ is an integral multiple of _n_

then the three functions under test, [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile), [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank), and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist), all produce the same bucket allocation.

It showed, too, that if not all of these conditions hold, then each of the three functions produces a different bucket allocation.

The moral of the tale is clear:

- If your aim is to allocate rows into, as close as is possible, equiheight buckets, then you should, of course, use the window function that is specified to meet this goal: [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile).
- If you have more subtle requirements that cannot be met by [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile), then you are probably well-versed in statistics and will understand in advance the different goals that [`percent_rank()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) and [`cume_dist()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) are specified to meet and will simply know, _a priori_ which one of these two to choose.

The study has also brought, as promised, several generic pedagogic benefits. And it did this precisely because it set out to meet a goal that was stated functionally in high-level terms. In other words, it did _not_ set out to meet any specific pedagogic goals. Rather, the techniques that were used emerged as a consequence of meeting the high-level goal.

Here's a list of some of these generic pedagogic benefits.

### Use of the normal_rand() function.

This function is useful in all sorts of generic testing scenarios. And it's often much better to use this than to use the ordinary `generate_series()` built-in.

### Using gen_random_uuid() to populate a surrogate primary key column

You saw that this idiom:
```
k uuid default gen_random_uuid() primary key
```
allows bulk insert into a table to go very much faster than this more familiar idiom:
```
k serial primary key
```
The reason for the speed difference is that `gen_random_uuid()` reliably produces unique values entirely programmatically while `serial` implies the use of a sequence—and such use brings the expense of inter-node coordinating round trips, in a multi-node distributed SQL database deployment, to guarantee that the values that are delivered are unique.

### The use of a view, for both SELECT and INSERT, to point to different tables

Notice these two scripts:  [`cr_dp_views.sql`](./cr-dp-views/) and [`cr_int_views.sql`](./cr-int-views/). They allow:

- _both_—the code that runs the window functions to be defined just once and to be switched to read from either the column with duplicate values or the column with unique values;
- _and_—the code that inserts into the results tables to be defined just once and to be switched correspondingly to write the results to the table that's appropriate for the present test.

This technique is often used in test scenarios where the same test is run under two, or more, conditions. The technique is _not recommended_ for production use because of the various well-know problems brought by doing DDL in such an environment.

### Using the WITH clause

The `WITH` clause brings the obvious benefit that you can name intermediate result sets so that your SQL become very much more readable, and therefore very much more likely to implement what you intend, than without such naming,

In particular, it is a far more self-evident way to implement self-joins than was possible, ages ago, before the advent of this device. (This technique was used in the script [`do_compare_dp_results.sql`](./do-compare-dp-results/).)

### Using the ordinary GROUP BY clause in conjunction with window functions

Very often, a window function is used in a `WITH` clause to specify intermediate results that then are reduced in volume in subsequent `WITH` clauses to meet the ultimate purpose of the SQL statement.

### The use of "language plpgsql" procedures

See, for example, the script that [table t4](../function-syntax-semantics/data-sets/table-t4/) presents. The procedure encapsulates several steps that each builds on the next in a way that you cannot manage to do easily (or at all) using a single SQL statement. In particular, this procedure populates an array with SQL, processes its content using SQL on the array rather than on a schema-level table, and then uses SQL again to insert the array's content directly into the target table. In other words, it uses SQL, in the implementation of procedural code, to bring the famous benefits of set-based processing, rather than iterative programming in a procedural loop. This is a very powerful technique.

### The use of "language sql" functions and procedures as an alternative to PREPARE

This brings several benefits over `PREPARE`: you can use named formal parameters to improve readability; you shift the responsibility from application code start-up time, for every new session, to a one-time database installation when you install all of the artifacts that implement that application's database back end; and you have a single point of maintenance for the SQL that the stored function or procedure encapsulates.

### The contrast between ntile() and width_bucket()

You saw that `ntile()` produces an equiheight histogram while `width_bucket()` produces an equiwidth histogram. The former is a window function; but the latter is a regular function. This reflects the fact that the latter is rather older in the history of databases than the former: it requires you to calculate the lower and upper bounds of the [_window_](../invocation-syntax-semantics/#the-window-definition-rule) yourself, and supply these as actual arguments. You might use the window functions [`first_value`](../function-syntax-semantics/first-value-nth-value-last-value/#first-value) and [`last_value`](../function-syntax-semantics/first-value-nth-value-last-value/#last-value) for this. And you might decide to implement `my_width_bucket` as a window function. YSQL has an extensibility framework (beyond the scope of this section) that would allow this.

It is to be hoped, therefore, that you might dip into this optional section periodically to seek inspiration for techniques that you might use when you have a new problem to solve.
