---
title: grouping sets, rollup, cube
linkTitle: grouping sets, rollup, cube
headerTitle: Using the GROUPING SETS, ROLLUP, and CUBE syntax for aggregate function invocation
description: Explains the GROUPING SETS, ROLLUP, and CUBE syntax and semantics for aggregate function invocation.
menu:
  latest:
    identifier: grouping-sets-rollup-cube
    parent: aggregate-functions
    weight: 60
isTocNested: true
showAsideToc: true
---

This section shows how to use the `GROUPING SETS`, `ROLLUP`, and `CUBE` syntax, as part of the `GROUP BY` clause, in concert with the invocation, as a `SELECT` list item, of one or more aggregate functions. These constructs are useful when the relation defined by a subquery's `FROM` list has two or more columns that you want to use in the `GROUP BY` clause and when you want to use them with singly or in combinations that have fewer columns than the available number.

## Semantics

`GROUPING SETS (...)`,  is a shorthand notation to let you achieve, in a single terse subquery, what you could achieve by the union of several subqueries that each uses the plain `GROUP BY <expression list>` syntax. 

`ROLLUP (...)` and `CUBE` are each shorthand notations for specifying two common uses of the `GROUPING SET` syntax.

_To_do:_ x-ref to `GROUP BY` in syntax diagrams.

### GROUPING SETS

Suppose that the list of candidate columns (or expressions) is _"g1"_,... _"g2"_,... _"gN"_ and you want to produce the results from a set of aggregate functions using each individual one in turn as the `GROUP BY` argument and also using no `GROUP BY` clause at all. You can easily simply write several individual queries, like this:

```
select... fn(...) filter (where... ),... from... group by g1 having...;
select... fn(...) filter (where... ),... from... group by g2 having...;
...
select... fn(...) filter (where... ),... from... group by gN having...;
select... fn(...) filter (where... ),... from... having...;
```

You could also, by appropriate use of subqueries defined in a `WITH` clause, `UNION` the results of each to create a single joint results set. While doing this is straightforward, it can become quite verbose—especially if, for example, you want to use a `FILTER` clause as part of the aggregate function invocation and want to use a `HAVING` clause to restrict the result set.

The `GROUPING SETS` syntax lets you achieve the required result in a terse fashion:

```
select fn(...) filter (where... ),... from... group by grouping sets ((g1), (g2), ()) having...
```

You can include any number of columns (or expressions) within each successive parenthesis pair. The empty parenthesis brings the effect of no `GROUP BY` clause at all.

### ROLLUP

This:

```
rollup (g1, g2, g3,... )
```

represents the given list of expressions and all prefixes of the list including the empty list; thus it is equivalent to this:

```
grouping sets (
  (g1, g2, g3,... ),
  ...
  (g1, g2, g3),
  (g1, g2),
  (g1),
  ()
)
```

This is commonly used for analysis over hierarchical data, for example _"total salary"_ by _"department"_, _"division"_, and "_company-wide total_".

### CUBE

This:

```
cube (g1, g2, g3,... )
```

is equivalent to GROUPING SETS with the given list and all of its possible subsets (i.e. the power set). Therefore this:

```
cube (g1, g2, g3)
```

is equivalent to this:

```
grouping sets (
  (g1, g2, g3),
  (g1, g2    ),
  (    g2, g3),
  (g1,     g3),
  (g1        ),
  (    g2    ),
  (        g3),
  (          )
)
```

### Using the GROUPING keyword in the SELECT list to label the different GROUPING SETS

