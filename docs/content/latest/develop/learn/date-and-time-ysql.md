---
title: Date and time in YSQL
headerTitle: Date and time
linkTitle: 7. Date and time
description: Learn how to work with date and time in YSQL.
aliases:
  - /latest/explore/date-and-time/
  - /latest/develop/learn/date-and-time/
menu:
  latest:
    parent: learn
    name: 7. Date and time
    identifier: date-and-time-1-ysql
    weight: 569
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/develop/learn/date-and-time-ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/develop/learn/date-and-time-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Introduction

YugabyteDB has extensive date and time capability that may be daunting for the new user. Once understood, the rich functionality will allow you to perform very sophisticated calculations and granular time capture.

For date and time data types, see [Data types](/latest/api/ysql/datatypes/).

## Special values

There are special values that you can reference - YugabyteDB only caters for some, other special values from postgresql are not implemented in YugabyteDB, but some can be recreated if you require them. Below is YSQL to select special date and time values. First start ysql from your command line.

```sh
./bin/ysqlsh
```

```
yugabyte=# select current_date, current_time, current_timestamp, now();

 current_date |    current_time    |       current_timestamp       |              now
--------------+--------------------+-------------------------------+-------------------------------
 2019-07-09   | 00:53:13.924407+00 | 2019-07-09 00:53:13.924407+00 | 2019-07-09 00:53:13.924407+00

yugabyte=# select make_timestamptz(1970, 01, 01, 00, 00, 00, 'UTC') as epoch;

         epoch
------------------------
 1970-01-01 00:00:00+00

yugabyte=# select (current_date-1)::timestamp as yesterday,
                  current_date::timestamp as today,
                  (current_date+1)::timestamp as tomorrow;

      yesterday      |        today        |      tomorrow
---------------------+---------------------+---------------------
 2019-07-08 00:00:00 | 2019-07-09 00:00:00 | 2019-07-10 00:00:00

```

{{< note title="Note" >}}
YugabyteDB cannot create the special values of `infinity`, `-infinity`, and `allballs` that can be found in postgresql. If you are wondering, 'allballs' is a theoretical time of "00:00:00.00 UTC".
{{< /note >}}

## Formatting

Date formatting is an important aspect. As you can see above, the examples show the default ISO format for dates and timestamps. We will now do some formatting of dates.

```
yugabyte=# select to_char(current_timestamp, 'DD-MON-YYYY');

   to_char
-------------
 09-JUL-2019

yugabyte=# select to_date(to_char(current_timestamp, 'DD-MON-YYYY'), 'DD-MON-YYYY');

  to_date
------------
 2019-07-09

yugabyte=# select to_char(current_timestamp, 'DD-MON-YYYY HH:MI:SS PM');

         to_char
-------------------------
 09-JUL-2019 01:50:13 AM

```

In the above you will see that to present the date in a friendly readable format, the date and time needs to be represented in text using `TO_CHAR`. When it is represented as a date or time data type, it is displayed using system settings, hence why the date representation of text `09-JUL-2019` appears as `2019-07-09`.

## Time zones

Thus far, you have been operating with the default time zone installed for YugabyteDB being UTC (+0). Lets select what time zones are available from Yugabyte:

