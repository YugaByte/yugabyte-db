---
title: Date and time data types (DATE, TIME, and TIMESTAMP) [YCQL]
headerTitle: Date and time data types (DATE, TIME, and TIMESTAMP)
linkTitle: DATE, TIME, and TIMESTAMP
description: Use the date and time data types (DATE, TIME, and TIMESTAMP) to specify dates and time.
block_indexing: true
menu:
  stable:
    parent: api-cassandra
    weight: 1450
aliases:
  - /stable/api/cassandra/type_datetime
  - /stable/api/cassandra/type_timestamp
  - /stable/api/ycql/type_datetime
  - /stable/api/ycql/type_timestamp
isTocNested: true
showAsideToc: true
---

## Synopsis

Use datetime data types to specify data of date and time at a time zone, `DATE` for a specific date, `TIME` for time of day, and `TIMESTAMP` for the combination of both date and time.

## Syntax

```
type_specification ::= TIMESTAMP | DATE | TIME
```

## Semantics

- Columns of type `DATE`, `TIME` and `TIMESTAMP` can be part of the `PRIMARY KEY`.
- Implicitly, values of datetime types cannot be converted or compared to other data types.
- Values of integer and text data types with the correct format (given above) are convertible to datetime types.
- Supported timestamp range is from year `1900` to year `9999`.
- The default value for hour, minute, second, and millisecond components is `0`.
- The default time zone is `UTC`.

### DATE

A date is represented using a 32-bit unsigned integer representing the number of days since epoch (January 1, 1970) with no corresponding time value.
Use [INSERT](../dml_insert) or [UPDATE](../dml_update) to add values as an integer (days since epoch) or in the string format shown below.

#### Syntax
```
yyyy-mm-dd
```

- `yyyy`: four digit year.
- `mm`: two digit month.
- `dd`: two digit day.

For example, `2020-07-29`.

### TIME

Values of the `time` data type are encoded as 64-bit signed integers representing the number of nanoseconds since midnight with no corresponding date value.

Use [INSERT](../dml_insert) or [UPDATE](../dml_update) to add values in the following string format, where subseconds (`f`) are optional and if provided, can be less than nanosecond:

#### Syntax
```
hh:mm:ss[.fffffffff]
```

- `hh`: two digit hour, using a 24-hour clock.
- `mm`: two digit minutes.
- `ss`: two digit seconds.
- `fffffffff`: (Optional) three digit sub-seconds, or nanoseconds. When excluded, set to `0`.

For example, `12:34:56` or `12:34:56.789` or `12:34:56.123456789`.

### TIMESTAMP

Values of the `timestamp` data type combines date, time, and time zone, in ISO 8601 format.

Use [INSERT](../dml_insert) or [UPDATE](../dml_update) to add values in the string format shown below, where milliseconds (`f`) are optional.

#### Syntax
```
yyyy-mm-dd[ (T| )HH:MM[:SS][.fff]][(+|-)NNNN]
```

Required date (`yyyy-mm-dd`) where:

- `yyyy`: four digit year.
- `mm`: two digit month.
- `dd`: two digit day.

Optional time (HH:MM[:SS][.fff]) where:

- `HH`: two digit hour, using a 24-hour clock.
- `MM`: two digit minutes.
- `SS`: (Optional) two digit seconds.
- `fff`: (Optional) three digit sub-seconds, or milliseconds. When excluded, set to `0`.

Optional time zone (`(+|-)NNNN`) where:

- `+|-`: Add or subtract the NNNN from GMT
- `NNNN`: The 4-digit time zone (RFC 822). For example, `+0000` is GMT and `-0800` is PST.

NNNN is the RFC-822 4-digit time zone, for example +0000 is GMT and -0800 is PST.

For example, for July 29, 2020 midnight PST, valid timestamp values include `2020-07-29 12:34:56.789+0000`, `2020-07-29 12:34:56.789`, `2020-07-29 12:34:56`, and `2020-07-29`.

## Examples

### Using the date and time types

```sql
ycqlsh:example> CREATE TABLE orders(customer_id INT, order_date DATE, order_time TIME, amount DECIMAL, PRIMARY KEY ((customer_id), order_date, order_time));
```

Date and time values can be inserted using `currentdate` and `currenttime` standard functions.

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
