---
title: YSQL aggregate functions
linkTitle: Aggregate functions
headerTitle: Aggregate functions
description: This major section describes the syntax and semantics of all of the aggregate functions that YSQL supports.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: aggregate-functions
    parent: api-ysql-exprs
isTocNested: true
showAsideToc: true
---

If you are already familiar with aggregate functions, then you can skip straight to the [syntax and semantics](./invocation-syntax-semantics/) section or the section that lists all of [the YSQL aggregate functions](./function-syntax-semantics/) and that links, in turn, to the definitive account of each function.

This page has only the [Synopsis](./#synopsis) section and the section [Organization of the aggregate functions documentation](./#organization-of-the-aggregate-functions-documentation) section.

## Synopsis

Aggregate functions operate on a set of values and return a single value that reflects a property of the set. The functions [`count()`](./function-syntax-semantics/avg-count-max-min-sum/#count) and [`avg()`](./function-syntax-semantics/avg-count-max-min-sum/#avg) are very familiar examples.

In the limit, the values in the set that the aggregate function operates on are taken from the whole of the result set that the `FROM` list defines, subject to whatever restriction the subquery's `WHERE` clause might define. Very commonly, the set in question is split into subsets according to what the `GROUP BY` clause specifies.

Very many aggregate functions may be invoked, not only using the ordinary syntax where `GROUP BY` might be used, but also as [window functions](../window_functions/).

Notice these differences and similarities between aggregate functions and window functions:

- A window function produces, in general, a different output value for _each different input row_ in the [_window_](../window_functions/invocation-syntax-semantics/#the-window-definition-rule).
- When an aggregate function is invoked using the regular `GROUP BY` clause, it produces a _single value_ for each entire subset that the `GROUP BY` clause defines.
- When an aggregate function is invoked in the same way as a window function, it might, or might not, produce the _same value_ for _each different input row_ in the [_window_](./invocation-syntax-semantics/#the-window-definition-rule). The exact behavior depends on what the [frame clause](../window_functions/invocation-syntax-semantics/#the-frame-clause-1) specifies.
- All of the thirty-seven aggregate functions are listed in the four tables in the section [Signature and purpose of each aggregate function](./function-syntax-semantics/).

## Organization of the aggregate functions documentation

The remaining pages are organized as follows:

### Informal overview of function invocation using the GROUP BY clause: [here](./functionality-overview/)

Skip this section entirely if you are already familiar with aggregate functions. It presents code examples that classify the aggregate functions into three kinds according to how they may be invoked:

- [ordinary aggregate functions](./functionality-overview/#ordinary-aggregate-functions)

- [within-group ordered-set aggregate functions](./functionality-overview/#ordinary-aggregate-functions)

- [within-group hypothetical-set aggregate functions](./functionality-overview/#within-group-hypothetical-set-aggregate-functions)

This section focuses on the _effect_ that each illustrated function has. It leaves formal definitions to the [invocation syntax and semantics](./invocation-syntax-semantics/) section and the [Signature and purpose of each aggregate function](./function-syntax-semantics/) section.

### Aggregate function invocation—SQL syntax and semantics: [here](./invocation-syntax-semantics/)

This section presents the formal treatment of the syntax and semantics of how an aggregate function is invoked as a special kind of `SELECT` list item—with the invocation syntax optionally decorated with an `ORDER BY` clause, or a `FILTER` clause. This account also explains the use of the `HAVING` clause which lets you restrict a result set according the the value(s) returned by a list of aggregate functions.

There are four variants of the `GROUP BY` invocation style: `GROUP BY <column list>`; `GROUP BY GROUPING SETS`; `GROUP BY ROLLUP`; and `GROUP BY CUBE`. Further, all but the bare `GROUP BY <column list>` allow the use of  a `GROUPING` keyword in the `SELECT` list to label the different `GROUPING SETS`. Because all of this requires a fairly lengthy explanation, this is covered in the dedicated section [`Using the GROUPING SETS, ROLLUP, and CUBE syntax for aggregate function invocation`](./grouping-sets-rollup-cube/).

### Signature and purpose of each aggregate function: [here](./function-syntax-semantics/)

The following list groups the thirty-seven aggregate functions in the same way that the sidebar items group them. The rationale for the grouping is explained in the referenced sections.

&#160;&#160;&#160;&#160;&#160;&#160;[`avg()`](./function-syntax-semantics/avg-count-max-min-sum/#avg)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`max()`](./function-syntax-semantics/avg-count-max-min-sum/#max-min)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`min()`](./function-syntax-semantics/avg-count-max-min-sum/#max-min)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`sum()`](./function-syntax-semantics/avg-count-max-min-sum/#sum)

&#160;&#160;&#160;&#160;&#160;&#160;[`array_agg()`](./function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#array-agg)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`string_agg()`](./function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#string-agg)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`jsonb_agg()`](./function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#jsonb-agg)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`jsonb_object_agg()`](./function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#jsonb-object-agg)

&#160;&#160;&#160;&#160;&#160;&#160;[`bit_and()`](./function-syntax-semantics/bit-and-or-bool-and-or/#bit-and)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`bit_or()`](./function-syntax-semantics/bit-and-or-bool-and-or/#bit-or)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`bool_and()`](./function-syntax-semantics/bit-and-or-bool-and-or/#bool-and)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`bool_or()`](./function-syntax-semantics/bit-and-or-bool-and-or/#bool-or)

&#160;&#160;&#160;&#160;&#160;&#160;[`variance()`](./function-syntax-semantics/variance-stddev/#variance)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`var_pop()`](./function-syntax-semantics/variance-stddev/#var-pop)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`var_samp()`](./function-syntax-semantics/variance-stddev/#var-samp)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`stddev()`](./function-syntax-semantics/variance-stddev/#stddev)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`stddev_pop()`](./function-syntax-semantics/variance-stddev/#stddev-pop)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`stddev_samp()`](./function-syntax-semantics/variance-stddev/#stddev-samp)

&#160;&#160;&#160;&#160;&#160;&#160;[`covar_pop()`](./function-syntax-semantics/linear-regression/covar-corr/#covar-pop-covar-samp)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`covar_samp()`](./function-syntax-semantics/linear-regression/covar-corr/#covar-pop-covar-samp)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`corr()`](./function-syntax-semantics/linear-regression/covar-corr/#corr)

&#160;&#160;&#160;&#160;&#160;&#160;[`regr_avgy()`](./function-syntax-semantics/linear-regression/regr/#regr-avgy-regr-avgx)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_avgx()`](./function-syntax-semantics/linear-regression/regr/#regr-avgy-regr-avgx)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_count()`](./function-syntax-semantics/linear-regression/regr/#regr-count)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_slope()`](./function-syntax-semantics/linear-regression/regr/#regr-slope-regr-intercept)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_intercept()`](./function-syntax-semantics/linear-regression/regr/#regr-slope-regr-intercept)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_r2()`](./function-syntax-semantics/linear-regression/regr/#regr-r2)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_syy()`](./function-syntax-semantics/linear-regression/regr/#regr-syy-regr-sxx-regr-sxy)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_sxx()`](./function-syntax-semantics/linear-regression/regr/#regr-syy-regr-sxx-regr-sxy)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`regr_sxy()`](./function-syntax-semantics/linear-regression/regr/#regr-syy-regr-sxx-regr-sxy)

&#160;&#160;&#160;&#160;&#160;&#160;[`mode()`](./function-syntax-semantics/mode-percentile-disc-percentile-cont/#mode)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`percentile_disc()`](./function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`percentile_cont()`](./function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont)

&#160;&#160;&#160;&#160;&#160;&#160;[`rank()`](./function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#rank)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`dense_rank()`](./function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#dense-rank)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`percent_rank()`](./function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#percent-rank)<br>
&#160;&#160;&#160;&#160;&#160;&#160;[`cume_dist()`](./function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#cume-dist)

### Aggregate functions case study—the "68–95–99.7" rule: [here](./case-study-the-6895997-rule/)

Regard this section as an optional extra. It shows the use of aggregate functions to demonstrate the so-called "68–95–99.7 rule"—described in [this Wikipedia article](https://en.wikipedia.org/wiki/68%e2%80%9395%e2%80%9399.7_rule). This case-study focuses on just one part of the rule:

> 68.27% of the values in a normal distribution lie within one standard deviation each side of the mean.