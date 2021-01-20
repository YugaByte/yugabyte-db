---
title: table t3
linkTitle: table t3
headerTitle: Create and populate table t3
description: Creates and populate table t3 with data that allows the demonstration of the YSQL window functions.
menu:
  latest:
    identifier: table-t3
    parent: data-sets
    weight: 40
isTocNested: true
showAsideToc: true
---

{{< note title=" " >}}
Make sure that you read the section [The data sets used by the code examples](../../data-sets/) before running the script to create table _"t3"_. In particular, it's essential that you have installed the [pgcrypto](../../../../../extensions/#pgcrypto) extension.
{{< /note >}}

The rows in table  _"t3"_ are inserted in random order. It has twenty-five rows of the Monday through Friday prices of a stock. This supports demonstrations of the [`lag()` and `lead()`](../../lag-lead/) window functions. It is also used in the section [Informal overview of function invocation using the OVER clause](../../../functionality-overview/).

This `ysqlsh` script creates and populates able _"t3"_. Save it as `t3.sql`.

```plpgsql
-- Suppress the spurious warning that is raised
-- when the to-be-deleted table doesn't yet exist.
set client_min_messages = warning;
drop type if exists rt cascade;
drop table if exists t3;

create table t3(
  surrogate_pk uuid primary key,
  day date not null unique,
  price money not null);

insert into t3(surrogate_pk, day, price)
with v1 as (
  values
    (to_date('15-Sep-2008', 'DD-Mon-YYYY'), 19.01),
    (to_date('16-Sep-2008', 'DD-Mon-YYYY'), 18.96),
    (to_date('17-Sep-2008', 'DD-Mon-YYYY'), 18.10),
    (to_date('18-Sep-2008', 'DD-Mon-YYYY'), 18.75),
    (to_date('19-Sep-2008', 'DD-Mon-YYYY'), 20.07),
    (to_date('22-Sep-2008', 'DD-Mon-YYYY'), 19.75),
    (to_date('23-Sep-2008', 'DD-Mon-YYYY'), 19.69),
    (to_date('24-Sep-2008', 'DD-Mon-YYYY'), 19.95),
    (to_date('25-Sep-2008', 'DD-Mon-YYYY'), 20.47),
    (to_date('26-Sep-2008', 'DD-Mon-YYYY'), 20.62),
    (to_date('29-Sep-2008', 'DD-Mon-YYYY'), 18.77),
    (to_date('30-Sep-2008', 'DD-Mon-YYYY'), 18.29),
    (to_date('01-Oct-2008', 'DD-Mon-YYYY'), 19.86),
    (to_date('02-Oct-2008', 'DD-Mon-YYYY'), 19.49),
    (to_date('03-Oct-2008', 'DD-Mon-YYYY'), 19.48),
    (to_date('06-Oct-2008', 'DD-Mon-YYYY'), 18.30),
    (to_date('07-Oct-2008', 'DD-Mon-YYYY'), 16.78),
    (to_date('08-Oct-2008', 'DD-Mon-YYYY'), 16.88),
    (to_date('09-Oct-2008', 'DD-Mon-YYYY'), 16.21),
    (to_date('10-Oct-2008', 'DD-Mon-YYYY'), 16.68),
    (to_date('13-Oct-2008', 'DD-Mon-YYYY'), 18.86),
    (to_date('14-Oct-2008', 'DD-Mon-YYYY'), 17.69),
    (to_date('15-Oct-2008', 'DD-Mon-YYYY'), 15.95),
    (to_date('16-Oct-2008', 'DD-Mon-YYYY'), 16.99),
    (to_date('17-Oct-2008', 'DD-Mon-YYYY'), 17.02)),
  v2 as (
    select
      gen_random_uuid() as r,
      column1,
      column2
    from v1)
select
  r,
  column1,
  column2
from v2
order by r;
```

Now inspect its contents:

```plpgsql
-- Notice the absence of "ORDER BY".
select
  to_char(day, 'Dy DD-Mon-YYYY') as "Day",
  price
from t3;

select
  to_char(day, 'Dy DD-Mon-YYYY') as "Day",
  price
from t3
order by day;
```
This is the result of the second `SELECT`. To make them easier to read, a blank line has been manually inserted here between the rows for each successive week.
```
       Day       | price  
-----------------+--------
 Mon 15-Sep-2008 | $19.01
 Tue 16-Sep-2008 | $18.96
 Wed 17-Sep-2008 | $18.10
 Thu 18-Sep-2008 | $18.75
 Fri 19-Sep-2008 | $20.07

 Mon 22-Sep-2008 | $19.75
 Tue 23-Sep-2008 | $19.69
 Wed 24-Sep-2008 | $19.95
 Thu 25-Sep-2008 | $20.47
 Fri 26-Sep-2008 | $20.62

 Mon 29-Sep-2008 | $18.77
 Tue 30-Sep-2008 | $18.29
 Wed 01-Oct-2008 | $19.86
 Thu 02-Oct-2008 | $19.49
 Fri 03-Oct-2008 | $19.48

 Mon 06-Oct-2008 | $18.30
 Tue 07-Oct-2008 | $16.78
 Wed 08-Oct-2008 | $16.88
 Thu 09-Oct-2008 | $16.21
 Fri 10-Oct-2008 | $16.68

 Mon 13-Oct-2008 | $18.86
 Tue 14-Oct-2008 | $17.69
 Wed 15-Oct-2008 | $15.95
 Thu 16-Oct-2008 | $16.99
 Fri 17-Oct-2008 | $17.02
```