```
yugabyte=# select * from pg_timezone_names;

               name               | abbrev | utc_offset | is_dst
----------------------------------+--------+------------+--------
 W-SU                             | MSK    | 03:00:00   | f
 GMT+0                            | GMT    | 00:00:00   | f
 ROK                              | KST    | 09:00:00   | f
 UTC                              | UTC    | 00:00:00   | f
 US/Eastern                       | EDT    | -04:00:00  | t
 US/Pacific                       | PDT    | -07:00:00  | t
 US/Central                       | CDT    | -05:00:00  | t
 MST                              | MST    | -07:00:00  | f
 Zulu                             | UTC    | 00:00:00   | f
 posixrules                       | EDT    | -04:00:00  | t
 GMT                              | GMT    | 00:00:00   | f
 Etc/UTC                          | UTC    | 00:00:00   | f
 Etc/Zulu                         | UTC    | 00:00:00   | f
 Etc/Universal                    | UTC    | 00:00:00   | f
 Etc/GMT+2                        | -02    | -02:00:00  | f
 Etc/Greenwich                    | GMT    | 00:00:00   | f
 Etc/GMT+12                       | -12    | -12:00:00  | f
 Etc/GMT+8                        | -08    | -08:00:00  | f
 Etc/GMT-12                       | +12    | 12:00:00   | f
 WET                              | WEST   | 01:00:00   | t
 EST                              | EST    | -05:00:00  | f
 Australia/West                   | AWST   | 08:00:00   | f
 Australia/Sydney                 | AEST   | 10:00:00   | f
 GMT-0                            | GMT    | 00:00:00   | f
 PST8PDT                          | PDT    | -07:00:00  | t
 Hongkong                         | HKT    | 08:00:00   | f
 Singapore                        | +08    | 08:00:00   | f
 Universal                        | UTC    | 00:00:00   | f
 Arctic/Longyearbyen              | CEST   | 02:00:00   | t
 UCT                              | UCT    | 00:00:00   | f
 GMT0                             | GMT    | 00:00:00   | f
 Europe/London                    | BST    | 01:00:00   | t
 GB                               | BST    | 01:00:00   | t
 ...
(593 rows)

```

{{< note title="Note" >}}
**Not all 593 rows are shown**, so don't be concerned if the timezone you want is not there. Check your YSQL output to find the timezone you are interested in. What has been left in the results above is that there is a lot of inconsistency in the naming convention and definition of the timezones, this is not the doing of Yugabyte!
{{< /note >}}

You can set the timezone to use for your session using the `SET` command. You can `SET` timezone using the timezone name as listed in pg_timezone_names, but not the abbreviation. You can also set the timezone to a numeric/decimal representation of the time offset. For example, -3.5 is 3 hours and 30 minutes before UTC.

It seems logical to be able to set the timezone using the `UTC_OFFSET` format above. YugabyteDB will allow this, however, be aware of the following behaviour if you choose this method:

{{< tip title="Tip" >}}
When using POSIX time zone names, positive offsets are used for locations west of Greenwich. Everywhere else, YugabyteDB follows the ISO-8601 convention that positive timezone offsets are east of Greenwich. Therefore an entry of '+10:00:00' will result in a timezone offset of -10 Hours as this is deemed East of Greenwich.
{{< /tip >}}

Lets start examining dates and times within Yugabyte.

```
yugabyte=# \echo `date`

Tue 09 Jul 12:27:08 AEST 2019

```

Note that the above does not use quotes, but is the "Grave Accent" symbol, which is normally found below the Tilde `~` symbol on your keyboard.

The above is showing you the current date and time of the underlying server.  It is not the date and time of the database. However, in a single node implementation of YugabyteDB there will be a relationship between your computer's date and the database date because YugabyteDB would have obtained the date from the server when it was started. We will explore the date and time (timestamps) within the database.

```
yugabyte=# SHOW timezone;

 TimeZone
----------
 UTC

yugabyte=# select current_timestamp;

      current_timestamp
------------------------------
 2019-07-09 02:27:46.65152+00

yugabyte=# SET timezone = +1;
SET

yugabyte=# SHOW timezone;

 TimeZone
----------
 <+01>-01

yugabyte=# select current_timestamp;

      current_timestamp
------------------------------
 2019-07-09 03:28:11.52311+01


yugabyte=# SET timezone = -1.5;
SET

yugabyte=# select current_timestamp;

        current_timestamp
----------------------------------
 2019-07-09 00:58:27.906963-01:30

yugabyte=# SET timezone = 'Australia/Sydney';
SET

yugabyte=# SHOW timezone;

     TimeZone
------------------
 Australia/Sydney

yugabyte=# select current_timestamp;

       current_timestamp
-------------------------------
 2019-07-09 12:28:46.610746+10

yugabyte=# SET timezone = 'UTC';
SET

yugabyte=# select current_timestamp;

       current_timestamp
-------------------------------
 2019-07-09 02:28:57.610746+00

yugabyte=# select current_timestamp AT TIME ZONE 'Australia/Sydney';

          timezone
----------------------------
 2019-07-09 12:29:03.416867

yugabyte=# select current_timestamp(0);

   current_timestamp
------------------------
 2019-07-09 03:15:38+00

yugabyte=# select current_timestamp(2);

     current_timestamp
---------------------------
 2019-07-09 03:15:53.07+00

```

