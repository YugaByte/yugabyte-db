---
title: Bucket allocation scheme
linkTitle: Bucket allocation scheme
headerTitle: The bucket allocation scheme
description: The bucket allocation scheme. Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: bucket-allocation
    parent: analyzing-a-normal-distribution
    weight: 10
isTocNested: true
showAsideToc: true
---

It might seem that the built-in function `width_bucket()` ([here](https://www.postgresql.org/docs/11/functions-math.html#FUNCTIONS-MATH-FUNC-TABLE) in the PostgreSQL documentation) is tailor-made for this task. But there's a snag.

The values in the column _t4.dp_score"_ lie in this so-called closed-closed interval `[0, 100]`—expressed like this in SQL:
```
dp_score is between 0 and 100
```
**Note:** [This Wikipedia article](https://en.wikipedia.org/wiki/Interval_(mathematics)) explains the terminology used to characterize intervals. For example:

> The open-closed interval (0,1] means greater than 0 and less than or equal to 1. And the closed-open interval [0,1) means greater than or equal to 0 and less than 1.

The values returned both by [`percent_rank()`](../../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) and by [`cume_dist()`](../../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist), when scaled so that they are percentages, also lie in this same range.

Here is the signature (using sensibly chosen names for the formal parameters) of the `double precision` overload of `width_bucket()`:
```
function width_bucket(
  val    in double precision,  -- the value to be allocated
  lb     in double precision,  -- the lower bound of the overall closed-open interval
  ub     in double precision,  -- the upper bound of the overall closed-open interval
  n_vals in int)               -- the required number of buckets
  returns int
```

If you invoke `width_bucket()` to allocate a value `$1` with `lb = 0`, `ub = 100`, and `n_vals = 10`,  like this:

```
width_bucket($1, 0, 100, 10)
```
then the buckets are defined as these _closed-open_ intervals:
```
[ 0,  10)  ->  bucket  1
[10,  20)  ->  bucket  2
[20,  30)  ->  bucket  3
...
[80,  90)  ->  bucket  9
[90, 100)  ->  bucket 10
```
Any value that is less than _0_ is allocated to the lower overflow bucket _0_. And any value that is greater than or equal to the value _100_ is allocated to the upper overflow bucket _11_.
Here is an example:

```plpgsql
do $body$
declare
  lb     constant double precision := 0;
  ub     constant double precision := 100;
  n_vals constant int              := 10;
begin
  assert width_bucket( -0.0000000001, lb, ub, n_vals) =  0, 'unexpected';
  assert width_bucket(  0,            lb, ub, n_vals) =  1, 'unexpected';
  assert width_bucket( 99.9999999999, lb, ub, n_vals) = 10, 'unexpected';
  assert width_bucket(100,            lb, ub, n_vals) = 11, 'unexpected';
end;
$body$;
```

This means that attention has to be paid to the special case when the value to be allocated is _100_ in order that it is allocated to bucket _10_, and _not_ to the upper overflow bucket _11_. However, this is not the main snag. Rather, it's this:

- Pen-and-paper analysis, and some empirical testing, show that [`ntile()`](../../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile), [`percent_rank()`](../../function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank), and [`cume_dist()`](../../function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) can produce the same results only when these _open-closed_ intervals are used:

```
( 0,  10]  ->  bucket  1
(10,  20]  ->  bucket  2
(20,  30]  ->  bucket  3
...
(80,  90]  ->  bucket  9
(90, 100]  ->  bucket 10
```
This means that attention has to be paid to the special case at the other end of the _closed-closed_ interval of possible input values: when the value to be allocated is _0_, it must be allocated to bucket _1_, and _not_ to the lower overflow bucket _0_.

The consequence is that `width_bucket()` cannot be used _as is_. This leaves two choices. Each relies on implementing a new function `bucket()` with the same signature as `width_bucket()` but with a different implementation.

- The first choice is to implement `bucket` as a wrapper around `width_bucket()` to change its behavior at the bucket boundaries, See [cr_bucket_using_width_bucket.sql](../cr-bucket-using-width-bucket/).

- The second choice is to implement `bucket` from scratch. See [`cr_bucket_dedicated_code.sql`](../cr-bucket-dedicated-code/).

See [do_assert_bucket_ok.sql](../do-assert-bucket-ok/) for the acceptance test that each must pass.
