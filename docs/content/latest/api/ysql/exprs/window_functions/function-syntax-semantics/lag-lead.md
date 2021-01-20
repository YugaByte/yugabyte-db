---
title: lag(), lead()
linkTitle: lag(), lead()
headerTitle: lag(), lead()
description: Describes the functionality of the YSQL window functions lag(), lead()
menu:
  latest:
    identifier: lag-lead
    parent: window-function-syntax-semantics
    weight: 40
isTocNested: true
showAsideToc: true
---

These functions look backwards, or forwards, by the specified number of rows from the current row, within the current [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). The functions fall into the second group, [Window functions that return column(s) of another row within the window](../#window-functions-that-return-column-s-of-another-row-within-the-window) in the section [List of all window functions](../#list-of-all-window-functions). Each of the functions in the second group makes obvious sense when the scope within which the specified row is found is the entire [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). Only this use will be described here.

## Example

{{< note title=" " >}}

If you haven't yet installed the tables that the code examples use, then go to the section [The data sets used by the code examples](../data-sets/).

{{< /note >}}

Try this basic demonstration. The actual argument, _2_, for each of `lag()` and `lead()` means, respectively, look back by two rows and look forward by two rows.

```plpgsql
\pset null '<null>'
select
  to_char(day, 'Dy DD-Mon') as "Day",
  price,
  lag (price, 2) over w as "last-but-one price",
  lead(price, 2) over w as "next-but-one price"
from t3
window w as (order by day)
order by day;
```

This is the result:

```
    Day     | price  | last-but-one price | next-but-one price 
------------+--------+--------------------+--------------------
 Mon 15-Sep | $19.01 |             <null> |             $18.10
 Tue 16-Sep | $18.96 |             <null> |             $18.75
 Wed 17-Sep | $18.10 |             $19.01 |             $20.07
 Thu 18-Sep | $18.75 |             $18.96 |             $19.75
 Fri 19-Sep | $20.07 |             $18.10 |             $19.69
 Mon 22-Sep | $19.75 |             $18.75 |             $19.95
 Tue 23-Sep | $19.69 |             $20.07 |             $20.47
 Wed 24-Sep | $19.95 |             $19.75 |             $20.62
 Thu 25-Sep | $20.47 |             $19.69 |             $18.77
 Fri 26-Sep | $20.62 |             $19.95 |             $18.29
 Mon 29-Sep | $18.77 |             $20.47 |             $19.86
 Tue 30-Sep | $18.29 |             $20.62 |             $19.49
 Wed 01-Oct | $19.86 |             $18.77 |             $19.48
 Thu 02-Oct | $19.49 |             $18.29 |             $18.30
 Fri 03-Oct | $19.48 |             $19.86 |             $16.78
 Mon 06-Oct | $18.30 |             $19.49 |             $16.88
 Tue 07-Oct | $16.78 |             $19.48 |             $16.21
 Wed 08-Oct | $16.88 |             $18.30 |             $16.68
 Thu 09-Oct | $16.21 |             $16.78 |             $18.86
 Fri 10-Oct | $16.68 |             $16.88 |             $17.69
 Mon 13-Oct | $18.86 |             $16.21 |             $15.95
 Tue 14-Oct | $17.69 |             $16.68 |             $16.99
 Wed 15-Oct | $15.95 |             $18.86 |             $17.02
 Thu 16-Oct | $16.99 |             $17.69 |             <null>
 Fri 17-Oct | $17.02 |             $15.95 |             <null>
```

Notice that _"last_but_one_price"_ is `NULL` for the first two rows. This is, of course, because looking back by two rows for each of these takes you to before the start of the window and so the result of_"lag(price, 2)"_ cannot be determined. In the same way,  _"next_but_one_price"_ is `NULL` for the last two rows. It's easier to check, visually, that the results are as promised by focusing on one particular row, say Wed 01-Oct, the previous-but-one row, Mon 29-Sep, and the next-but-one row, Fri 03-Oct, thus:

```plpgsql
with v as (
  select
    day,
    price,
    lag (price, 2) over w as lag_2,
    lead(price, 2) over w as lead_2
  from t3
  window w as (order by day))
select 
  to_char(day, 'Dy DD-Mon') as "Day",
  price,
  lag_2  as "last-but-one price",
  lead_2 as "next-but-one price"
from v
where to_char(day, 'Dy DD-Mon') in
  ('Mon 29-Sep', 'Wed 01-Oct', 'Fri 03-Oct')
order by day;
```

Notice that the `WHERE` clause restriction must be applied only _after_ the `lag()` and `lead()` window functions have been able to access _all_ the rows in the window.

The values for _"last-but-one price"_ and _"next-but-one price"_ have been manually blanked out in the results, here, to remove distractions from the visual check:

```
    Day     | price  | last-but-one price | next-but-one price 
------------+--------+--------------------+--------------------
 Mon 29-Sep | $18.77 |                    |
 Wed 01-Oct | $19.86 |             $18.77 |             $19.48
 Fri 03-Oct | $19.48 |                    |
```

## lag()

**Signature:**

```
input value:       anyelement, int, [, anyelement]
                   
return value:      anyelement
```

**Purpose:** Return, for the current row, the designated value from the row in the ordered input set that is _"lag_distance "_ rows before it. The data type of the _return value_ matches that of the _input value_. `NULL` is returned when the value of _"lag_distance"_ places the earlier row before the start of the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). This is illustrated by the example above.

