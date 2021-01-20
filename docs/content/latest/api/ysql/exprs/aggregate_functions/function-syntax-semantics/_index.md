---
title: YSQL aggregate functions signature and purpose
linkTitle: Per function signature and purpose
headerTitle: Signature and purpose of each aggregate function
description: This section summarizes the signature and purpose of each of the YSQL aggregate functions and links to their individual accounts.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: aggregate-function-syntax-semantics
    parent: aggregate-functions
    weight: 90
aliases:
  - /latest/api/ysql/exprs/aggregate_functions
isTocNested: true
showAsideToc: true
---

The aggregate functions are categorized into four classes:

- [general-purpose aggregate functions](#general-purpose-aggregate-functions)

- [statistical aggregate functions](#statistical-aggregate-functions)

- [_within-group ordered-set_ aggregate functions](#within-group-ordered-set-aggregate-functions) 

- [_within-group hypothetical-set_ aggregate functions](#within-group-hypothetical-set-aggregate-functions)

## General-purpose aggregate functions

The aggregate functions in this class can be invoked in one of two ways:

- _Either_ "ordinarily" on all the rows in a table or in connection with `GROUP BY`, when they return a single value for the set of rows.

  In this use, row ordering often doesn't matter. For example, [`avg()`](./avg-count-max-min-sum/#avg) has this property. Sometimes, row ordering _does_ matter. For example, the order of grouped values determines the mapping between array index and array value with [`array_agg()`](./array-string-jsonb-jsonb-object-agg/#array-agg).

- _Or_ as a [window function](../../window_functions/)  with `OVER`.

  In this use, where the aggregate function is evaluated for each row in the [window](../../window_functions/invocation-syntax-semantics/#the-window-definition-rule), ordering always matters.

Arguably, `avg()` might be better classified as a statistical aggregate function. (It is often used together with `stddev()`.) But, because it is so very commonly used, and used as a canonical example of the _"aggregate function"_ notion, it is classified here as a general-purpose aggregate function.

| Function | Description |
| ---- | ---- |
| [`array_agg()`](./array-string-jsonb-jsonb-object-agg/#array-agg) | Returns an array whose elements are the individual values that are aggregated. This is described in full detail the [`array_agg()`](../../../datatypes/type_array/functions-operators/array-agg-unnest/#array-agg) subsection in the main [Array](../../../datatypes/type_array/) section. |
| [`avg()`](./avg-count-max-min-sum/#avg) | Computes the arithmetic mean of a set of summable values by adding them all together and dividing by the number of values. If the set contains nulls, then these are simply ignored—both when computing the sum and when counting the number of values. |
| [`bit_and()`](./bit-and-or-bool-and-or/#bit-and) | Returns a value that represents the outcome of the applying the two-by-two matrix `AND` rule to each alligned set of bits for the set of `NOT NULL` input values. |
| [`bit_or()`](./bit-and-or-bool-and-or#bit-or) | Returns a value that represents the outcome of the applying the two-by-two matrix `OR` rule to each alligned set of bits for the set of `NOT NULL` input values. |
| [`bool_and()`](./bit-and-or-bool-and-or/#bool-and) | Returns a value that represents the outcome of the applying the two-by-two matrix `AND` rule to to the set of `NOT NULL` input boolean values. |
| [`bool_or()`](./bit-and-or-bool-and-or/#bool-or) | Returns a value that represents the outcome of the applying the two-by-two matrix `OR` rule to to the set of `NOT NULL` input boolean values. |
| [`count()`](./avg-count-max-min-sum/#count) | Counts the number of non null values in a set. The data type of the values is of no consequence. |
| `every()` | `every()` is a synonym for [`bool_and()`](./bit-and-or-bool-and-or/#bool-and) |
| [`jsonb_agg()`](./array-string-jsonb-jsonb-object-agg/#jsonb-agg) | This, and `json_agg()` are described in detail the [`jsonb_agg()`](../../../datatypes/type_json/functions-operators/jsonb-agg/) section in the main [JSON](../../../datatypes/type_json/) section. |
| [`jsonb_object_agg()`](./array-string-jsonb-jsonb-object-agg/#jsonb-object-agg) | This and `json_object_agg()` are described in detail the [`jsonb_object_agg()`](../../../datatypes/type_json/functions-operators/jsonb-object-agg/) section in the main [JSON](../../../datatypes/type_json/) section. |
| [`max()`](./avg-count-max-min-sum/#max-min) | Computes the greatest value among the values in the set using the rule that is used for the particular data type in the ORDER BY clause. nulls are removed before sorting the values. |
| [`min()`](./avg-count-max-min-sum/#max-min) | Computes the least value among the values in the set using the rule that is used for the particular data type in the ORDER BY clause. nulls are removed before sorting the values. |
| [`string_agg()`](./array-string-jsonb-jsonb-object-agg/#string-agg) | Returns a single value produced by concatenating the aggregated values (first argument) separated by a mandatory separator (second argument). The first overload has `text` inputs and returns `text`. The second overload has `bytea` inputs and returns `bytea`. |
| [`sum()`](./avg-count-max-min-sum/#sum) | Computes the sum of a set of summable values by adding them all together. If the set contains nulls, then these are simply ignored. |
| [`xmlagg()`](https://www.postgresql.org/docs/11/functions-aggregate.html) | This is not supported through Version YB 2.2. See [GitHub Issue #1043](https://github.com/yugabyte/yugabyte-db/issues/1043) |

## Statistical aggregate functions

The aggregate functions in this class can be invoked in one of two ways:

- _Either_ "ordinarily" on all the rows in a table or in connection with `GROUP BY`, when they return a single value for the set of rows. In this use, row ordering doesn't matter.

- _Or_ as a [window function](../../window_functions/)  with `OVER`.

  In this use, where the aggregate function is evaluated for each row in the [window](../../window_functions/invocation-syntax-semantics/#the-window-definition-rule), ordering always matters.

| Function | Description |
| ---- | ---- |
| [`covar_pop()`](./linear-regression/covar-corr/#covar-pop-covar-samp) | Returns the so-called covariance, taking the available values to be the entire population. |
| [`covar_samp()`](./linear-regression/covar-corr/#covar-pop-covar-samp) | Returns the so-called covariance, taking the available values to be a sample of the population. |
| [`corr()`](./linear-regression/covar-corr/#corr) | Returns the so-called correlation coefficient. This measures the extent to which the _"y"_ values are linearly related to the _"x"_ values. A return value of 1.0 indicates perfect correlation. |
| [`regr_avgy()`](./linear-regression/regr/#regr-avgy-regr-avgx) | Returns the average of the first argument for those rows where both arguments are `NOT NULL`. |
| [`regr_avgx()`](./linear-regression/regr/#regr-avgy) | Returns the average of the second argument for those rows where both arguments are `NOT NULL`. |
| [`regr_count()`](./linear-regression/regr/#regr-count) | Returns the number of rows where both arguments are `NOT NULL`. |
| [`regr_slope()`](./linear-regression/regr/#regr-slope-regr-intercept) | Returns the slope of the straight line that linear regression analysis has determined best fits the "(y, x)" pairs. |
| [`regr_intercept()`](./linear-regression/regr/#regr-slope-regr-intercept) | Returns the point at which the straight line that linear regression analysis has determined best fits the "(y, x)" pairs intercepts the "y"-axis. |
| [`regr_r2()`](./linear-regression/regr/#regr-r2) | Returns the square of the correlation coefficient, [`corr()`](./linear-regression/covar-corr/#corr). |
| [`regr_syy()`](./linear-regression/regr/#regr-syy-regr-sxx-regr-sxy) | Returns `regr_count(y, x)*var_pop(y)` for `NOT NULL` pairs. |
| [`regr_sxx()`](./linear-regression/regr/#regr-syy-regr-sxx-regr-sxy) | Returns `regr_count(y, x)*var_pop(x)` for `NOT NULL pairs`. |
| [`regr_sxy()`](./linear-regression/regr/#regr-syy-regr-sxx-regr-sxy) | Returns `regr_count(y, x)*covar_pop(y, x)` for `NOT NULL` pairs. |
| [`variance()`](./variance-stddev/#variance) | The semantics of `variance()` and [`var_samp()`](./variance-stddev/#var-samp) are identical. |
| [`var_pop()`](./variance-stddev/#var-pop) | Returns the variance of a set of values using the "population" variant of the formula that divides by _N_, the number of values. |
| [`var_samp()`](./variance-stddev/#var-samp) | Returns the variance of a set of values using the "sample" variant of the formula that divides by _(N - 1)_ where _N_ is the number of values. |
| [`stddev()`](./variance-stddev/#stddev) | The semantics of `stddev()` and [`stddev_samp()`](./variance-stddev/#stddev-samp) are identical. |
| [`stddev_pop()`](./variance-stddev/#stddev-pop) | Returns the standard deviation of a set of values using the naïve formula (i.e. the "population" variant) that divides by the number of values, _N_. |
| [`stddev_samp()`](./variance-stddev/#stddev-samp) | Returns the standard deviation of a set of values using the "sample" variant of the formula that divides by _(N - 1)_ where _N_ is the number of values. |

## Within-group ordered-set aggregate functions

These functions are sometimes referred to as “inverse distribution” functions. They can be invoked only with the dedicated `WITHIN GROUP (ORDER BY ...)` syntax. They cannot be invoked as a [window function](../../window_functions/)  with `OVER`.

| Function | Description |
| ---- | ---- |
| [`mode()`](./mode-percentile-disc-percentile-cont/#mode) | Return the most frequent value of "sort expression". If there's more than one equally-frequent value, then one of these is silently chosen arbitrarily. |
| [`percentile_disc()`](./mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont) | Discrete variant. The scalar overload of `percentile_disc()` takes a percentile rank value as input and returns the value, within the specified window, that would produce this. The array overload takes an array of percentile rank values as input and returns the array of values that would produce these. |
| [`percentile_cont()`](./mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont) | Continuous variant. The scalar overload of `percentile_cont()` takes a percentile rank value as input and returns the value, within the specified window, that would produce this. The array overload takes an array of percentile rank values as input and returns the array of values that would produce these. |

## Within-group hypothetical-set aggregate functions

These functions as invoked, as within-group hypothetical-set aggregate functions, with the dedicated `WITHIN GROUP (ORDER BY ...)` syntax. Further, each of the functions listed in this class is associated with a [window function](../../window_functions/) of the same name. (But not every window function can be invoked in this way.) For each, the result is the value that the associated window function would have returned for the “hypothetical” row constructed, as the invocation syntax specifies, as if such a row had been added to the [window](../../window_functions/invocation-syntax-semantics/#the-window-definition-rule).

| Function | Description |
| ---- | ---- |
| [`rank()`](./rank-dense-rank-percent-rank-cume-dist/#rank) | Returns the integer ordinal rank of each row according to the emergent order that the window `ORDER BY` clause specifies. The series of values starts with 1 but, when the window contains ties, the series is not dense. See the account of [rank()](../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#rank) in the [Window functions](../../window_functions/) section for more information. |
| [`dense_rank()`](./rank-dense-rank-percent-rank-cume-dist/#dense-rank) | Returns the integer ordinal rank of the distinct value of each row according to what the window `ORDER` BY clause specifies. The series of values starts with 1 and, even when the window contains ties, the series is dense. See the account of [dense_rank()](../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#dense-rank) in the [Window functions](../../window_functions/) section for more information.|
| [`percent_rank()`](./rank-dense-rank-percent-rank-cume-dist/#percent-rank) | Returns the percentile rank of each row within the window, with respect to the argument of the window_definition's window `ORDER BY` clause. See the account of [percent_rank()](../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) in the [Window functions](../../window_functions/) section for more information. |
| [`cume_dist()`](./rank-dense-rank-percent-rank-cume-dist/#cume-dist) | Returns a value that represents the number of rows with values less than or equal to the current row’s value divided by the total number of rows—in other words, the relative position of a value in a set of values. See the account of [cume_dist()](../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) in the [Window functions](../../window_functions/) section for more information. |