As shown above, you can set your SESSION to a particular timezone. When working with timestamps, you can control their seconds precision by specifying a value from 0 -> 6. Timestamps cannot go beyond millisecond precision which is 1,000,000 parts to one second.

If your application assumes a local time, ensure that it issues a `SET` command to set to the correct time offset. Note that Daylight Savings is quite an advanced topic, so for the time being it is recommended to instead use the offset notation, for example -3.5 for 3 hours and 30 minutes before UTC.

Note that the `AT TIME ZONE` statement above does not cater for the variants of `WITH TIME ZONE` and `WITHOUT TIME ZONE`.

## Timestamps

{{< note title="Note" >}}
A database normally obtains its date and time from the underlying server. However, in the case of a distributed database, it is one synchronized database that is spread across many servers that are unlikely to have synchronized time.

A detailed explanation of how time is obtained can be found at the blog post describing the [architecture of the storage layer](https://blog.yugabyte.com/distributed-postgresql-on-a-google-spanner-architecture-storage-layer/)

A simpler explanation is that the time is determined by the 'Shard Leader' of the table and this is the time used by all followers of the leader. Therefore there could be differences to the UTC timestamp of the underlying server to the current timestamp that is used for a transaction on a particular table.
{{< /note >}}

Lets start working with dates and timestamps. The following assumes that you have installed the yb_demo database and its demo data.

```
yugabyte=# \c yb_demo
You are now connected to database "yb_demo" as user "yugabyte".

yb_demo=# select to_char(max(orders.created_at), 'DD-MON-YYYY HH24:MI') AS "Last Order Date" from orders;

  Last Order Date  
-------------------
 19-APR-2020 14:07

yb_demo=# select extract(MONTH from o.created_at) AS "Mth Num", to_char(o.created_at, 'MON') AS "Month",
          extract(YEAR from o.created_at) AS "Year", count(*) AS "Orders"
          from orders o
          where o.created_at > current_timestamp(0)
          group by 1,2,3
          order by 3 DESC, 1 DESC limit 10;

 Mth Num | Month | Year | Orders
---------+-------+------+--------
       4 | APR   | 2020 |    344
       3 | MAR   | 2020 |    527
       2 | FEB   | 2020 |    543
       1 | JAN   | 2020 |    580
      12 | DEC   | 2019 |    550
      11 | NOV   | 2019 |    542
      10 | OCT   | 2019 |    540
       9 | SEP   | 2019 |    519
       8 | AUG   | 2019 |    566
       7 | JUL   | 2019 |    421
(10 rows)

yb_demo=# select to_char(o.created_at, 'HH AM') AS "Popular Hours", count(*) AS "Orders"
          from orders o
          group by 1
          order by 2 DESC
          limit 4;

 Popular Hours | Orders
---------------+--------
 12 PM         |    827
 11 AM         |    820
 03 PM         |    812
 08 PM         |    812
(4 rows)

yb_demo=# update orders
          set created_at = created_at + ((floor(random() * (25-2+2) + 2))::int * interval '1 day 14 hours');

UPDATE 18760

yb_demo=# select to_char(o.created_at, 'Day') AS "Top Day",
          count(o.*) AS "SALES"
          from orders o
          group by 1
          order by 2 desc;

Top Day  | SALES
-----------+---------
 Monday    |    2786
 Tuesday   |    2737
 Saturday  |    2710
 Wednesday |    2642
 Friday    |    2634
 Sunday    |    2630
 Thursday  |    2621
(7 rows)

yb_demo=# create table order_deliveries (
          order_id bigint,
          creation_date date DEFAULT current_date,
          delivery_date timestamptz);

CREATE TABLE

yb_demo=# insert into order_deliveries
          (order_id, delivery_date)
          select o.id, o.created_at + ((floor(random() * (25-2+2) + 2))::int * interval '1 day 3 hours')
          from orders o
          where o.created_at < current_timestamp - (20 * interval '1 day');

INSERT 0 12268

yb_demo=# select * from order_deliveries limit 5;

 order_id | creation_date |       delivery_date
----------+---------------+----------------------------
     5636 | 2019-07-09    | 2017-01-06 03:06:01.071+00
    10990 | 2019-07-09    | 2018-12-16 12:02:56.169+00
    13417 | 2019-07-09    | 2018-06-26 09:28:02.153+00
     9367 | 2019-07-09    | 2017-05-21 06:49:42.298+00
    13954 | 2019-07-09    | 2019-02-08 04:07:01.457+00
(5 rows)

yb_demo=# select d.order_id, to_char(o.created_at, 'DD-MON-YYYY HH AM') AS "Ordered",
          to_char(d.delivery_date, 'DD-MON-YYYY HH AM') AS "Delivered",
          d.delivery_date - o.created_at AS "Delivery Days"
          from orders o, order_deliveries d
          where o.id = d.order_id
          and d.delivery_date - o.created_at > interval '15 days'
          order by d.delivery_date - o.created_at DESC, d.delivery_date DESC limit 10;

 order_id |      Ordered      |     Delivered     |  Delivery Days
----------+-------------------+-------------------+------------------
    10984 | 12-JUN-2019 08 PM | 07-JUL-2019 02 AM | 24 days 06:00:00
     6263 | 01-JUN-2019 03 AM | 25-JUN-2019 09 AM | 24 days 06:00:00
    10498 | 18-MAY-2019 01 AM | 11-JUN-2019 07 AM | 24 days 06:00:00
    14996 | 14-MAR-2019 05 PM | 08-APR-2019 12 AM | 24 days 06:00:00
     6841 | 06-FEB-2019 01 AM | 02-MAR-2019 07 AM | 24 days 06:00:00
    10977 | 11-MAY-2019 01 PM | 03-JUN-2019 07 PM | 23 days 06:00:00
    14154 | 09-APR-2019 01 PM | 02-MAY-2019 07 PM | 23 days 06:00:00
     6933 | 31-MAY-2019 05 PM | 23-JUN-2019 12 AM | 22 days 06:00:00
     5289 | 04-MAY-2019 04 PM | 26-MAY-2019 10 PM | 22 days 06:00:00
    10226 | 01-MAY-2019 06 AM | 23-MAY-2019 12 PM | 22 days 06:00:00
(10 rows)

```

{{< note title="Note" >}}
Your data will be slightly different as you used a `RANDOM()` function for setting the 'delivery_date' in the new 'order_deliveries' table.
{{< /note >}}

You can use views of the YugabyteDB Data Catalogs to create data that is already prepared and formatted for your application code so that your SQL is simpler. Below is an example that is defined in the yb_demo database (has no dependency on yb_demo). This demonstration shows how you can nominate a shortlist of timezones that are formatted and ready to use for display purposes.

```
yb_demo=# CREATE OR REPLACE VIEW TZ AS
          select '* Current time' AS "tzone", '' AS "offset", to_char(current_timestamp AT TIME ZONE 'Australia/Sydney', 'Dy dd-Mon-yy hh:mi PM') AS "Local Time"
          UNION
          select x.name AS "tzone",
          left(x.utc_offset::text, 5) AS "offset",
          to_char(current_timestamp AT TIME ZONE x.name, 'Dy dd-Mon-yy hh:mi PM') AS "Local Time"
          from pg_catalog.pg_timezone_names x
          where  x.name like 'Australi%' or name in('Singapore', 'NZ', 'UTC')
          order by 1 asc;

CREATE VIEW

yb_demo=# select * from tz;

         tzone         | offset |       Local Time
-----------------------+--------+------------------------
 * Current time        |        | Wed 10-Jul-19 11:49 AM
 Australia/ACT         | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Adelaide    | 09:30  | Wed 10-Jul-19 11:19 AM
 Australia/Brisbane    | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Broken_Hill | 09:30  | Wed 10-Jul-19 11:19 AM
 Australia/Canberra    | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Currie      | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Darwin      | 09:30  | Wed 10-Jul-19 11:19 AM
 Australia/Eucla       | 08:45  | Wed 10-Jul-19 10:34 AM
 Australia/Hobart      | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/LHI         | 10:30  | Wed 10-Jul-19 12:19 PM
 Australia/Lindeman    | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Lord_Howe   | 10:30  | Wed 10-Jul-19 12:19 PM
 Australia/Melbourne   | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/NSW         | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/North       | 09:30  | Wed 10-Jul-19 11:19 AM
 Australia/Perth       | 08:00  | Wed 10-Jul-19 09:49 AM
 Australia/Queensland  | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/South       | 09:30  | Wed 10-Jul-19 11:19 AM
 Australia/Sydney      | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Tasmania    | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/Victoria    | 10:00  | Wed 10-Jul-19 11:49 AM
 Australia/West        | 08:00  | Wed 10-Jul-19 09:49 AM
 Australia/Yancowinna  | 09:30  | Wed 10-Jul-19 11:19 AM
 NZ                    | 12:00  | Wed 10-Jul-19 01:49 PM
 Singapore             | 08:00  | Wed 10-Jul-19 09:49 AM
 UTC                   | 00:00  | Wed 10-Jul-19 01:49 AM
(27 rows)

```

Assuming that you chose the timezones that interest you, then your results should be different to those shown above.

{{< tip title="Fun Fact" >}}
Who would have thought that Australia needs 23 timezone records ?
{{< /tip >}}

## Date and time intervals

You may have noticed that the above YSQL has references to `INTERVAL`. An interval is a data type that describes an increment of time. An interval allows you to show the difference between two timestamps or to create a new timestamp by adding or subtracting a particular unit of measure. Some examples are:

```
yugabyte=# select current_timestamp AS "Current Timestamp",
           current_timestamp + (10 * interval '1 min') AS "Plus 10 Mins",
           current_timestamp + (10 * interval '3 min') AS "Plus 30 Mins",
           current_timestamp + (10 * interval '2 hour') AS "Plus 20 hours",
           current_timestamp + (10 * interval '1 month') AS "Plus 10 Months"

       Current Timestamp       |         Plus 10 Mins          |         Plus 30 Mins          |         Plus 20 hours         |        Plus 10 Months
-------------------------------+-------------------------------+-------------------------------+-------------------------------+-------------------------------
 2019-07-09 05:08:58.859123+00 | 2019-07-09 05:18:58.859123+00 | 2019-07-09 05:38:58.859123+00 | 2019-07-10 01:08:58.859123+00 | 2020-05-09 05:08:58.859123+00

yugabyte=# select current_time::time(0), time '05:00' + interval '5 hours 7 mins' AS "New time";

 current_time | New Time
--------------+----------
 05:09:24     | 10:16:24

yugabyte=# select current_date - date '01-01-2019' AS "Day of Year(A)", current_date - date_trunc('year', current_date) AS "Day of Year(B)";

 Day of Year(A) | Day of Year(B)
----------------+----------------
            189 | 189 days

yugabyte=# select timestamp '2019-07-09 10:00:00.000000+00' - timestamp '2019-07-09 09:00:00.000000+00' AS "Time Difference";

 Time Difference
-----------------
 01:00:00

yugabyte=# select timestamp '2019-07-09 10:00:00.000000+00' - timestamptz '2019-07-09 10:00:00.000000+00' AS "Time Offset";

 Time Offset
-------------
 00:00:00

yugabyte=# select timestamp '2019-07-09 10:00:00.000000+00' - timestamptz '2019-07-09 10:00:00.000000EST' AS "Time Offset";

 Time Offset
-------------
 -05:00:00

yugabyte=# select timestamp '2019-07-09 10:00:00.000000+00' - timestamptz '2019-07-08 10:00:00.000000EST' AS "Time Offset";

 Time Offset
-------------
 19:00:00

yugabyte=# select timestamp '2019-07-09 10:00:00.000000+00' - timestamptz '2019-07-07 10:00:00.000000EST' AS "Time Offset";

  Time Offset
----------------
 1 day 19:00:00

yugabyte=# select age(timestamp '2019-07-09 10:00:00.000000+00', timestamptz '2019-07-07 10:00:00.000000EST') AS "Age Diff";

    Age Diff
----------------
 1 day 19:00:00

yugabyte=# select (extract('days' from age(timestamp '2019-07-09 10:00:00.000000+00', timestamptz '2019-07-07 10:00:00.000000EST'))*24)+
           (extract('hours' from age(timestamp '2019-07-09 10:00:00.000000+00', timestamptz '2019-07-07 10:00:00.000000EST'))) AS "Hours Diff";

 Hours Diff
------------
         43

```

The above shows that date and time manipulation can be achieved in several ways. It is important to note that some outputs are of type `INTEGER`, whilst others are of type `INTERVAL` (not text as they may appear). The final YSQL above for "Hours Diff" uses the output of `EXTRACT` which produces an `INTEGER` so that it may be multiplied by the hours per day whereas the `EXTRACT` function itself requires either a `INTERVAL` or `TIMESTAMP(TZ)` data type as its input.

Ensure to cast your values thoroughly. Casts can be done for time(tz), date and timestamp(tz) like `MY_VALUE::timestamptz`.

{{< note title="Note" >}}
The `EXTRACT` command is the preferred command to `DATE_PART`.
{{< /note >}}

## Manipulating using truncation

Another useful command is `DATE_TRUNC` which is used to 'floor' the timestamp to a particular unit. For the following YSQL, you assume that you are in the 'yb_demo' database with the demo data loaded.

```
yb_demo=# select date_trunc('hour', current_timestamp);
       date_trunc
------------------------
 2019-07-09 06:00:00+00
(1 row)

yb_demo=# select to_char((date_trunc('month', generate_series)::date)-1, 'DD-MON-YYYY') AS "Last Day of Month"
          from generate_series(current_date-(365-1), current_date, '1 month');

 Last Day of Month
-------------------
 30-JUN-2018
 31-JUL-2018
 31-AUG-2018
 30-SEP-2018
 31-OCT-2018
 30-NOV-2018
 31-DEC-2018
 31-JAN-2019
 28-FEB-2019
 31-MAR-2019
 30-APR-2019
 31-MAY-2019
(12 rows)

yb_demo=# select date_trunc('days', age(created_at)) AS "Product Age" from products order by 1 desc limit 10;

      Product Age
------------------------
 3 years 2 mons 12 days
 3 years 2 mons 10 days
 3 years 2 mons 6 days
 3 years 2 mons 4 days
 3 years 1 mon 28 days
 3 years 1 mon 27 days
 3 years 1 mon 15 days
 3 years 1 mon 9 days
 3 years 1 mon 9 days
 3 years 1 mon
(10 rows)

```

## Bringing it all together

A common requirement is to find out the date of next Monday, for example that might be the first day of the new week for scheduling purposes. This can be achieved in many ways, maybe in a simpler fashion than I have illustrated below. Below illustrates the chaining together of different date and time operators and functions to achieve the result you want.

```
yugabyte=# select to_char(current_date, 'Day, DD-MON-YYYY') AS "Today",
           to_char((current_timestamp AT TIME ZONE 'Australia/Sydney')::date +
           (7-(extract('isodow' from current_timestamp AT TIME ZONE 'Australia/Sydney'))::int + 1),
           'Day, DD-MON-YYYY') AS "Start of Next Week";

         Today          |   Start of Next Week
------------------------+------------------------
 Tuesday  , 09-JUL-2019 | Monday   , 15-JUL-2019

```

The above approach is to `EXTRACT` the current day of the week as an integer. As today is a Tuesday, the result will be 2. As you know there are 7 days per week, you need to target a calculation that has a result of 8, being 1 day more than the 7th day. We use this to calculate how many days to add to the current date (7 days - 2 + 1 day) to arrive at the next Monday which is day of the week (ISO dow) #1. My addition of the `AT TIME ZONE` was purely illustrative and would not impact the result because I am dealing with days, and my timezone difference is only +10 hours, therefore it does not affect the date. However, if you are working with hours or smaller, then the timezone will *potentially* have a bearing on your result.

{{< tip title="Fun Fact" >}}
For the very curious, why is there a gap after 'Tuesday' and 'Monday' in the example above? All 'Day' values are space padded to 9 characters. You could use string functions to remove the extra spaces if needed for formatting purposes or you could do a trimmed `TO_CHAR` for the 'Day' then concatenate with a comma and another `TO_CHAR` for the 'DD-MON-YYYY'.
{{< /tip >}}

## Ambiguity - Using DateStyle

People in different locations of the world are familiar with local representations of dates. Times are reasonably similar, but dates can differ. Within the USA, they use 3/5/19, whereas in Australia you would use 5/3/19 and in Europe they would use either 5.3.19 or 5/3/19. What is the date in question? 5th March, 2019.

YugabyteDB has `DateStyle` which is a setting that you apply to your session so that ambiguous dates can be determined and the display of dates in YSQL can be defaulted to a particular format.

By default, YugabyteDB uses the ISO Standard of YYYY-MM-DD HH24:MI:SS. Other settings you can use are 'SQL', 'German', and 'Postgres'. These are all referenced below allowing you to see examples.

All settings except ISO allow you specify whether a Day appears before or after the Month. Therefore, a setting of 'DMY' will result in 3/5 being 3rd May, whereas 'MDY' will result in 5th March.

If you are reading dates as text fields from a file or any source that is not a YugabyteDB date or timestamp data type, then it is very important that you set your DateStyle properly unless you are very specific on how to convert a text field to a date - an example of which is included below.

Note that YugabyteDB will always interpret '6/6' as 6th June, and '13/12' as 13th December (because the month cannot be 13), but what about about '6/12'? Lets work through some examples within YSQL.

```
yugabyte=# SHOW DateStyle;

 DateStyle
-----------
 ISO, DMY

yugabyte=# select current_date, current_time(0), current_timestamp(0);

 current_date | current_time |   current_timestamp
--------------+--------------+------------------------
 2019-07-09   | 20:26:28+00  | 2019-07-09 20:26:28+00

yugabyte=# SET DateStyle = 'SQL, DMY';
SET

yugabyte=# select current_date, current_time(0), current_timestamp(0);

 current_date | current_time |    current_timestamp
--------------+--------------+-------------------------
 09/07/2019   | 20:26:48+00  | 09/07/2019 20:26:48 UTC

yugabyte=# SET DateStyle = 'SQL, MDY';
SET

yugabyte=# select current_date, current_time(0), current_timestamp(0);

 current_date | current_time |    current_timestamp
--------------+--------------+-------------------------
 07/09/2019   | 20:27:04+00  | 07/09/2019 20:27:04 UTC

yugabyte=# SET DateStyle = 'German, DMY';
SET

yugabyte=# select current_date, current_time(0), current_timestamp(0);

 current_date | current_time |    current_timestamp
--------------+--------------+-------------------------
 09.07.2019   | 20:27:30+00  | 09.07.2019 20:27:30 UTC

yugabyte=# SET DateStyle = 'Postgres, DMY';
SET

yugabyte=# select current_date, current_time(0), current_timestamp(0);

 current_date | current_time |      current_timestamp
--------------+--------------+------------------------------
 09-07-2019   | 20:28:07+00  | Tue 09 Jul 20:28:07 2019 UTC

yugabyte=# SET DateStyle = 'Postgres, MDY';
SET

yugabyte=# select current_date, current_time(0), current_timestamp(0);

 current_date | current_time |      current_timestamp
--------------+--------------+------------------------------
 07-09-2019   | 20:28:38+00  | Tue Jul 09 20:28:38 2019 UTC

yugabyte=# select '01-01-2019'::date;

    date
------------
 01-01-2019

yugabyte=# select to_char('01-01-2019'::date, 'DD-MON-YYYY');

   to_char
-------------
 01-JAN-2019

yugabyte=# select to_char('05-03-2019'::date, 'DD-MON-YYYY');

   to_char
-------------
 03-MAY-2019

yugabyte=# SET DateStyle = 'Postgres, DMY';
SET

yugabyte=# select to_char('05-03-2019'::date, 'DD-MON-YYYY');

   to_char
-------------
 05-MAR-2019

yugabyte=# select to_char(to_date('05-03-2019', 'MM-DD-YYYY'), 'DD-MON-YYYY');

   to_char
-------------
 03-MAY-2019

```

Best practise is to pass all text representations of date and time data types through a `TO_DATE` or `TO_TIMESTAMP` function. There is not a 'to_time' function as its format is always fixed of 'HH24:MI:SS.ms', therefore be careful of AM/PM times and your milliseconds can also be thousandths of a second, so either 3 or 6 digits should be supplied.

The final example above illustrates the difficulty that can occur with dates. The system is expecting a 'DMY' value but your source is of format 'MDY', therefore YugabyteDB will not know how to convert it in ambiguous cases, therefore be explicit as shown.

## Getting dirty - into the logs you go

{{< note title="Note" >}}
This is for those more interested in getting into some of the more finer points of control.
{{< /note >}}

YugabyteDB has inherited a lot of similar capability of the YSQL API to the PostgreSQL SQL API, and this will explain why when you start to look under the hood, it is looking very much like pg.

YugabyteDB tracks its settings in its catalog, lets query some relevant settings and this time you will transform the layout of the query results using the `Expanded display` setting. This can be done in any database.

```
yugabyte=# \x on

Expanded display is on.

yugabyte=# select name, short_desc, coalesce(setting, reset_val) AS "setting_value", sourcefile  
          from pg_catalog.pg_settings
          where name in('log_timezone', 'log_directory', 'log_filename', 'lc_time')
          order by name asc;

-[ RECORD 1 ]-+----------------------------------------------------------------
name          | lc_time
short_desc    | Sets the locale for formatting date and time values.
setting_value | en_US.UTF-8
sourcefile    | /home/xxxxx/yugabyte-data/node-1/disk-1/pg_data/postgresql.conf
-[ RECORD 2 ]-+----------------------------------------------------------------
name          | log_directory
short_desc    | Sets the destination directory for log files.
setting_value | /home/xxxxx/yugabyte-data/node-1/disk-1/yb-data/tserver/logs
sourcefile    |
-[ RECORD 3 ]-+----------------------------------------------------------------
name          | log_filename
short_desc    | Sets the file name pattern for log files.
setting_value | postgresql-%Y-%m-%d_%H%M%S.log
sourcefile    |
-[ RECORD 4 ]-+----------------------------------------------------------------
name          | log_timezone
short_desc    | Sets the time zone to use in log messages.
setting_value | UTC
sourcefile    | /home/xxxxx/yugabyte-data/node-1/disk-1/pg_data/postgresql.conf

yugabyte=# \x off

```

Using the `log_directory` and `log_filename` references, you can find the YugabyteDB log to examine the timestamps being inserted into the logs. These are all UTC timestamps and should remain that way.

You will see that the `lc_time` setting is currently UTF and the file the setting is obtained from is listed. Opening that file **as sudo/superuser**, you will see contents that look like the below (after much scrolling or searching for 'datestyle'):

```sh

# - Locale and Formatting -

datestyle = 'iso, mdy'
#intervalstyle = 'postgres'
timezone = 'UTC'
#timezone_abbreviations = 'Default'     # Select the set of available time zone
                                        # abbreviations.  Currently, there are
                                        #   Default
                                        #   Australia (historical usage)
                                        #   India
                                        # You can create your own file in
                                        # share/timezonesets/.
#extra_float_digits = 0                 # min -15, max 3
#client_encoding = sql_ascii            # actually, defaults to database
                                        # encoding

# These settings are initialized by initdb, but they can be changed.
lc_messages = 'en_US.UTF-8'                     # locale for system error message
                                        # strings
lc_monetary = 'en_US.UTF-8'                     # locale for monetary formatting
lc_numeric = 'en_US.UTF-8'                      # locale for number formatting
lc_time = 'en_US.UTF-8'                         # locale for time formatting

# default configuration for text search
default_text_search_config = 'pg_catalog.english'

```

Make a backup of the original file and then change `datestyle = 'SQL, DMY'`, `timezone = 'GB'` (or any other timezone **name** you prefer) and save the file. You will need to restart your YugabyteDB cluster for the changes to take affect using the shell command `./bin/yb-ctl restart` (and ensure you append any startup flags if you do this).

Once the cluster is running as expected, then:

```sh
$ ./bin/ysqlsh
ysqlsh (11.2)
Type "help" for help.

yugabyte=# SHOW timezone;

 TimeZone
----------
 GB

yugabyte=# select current_date;

 current_date
--------------
 09/07/2019

```

Now you don't need to make those settings each time you enter YSQL. However, applications should not rely upon these settings, they should always `SET` their requirements before submitting their SQL. These settings should only be used by 'casual querying' such as you are doing now.

## Conclusion

As illustrated, the area of dates and times is a comprehensive area that is well addressed by PostgreSQL and hence YSQL within YugabyteDB. All of the date-time data types are implemented, and the vast majority of methods, operators and special values are available. The functionality is complex enough for you to be able to code any shortfalls that you find within the YSQL implementation of its SQL API.