The `GROUPING` keyword is used to introduce a `SELECT` list item when the `GROUP BY` clause uses `GROUPING SETS`, `ROLLUP`, or `CUBE`. It produces a label so that result rows can be distinguished. The `GROUPING` arguments are not evaluated, but they must match exactly expressions given in the` GROUP BY` clause of the associated query level. Bits are assigned with the rightmost argument being the least-significant bit; each bit is 0 if the corresponding expression is included in the grouping criteria of the grouping set generating the result row, and 1 if it is not. See the section [Using the GROUPING keyword in the SELECT list to label the different GROUPING SETS](./#using-the-grouping-keyword-in-the-select-list-to-label-the-different-grouping-sets-1) at the end of the [Examples](./#examples) section below.

### Further detail

The individual elements of a `ROLLUP` or `CUBE` clause may be either individual expressions, or sublists of elements in parentheses. In the latter case, the sublists are treated as single units for the purposes of generating the individual grouping sets. For example, this:

```
rollup (g1, (g2, g3), g4)
```

is equivalent to this:

```
grouping sets (
  (g1, g2, g3, g4),
  (g1, g2, g3    ),
  (g1            ),
  (              )
)
```

And this:

```
cube ((g1, g2), (g3, g4))
```

is equivalent to this:

```
grouping sets (
  (g1, g2, g3, g4),
  (g1, g2        ),
  (        g3, g4),
  (              )
)
```

The `ROLLUP` and `CUBE` constructs can be used either directly in the `GROUP BY` clause, or nested inside a `GROUPING SETS` clause. If one `GROUPING SETS` clause is nested inside another, the effect is the same as if all the elements of the inner clause had been written directly in the outer clause.

If many grouping items are specified in a single `GROUP BY` clause, then the final list of grouping sets is the cross product of the individual items. For example, this:

```
group by g1, cube (g2, g3), grouping sets ((g4), (g5))
```

is equivalent to this:

```
group by grouping sets (
  (g1, g2, g3, g4), (g1, g2, g3, g5),
  (g1, g2, g4    ), (g1, g2, g5    ),
  (g1, g3, g4    ), (g1, g3, g5    ),
  (g1, g4        ), (g1, g5        )
)
```

## Examples

Create and populate the test table. Notice the `assert` that tests that each of the grouping columns _"g1"_ and _"g2_" has at least two rows for each of its two  values. The reason for this test is that [`stddev()`](../function-syntax-semantics/variance-stddev/#stddev) returns `NULL` when it is presented with just a single row. This unlucky outcome is very unlikely. But it was seen while developing this code example. This outcome, while not wrong, or even remarkable, makes the results produced by the verbose approach and the terse approach, below, a little harder to compare—thereby making the semantics demonstration a little less convincing.

```plpgsql
drop table if exists t cascade;
create table t(
  k  int primary key,
  g1 int not null,
  g2 int not null,
  v  int not null);

drop procedure if exists populate_t cascade;
create procedure populate_t(no_of_rows in int)
  language plpgsql
as $body$
declare
  s0 constant double precision not null := to_number(to_char(clock_timestamp(), 'ms'), '9999');
  s  constant double precision not null := s0/500.0 - 1.0;
begin
  perform setseed(s);

  delete from t;
  insert into t(k, g1, g2, v)
  select
    g.v,
    case random() < 0.5::double precision
      when true then 1
                else 2
    end,
    case random() < 0.5::double precision
      when true then 1
                else 2
    end,
    round(100*random())
  from generate_series(1, no_of_rows) as g(v);

  declare
    c_g1_1 constant int = (select count(v) from t where g1 = 1);
    c_g1_2 constant int = (select count(v) from t where g1 = 2);
    c_g2_1 constant int = (select count(v) from t where g2 = 1);
    c_g2_2 constant int = (select count(v) from t where g2 = 2);
  begin
    assert
      c_g1_1 > 1 and c_g1_2 > 1 and c_g2_1 > 1 and c_g2_2 > 1,
    'Unlucky outcome. Try again.';
  end;
end;
$body$;

call populate_t(100);
```

### GROUPING SETS

Now do three simple `GROUP BY` queries. (See [`avg()`](../function-syntax-semantics/avg-count-max-min-sum/#avg) and [`stddev()`](../function-syntax-semantics/variance-stddev/#stddev) for the specification of these two aggregate functions.)

```plpgsql
-- First:
select
  g1,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by g1
order by g1;

-- Second:
select
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by g2
order by g2;

-- Third:
select
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t;
```

Here are typical results:

```
 g1 |   avg   | stddev  
----+---------+---------
  1 |   53.65 |   26.94
  2 |   52.16 |   30.64
  
 g2 |   avg   | stddev  
----+---------+---------
  1 |   54.55 |   30.04
  2 |   51.81 |   26.72
  
   avg   | stddev  
---------+---------
   53.10 |   28.22
```

Now combine these three queries into a single, verbose, query:

```plpgsql
with
  a1 as (
    select g1, avg(v) as avg, stddev(v) as stddev from t group by g1),

  a2 as (
    select g2, avg(v) as avg, stddev(v) as stddev from t group by g2),

  a3 as (
    select avg(v) as avg, stddev(v) as stddev from t),

  a4 as (
    select g1,              null::int as g2, avg, stddev from a1
    union all
    select null::int as g1,              g2, avg, stddev from a2
    union all
    select null::int as g1, null::int as g2, avg, stddev from a3)

select
  g1,
  g2,
  to_char(avg,    '999.99') as avg,
  to_char(stddev, '999.99') as stddev
from a4
order by g1 nulls last, g2 nulls last;
```

This is the result for the table population that produced the typical results shown above:

```
 g1 | g2 |   avg   | stddev  
----+----+---------+---------
  1 |    |   53.65 |   26.94
  2 |    |   52.16 |   30.64
    |  1 |   54.55 |   30.04
    |  2 |   51.81 |   26.72
    |    |   53.10 |   28.22
```

The same result is given by this terse query using the `GROUPING SETS` syntax:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v),    '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by grouping sets ((g1), (g2), ())
order by g1 nulls last, g2 nulls last;
```

Notice that it's easy to include a `HAVING` clause in the terse `GROUPING SETS` syntax:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v),    '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by grouping sets ((g1), (g2), ())
having avg(v) > 53.0::numeric
order by g1 nulls last, g2 nulls last;
```
This is the result with the table population that produced the results above:

```
 g1 | g2 |   avg   | stddev  
----+----+---------+---------
  1 |    |   53.65 |   26.94
    |  1 |   54.55 |   30.04
    |    |   53.10 |   28.22
```

The actual result here will depend on the outcome of the _"no_of_rows"_ invocations of the `random()` function. It's possible (but quite rare) that you'll see no rows at all.

To get the same semantics with the verbose query, you must, of course, include the identical `HAVING` clause in each of the three `WITH` clause subqueries, _"a1"_, _"a2"_, and _"a3"_ that it uses. This makes the verbose formulation yet more verbose and, more importantly, yet more subject to copy-and-paste error when the aim is to repeat this identical text three times:

```
...avg(v) as avg, stddev(v) as stddev from t... having avg(v) > 50.0::numeric...
```

Extending this thinking, the terse syntax also makes it easier to add a `FILTER` clause:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v)    filter (where v > 20), '999.99') as avg,
  to_char(stddev(v) filter (where v > 20), '999.99') as stddev
from t
group by grouping sets ((g1), (g2), ())
having avg(v) > 53.0::numeric
order by g1 nulls last, g2 nulls last;
```

This is the result with the table population that produced the results above:

```
 g1 | g2 |   avg   | stddev  
----+----+---------+---------
  1 |    |   59.93 |   22.60
    |  1 |   63.79 |   23.85
    |    |   60.78 |   23.13
```

### ROLLUP

This `GROUP BY ROLLUP` clause:

```
group by rollup (g1, g2)
```

has the same meaning as this `GROUP BY GROUPING SETS` clause:

```
group by grouping sets ((g1, g2), (g1), ())
```

So this query:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by rollup (g1, g2)
order by g1 nulls last, g2 nulls last;
```

has the same meaning as this:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by grouping sets ((g1, g2), (g1), ())
order by g1 nulls last, g2 nulls last;
```

Each produces this result with the table population that produced the results above:

```
 g1 | g2 |   avg   | stddev  
----+----+---------+---------
  1 |  1 |   54.00 |   29.37
  1 |  2 |   53.35 |   25.12
  1 |    |   53.65 |   26.94
  2 |  1 |   55.44 |   31.93
  2 |  2 |   49.05 |   29.90
  2 |    |   52.16 |   30.64
    |    |   53.10 |   28.22
```

### CUBE

This `GROUP BY CUBE` clause:

```
group by cube (g1, g2)
```

has the same meaning as this `GROUP BY GROUPING SETS` clause:

```
group by grouping sets ((g1, g2), (g1), (g2), ())
```

So this query:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by cube (g1, g2)
order by g1 nulls last, g2 nulls last;
```

has the same meaning as this:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by grouping sets ((g1, g2), (g1), (g2), ())
order by g1 nulls last, g2 nulls last;
```

Each produces this result with the table population that produced the results above:

```
 g1 | g2 |   avg   | stddev  
----+----+---------+---------
  1 |  1 |   54.00 |   29.37
  1 |  2 |   53.35 |   25.12
  1 |    |   53.65 |   26.94
  2 |  1 |   55.44 |   31.93
  2 |  2 |   49.05 |   29.90
  2 |    |   52.16 |   30.64
    |  1 |   54.55 |   30.04
    |  2 |   51.81 |   26.72
    |    |   53.10 |   28.22
```

### Using the GROUPING keyword in the SELECT list to label the different GROUPING SETS

Try this:

```plpgsql
select
  grouping(g1, g2),
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by cube (g1, g2)
order by g1 nulls last, g2 nulls last;
```

It's equivalent to this:

```plpgsql
select
  grouping(g1, g2),
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by grouping sets ((g1, g2), (g1), (g2), ())
order by g1 nulls last, g2 nulls last;
```

Each produces this result with the table population that produced the results above:

```
 grouping | g1 | g2 |   avg   | stddev  
----------+----+----+---------+---------
        0 |  1 |  1 |   54.00 |   29.37
        0 |  1 |  2 |   53.35 |   25.12
        1 |  1 |    |   53.65 |   26.94
        0 |  2 |  1 |   55.44 |   31.93
        0 |  2 |  2 |   49.05 |   29.90
        1 |  2 |    |   52.16 |   30.64
        2 |    |  1 |   54.55 |   30.04
        2 |    |  2 |   51.81 |   26.72
        3 |    |    |   53.10 |   28.22
```

You might prefer to order first by the `GROUPING` item and not include it in the `SELECT` list:

```plpgsql
select
  g1,
  g2,
  to_char(avg(v), '999.99') as avg,
  to_char(stddev(v), '999.99') as stddev
from t
group by grouping sets ((g1, g2), (g1), (g2), ())
order by grouping(g1, g2), g1 nulls last, g2 nulls last;
```

It produces this result with the table population that produced the results above:

```
 g1 | g2 |   avg   | stddev  
----+----+---------+---------
  1 |  1 |   54.00 |   29.37
  1 |  2 |   53.35 |   25.12
  2 |  1 |   55.44 |   31.93
  2 |  2 |   49.05 |   29.90
  1 |    |   53.65 |   26.94
  2 |    |   52.16 |   30.64
    |  1 |   54.55 |   30.04
    |  2 |   51.81 |   26.72
    |    |   53.10 |   28.22
```