Use the optional last parameter to specify the value to be returned, instead of `NULL`, when the looked-up row falls outside of the current [_window_](../../invocation-syntax-semantics/#the-window-definition-rule).

## lead()

**Signature:**

```
input value:       anyelement, int, [, anyelement]
                   
return value:      anyelement
```

**Purpose:** Return, for the current row, the designated value from the row in the ordered input set that is _"lead_distance"_ rows after it. The data type of the _return value_ matches that of the _input value_. `NULL` is returned when the value of _"lead_distance"_ places the later row after the start of the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). This is illustrated by the example above.

Use the optional last parameter to specify the value to be returned, instead of `NULL`, when the looked-up row falls outside of the current [_window_](../../invocation-syntax-semantics/#the-window-definition-rule).

## Example of returning a row

Here is the minimal demonstration of this technique. Notice that the syntax requires that the actual argument to `lag()` must used to construct a record or a _"row"_ type value. Usually, you want to access the individual column values, but, it can be tricky to work with records because they are anonymous data types. This means that you don't know what the fields are called. The better, therefore, is given by a user-defined _"row"_ type. Do this to demonstrate the technique:

```plpgsql
drop type if exists rt cascade;
create type rt as(day date, price money);
\pset null '<null>'

select
  day,
  price,
  lag((day, price)::rt, 1) over w as lag_1
from t3
window w as (order by day);
```

This is the result. (Some rows have been manually elided.)

```
    day     | price  |        lag_1        
------------+--------+---------------------
 2008-09-15 | $19.01 | <null>
 2008-09-16 | $18.96 | (2008-09-15,$19.01)
 2008-09-17 | $18.10 | (2008-09-16,$18.96)
 ...
 2008-10-15 | $15.95 | (2008-10-14,$17.69)
 2008-10-16 | $16.99 | (2008-10-15,$15.95)
 2008-10-17 | $17.02 | (2008-10-16,$16.99)
```

Now, the fields of the _"row"_ type values can be accessed, thus:

```plpgsql
drop type if exists rt cascade;
create type rt as(day date, price money);
\pset null '<null>'

with
  v1 as (
    select
      day,
      price,
      lag ((day, price)::rt, 1) over w as lag_1,
      lead((day, price)::rt, 1) over w as lead_1
    from t3
    window w as (order by day)),
  v2 as (
    select
      day,
      price,
      (lag_1).day     as lag_day,
      (lag_1).price   as lag_price,
      (lead_1).day    as lead_day,
      (lead_1).price  as lead_price
    from v1)
select
  to_char(day, 'Dy DD-Mon')       as "Curr Day",
  price                           as "Curr Price",
  '  '                            as " ",
  to_char(lag_day, 'Dy DD-Mon')   as "Prev Day",
  lag_price                       as "Prev Price",
  '  '                            as "  ",
  to_char(lead_day, 'Dy DD-Mon')  as "Next Day",
  lead_price                      as "Next Price"
from v2;
```

This is the result. (Again, some rows have been manually elided.)

```
  Curr Day  | Curr Price |    |  Prev Day  | Prev Price |    |  Next Day  | Next Price 
------------+------------+----+------------+------------+----+------------+------------
 Mon 15-Sep |     $19.01 |    | <null>     |     <null> |    | Tue 16-Sep |     $18.96
 Tue 16-Sep |     $18.96 |    | Mon 15-Sep |     $19.01 |    | Wed 17-Sep |     $18.10
 Wed 17-Sep |     $18.10 |    | Tue 16-Sep |     $18.96 |    | Thu 18-Sep |     $18.75
 ...
 Wed 15-Oct |     $15.95 |    | Tue 14-Oct |     $17.69 |    | Thu 16-Oct |     $16.99
 Thu 16-Oct |     $16.99 |    | Wed 15-Oct |     $15.95 |    | Fri 17-Oct |     $17.02
 Fri 17-Oct |     $17.02 |    | Thu 16-Oct |     $16.99 |    | <null>     |     <null>
```

## Realistic use case

The first example lists the daily change in stock price for the available data, deliberately excluding the case where `lag()` would look before the start of the window:

```plpgsql
with
  v1 as (
  select
    day,
    price,
    lag(price, 1) over (order by day) as prev_price
  from t3)
select
  to_char(day, 'Dy DD-Mon') as "Day",
  price,
  prev_price,
  to_char((100*(price - prev_price))/prev_price, '990.0') as percent_change
from v1
where day <> (select min(day) from t3)
order by day;
```

This is the result:

```
    Day     | price  | prev_price | percent_change 
------------+--------+------------+----------------
 Tue 16-Sep | $18.96 |     $19.01 |   -0.3
 Wed 17-Sep | $18.10 |     $18.96 |   -4.5
 Thu 18-Sep | $18.75 |     $18.10 |    3.6
 Fri 19-Sep | $20.07 |     $18.75 |    7.0
 Mon 22-Sep | $19.75 |     $20.07 |   -1.6
 Tue 23-Sep | $19.69 |     $19.75 |   -0.3
 Wed 24-Sep | $19.95 |     $19.69 |    1.3
 Thu 25-Sep | $20.47 |     $19.95 |    2.6
 Fri 26-Sep | $20.62 |     $20.47 |    0.7
 Mon 29-Sep | $18.77 |     $20.62 |   -9.0
 Tue 30-Sep | $18.29 |     $18.77 |   -2.6
 Wed 01-Oct | $19.86 |     $18.29 |    8.6
 Thu 02-Oct | $19.49 |     $19.86 |   -1.9
 Fri 03-Oct | $19.48 |     $19.49 |   -0.1
 Mon 06-Oct | $18.30 |     $19.48 |   -6.1
 Tue 07-Oct | $16.78 |     $18.30 |   -8.3
 Wed 08-Oct | $16.88 |     $16.78 |    0.6
 Thu 09-Oct | $16.21 |     $16.88 |   -4.0
 Fri 10-Oct | $16.68 |     $16.21 |    2.9
 Mon 13-Oct | $18.86 |     $16.68 |   13.1
 Tue 14-Oct | $17.69 |     $18.86 |   -6.2
 Wed 15-Oct | $15.95 |     $17.69 |   -9.8
 Thu 16-Oct | $16.99 |     $15.95 |    6.5
 Fri 17-Oct | $17.02 |     $16.99 |    0.2
```

The second example uses conventional techniques to show the row that saw the biggest daily price change:

```plpgsql
with
  v1 as (
  select
    day,
    price,
    lag(price, 1) over (order by day) as prev_price
  from t3),
  v2 as (
    select
      day,
      price,
      prev_price,
      (price - prev_price) as change
    from v1
    where day <> (select min(day) from t3))
select
  to_char(day, 'Dy DD-Mon') as "Day",
  price,
  prev_price,
  to_char((100*change)/prev_price, '990.0') as percent_change
from v2
where abs(change::numeric) = (select max(abs(change::numeric)) from v2);
```

This is the result:

```
    Day     | price  | prev_price | percent_change 
------------+--------+------------+----------------
 Mon 13-Oct | $18.86 |     $16.68 |   13.1
```
