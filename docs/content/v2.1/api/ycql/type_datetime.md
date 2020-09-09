---
title: Date and time data types (DATE, TIME, and TIMESTAMP) [YCQL]
summary: DATE, TIME, and TIMESTAMP
linkTitle: DATE, TIME, and TIMESTAMP
description: Use the date and time data types (DATE, TIME, and TIMESTAMP) to specify data of date and time at a time zone.
block_indexing: true
menu:
  v2.1:
    parent: api-cassandra
    weight: 1450
isTocNested: true
showAsideToc: true
---

## Synopsis

Use datetime data types to specify data of date and time at a time zone, `DATE` for a specific day, `TIME` for time of day, and `TIMESTAMP` for the combination of both date and time.

## Syntax

```
type_specification ::= TIMESTAMP | DATE | TIME

timestamp_format ::= date_format [ time_format ] [ timezone_format ]
date_format ::= digit digit digit digit '-' digit [ digit ] '-' digit [ digit ]
time_format ::= digit [ digit ] [ ':' digit [ digit ] [ ':' digit [digit]  [ '.' digit [ digit [ digit ] ] ] ] ] 
timezone_format ::= [ 'UTC' ] ( '+' | '-' ) digit [ digit ] ':' digit [ digit ] 
```

Where

- the `timestamp_format` given above is not the timestamp literal but is used to match text literals when converting them to `TIMESTAMP` type.

## Semantics

- Columns of type `DATE`, `TIME` and `TIMESTAMP` can be part of the `PRIMARY KEY`.
- Implicitly, value of type datetime type are neither convertible nor comparable to other data types.
- Values of integer and text data types with the correct format (given above) are convertible to datetime types.
- Supported timestamp range is from year `1900` to year `9999`.
- If not specified, the default value for hour, minute, second, and millisecond components is `0`.
- If not specified, the default timezone is UTC.

## Examples

### Using the date and type types

```sql
ycqlsh:example> CREATE TABLE orders(customer_id INT, order_date DATE, order_time TIME, amount DECIMAL, PRIMARY KEY ((customer_id), order_date, order_time));
```

Date and time values can be inserted using currentdate and currenttime standard functions.

```sql
ycqlsh:example> INSERT INTO orders(customer_id, order_date, order_time, amount) VALUES (1, currentdate(), currenttime(), 85.99);
```

```sql
ycqlsh:example> INSERT INTO orders(customer_id, order_date, order_time, amount) VALUES (1, currentdate(), currenttime(), 34.15);
```

```sql
ycqlsh:example> INSERT INTO orders(customer_id, order_date, order_time, amount) VALUES (2, currentdate(), currenttime(), 55.45);
```

```sql
ycqlsh:example> SELECT * FROM orders;
```

```
 customer_id | order_date | order_time         | amount
-------------+------------+--------------------+--------
           1 | 2018-10-09 | 17:12:25.824094000 |  85.99
           1 | 2018-10-09 | 17:12:56.350031000 |  34.15
           2 | 2018-10-09 | 17:13:15.203633000 |  55.45
```

Date values can be given using date-time literals.

```sql
ycqlsh:example> SELECT sum(amount) FROM orders WHERE customer_id = 1 AND order_date = '2018-10-09';
```

```
 system.sum(amount)
--------------------
             120.14
```

### Using the timestamp type

You can do this as shown below.

```sql
ycqlsh:example> CREATE TABLE sensor_data(sensor_id INT, ts TIMESTAMP, value FLOAT, PRIMARY KEY(sensor_id, ts));
```

Timestamp values can be given using date-time literals.

```sql
ycqlsh:example> INSERT INTO sensor_data(sensor_id, ts, value) VALUES (1, '2017-07-04 12:30:30 UTC', 12.5);
```

```sql
ycqlsh:example> INSERT INTO sensor_data(sensor_id, ts, value) VALUES (1, '2017-07-04 12:31 UTC', 13.5);
```

Timestamp values can also be given as integers (milliseconds from epoch).

```sql
ycqlsh:example> INSERT INTO sensor_data(sensor_id, ts, value) VALUES (2, 1499171430000, 20);
```

```sql
ycqlsh:example> SELECT * FROM sensor_data;
```

```
 sensor_id | ts                              | value
-----------+---------------------------------+-------
         2 | 2017-07-04 12:30:30.000000+0000 |    20
         1 | 2017-07-04 12:30:30.000000+0000 |  12.5
         1 | 2017-07-04 12:31:00.000000+0000 |  13.5
```

### Supported timestamp literals

```
'1992-06-04 12:30'
'1992-6-4 12:30'
'1992-06-04 12:30+04:00'
'1992-6-4 12:30-04:30'
'1992-06-04 12:30 UTC+04:00'
'1992-6-4 12:30 UTC-04:30'
'1992-06-04 12:30.321'
'1992-6-4 12:30.12'
'1992-06-04 12:30.321+04:00'
'1992-6-4 12:30.12-04:30'
'1992-06-04 12:30.321 UTC+04:00'
'1992-6-4 12:30.12 UTC-04:30'
'1992-06-04 12:30:45'
'1992-6-4 12:30:45'
'1992-06-04 12:30:45+04:00'
'1992-6-4 12:30:45-04:30'
'1992-06-04 12:30:45 UTC+04:00'
'1992-6-4 12:30:45 UTC-04:30'
'1992-06-04 12:30:45.321'
'1992-6-4 12:30:45.12'
'1992-06-04 12:30:45.321+04:00'
'1992-6-4 12:30:45.12-04:30'
'1992-06-04 12:30:45.321 UTC+04:00'
'1992-6-4 12:30:45.12 UTC-04:30'
'1992-06-04T12:30'
'1992-6-4T12:30'
'1992-06-04T12:30+04:00'
'1992-6-4T12:30-04:30'
'1992-06-04T12:30 UTC+04:00'
'1992-6-4T12:30TUTC-04:30'
'1992-06-04T12:30.321'
'1992-6-4T12:30.12'
'1992-06-04T12:30.321+04:00'
'1992-6-4T12:30.12-04:30'
'1992-06-04T12:30.321 UTC+04:00'
'1992-6-4T12:30.12 UTC-04:30'
'1992-06-04T12:30:45'
'1992-6-4T12:30:45'
'1992-06-04T12:30:45+04:00'
'1992-6-4T12:30:45-04:30'
'1992-06-04T12:30:45 UTC+04:00'
'1992-6-4T12:30:45 UTC-04:30'
'1992-06-04T12:30:45.321'
'1992-6-4T12:30:45.12'
'1992-06-04T12:30:45.321+04:00'
'1992-6-4T12:30:45.12-04:30'
'1992-06-04T12:30:45.321 UTC+04:00'
'1992-6-4T12:30:45.12 UTC-04:30'
'1992-06-04'
'1992-6-4'
'1992-06-04+04:00'
'1992-6-4-04:30'
'1992-06-04 UTC+04:00'
'1992-6-4 UTC-04:30'
 ```

## See also

- [Date and time functions](../function_datetime)
- [Data types](..#data-types)
