---
title: Built-in function call [YCQL]
headerTitle: Built-in function call
linkTitle: Function call
description: Use a function call expression to apply the specified function to to given arguments between parentheses and return the result of the computation.
menu:
  latest:
    parent: api-cassandra
    weight: 1350
aliases:
  - /latest/api/cassandra/expr_fcall
  - /latest/api/ycql/expr_fcall
isTocNested: true
showAsideToc: true
---

## Synopsis

Use a function call expression to apply the specified function to to given arguments between parentheses and return the result of the computation.

## Syntax

```
function_call ::= function_name '(' [ arguments ... ] ')'
```

## Builtin Functions

| Function | Return Type | Argument Type | Description |
|----------|-------------|---------------|-------------|
| BlobAs\<Type> | \<Type> | ([`BLOB`](../type_blob)) | Converts a value from `BLOB` |
| \<Type>AsBlob | [`BLOB`](../type_blob) | (\<Type>) | Converts a value to `BLOB` |
| [DateOf](../function_datetime/#dateof) | [`TIMESTAMP`](../type_datetime) | ([`TIMEUUID`](../type_uuid)) | Conversion |
| MaxTimeUuid | [`TIMEUUID`](../type_uuid) | ([`TIMESTAMP`](../type_datetime)) | Returns the associated max time uuid  |
| MinTimeUuid | [`TIMEUUID`](../type_uuid) | ([`TIMESTAMP`](../type_datetime)) | Returns the associated min time uuid  |
| [CurrentDate](../function_datetime/#currentdate-currenttime-and-currenttimestamp) | [`DATE`](../type_datetime) | () | Return the system current date |
| [CurrentTime](../function_datetime/#currentdate-currenttime-and-currenttimestamp) | [`TIME`](../type_datetime) | () | Return the system current time of day |
| [CurrentTimestamp](../function_datetime/#currentdate-currenttime-and-currenttimestamp) | [`TIMESTAMP`](../type_datetime) | () | Return the system current timestamp |
| [Now](../function_datetime/#now) | [`TIMEUUID`](../type_uuid) | () | Returns the UUID of the current timestamp |
| [TTL](#ttl-function) | [`BIGINT`](../type_int) | (\<AnyType>) | Get time-to-live of a column |
| [ToDate](../function_datetime/#todate) | [`DATE`](../type_datetime) | ([`TIMESTAMP`](../type_datetime)) | Conversion |
| [ToDate](../function_datetime/#todate) | [`DATE`](../type_datetime) | ([`TIMEUUID`](../type_uuid)) | Conversion |
| ToTime | [`TIME`](../type_datetime) | ([`TIMESTAMP`](../type_datetime))  | Conversion |
| ToTime | [`TIME`](../type_datetime) | ([`TIMEUUID`](../type_uuid) | Conversion |
| [ToTimestamp](../function_datetime/#totimestamp) | ([`TIMESTAMP`](../type_datetime))  | ([`DATE`](../type_datetime)) | Conversion |
| [ToTimestamp](../function_datetime/#totimestamp) | ([`TIMESTAMP`](../type_datetime)) | (`TIMEUUID`) | Conversion |
| [ToUnixTimestamp](../function_datetime/#tounixtimestamp) | [`BIGINT`](../type_int) | ([`DATE`](../type_datetime)) | Conversion |
| [ToUnixTimestamp](../function_datetime/#tounixtimestamp) | [`BIGINT`](../type_int) | ([`TIMESTAMP`](../type_datetime))  | Conversion |
| [ToUnixTimestamp](../function_datetime/#tounixtimestamp) | [`BIGINT`](../type_int) | ([`TIMEUUID`](../type_uuid)) | Conversion |
| [UnixTimestampOf](../function_datetime/#unixtimestampof) | [`BIGINT`](../type_int) | ([`TIMEUUID`](../type_uuid)) | Conversion |
| [UUID](../function_datetime/#uuid) | [`UUID`](../type_uuid) | () | Returns a version 4 UUID |
| [WriteTime](#writetime-function) | [`BIGINT`](../type_int) | (\<AnyType>) | Returns the timestamp when the column was written |
| [partition_hash](#partition-hash-function) | [`BIGINT`](../type_int) | () | Computes the partition hash value (uint16) for the partition key columns of a row |

## Aggregate Functions

| Function | Description |
|----------|-------------|
| COUNT | Returns number of selected rows |
| SUM | Returns sums of column values |
| AVG | Returns the average of column values |
| MIN | Returns the minimum value of column values |
| MAX | Returns the maximum value of column values |

## Semantics

<li>The argument data types must be convertible to the expected type for that argument that was specified by the function definition.</li>
<li>Function execution will return a value of the specified type by the function definition.</li>
<li>YugabyteDB allows function calls to be used any where that expression is allowed.</li>

## Cast function

```
cast_call ::= CAST '(' column AS type ')'
```

CAST function converts the value returned from a table column to the specified data type.

| Source Column Type | Target Data Type |
|--------------------|------------------|
| `BIGINT` | `SMALLINT`, `INT`, `TEXT` |
| `BOOLEAN` | `TEXT` |
| `DATE` | `TEXT`, `TIMESTAMP` |
| `DOUBLE` | `BIGINT`, `INT`, `SMALLINT`, `TEXT` |
| `FLOAT` | `BIGINT`, `INT`, `SMALLINT`, `TEXT` |
| `INT` | `BIGINT`, `SMALLINT`, `TEXT` |
| `SMALLINT` | `BIGINT`, `INT`, `TEXT` |
| `TIME` | `TEXT` |
| `TIMESTAMP` | `DATE`, `TEXT` |
| `TIMEUUID` | `DATE`, `TIMESTAMP` |


## partition_hash function
`partition_hash` is a function that takes as arguments the partition key columns of the primary key of a row and 
returns a `uint16` hash value representing the hash value for the row used for partitioning the table.
The hash values used for partitioning fall in the `0-65535` (uint16) range. 
Tables are partitioned into tablets, with each tablet being responsible for a range of partition values. 
The `partition_hash` of the row is used to decide which tablet the row will reside in.

`partition_hash` can be handy for querying a subset of the data to get approximate row counts or to breakdown 
full-table operations into smaller sub-tasks that can be run in parallel.

### Querying a subset of the data
One use of `partition_hash` is to query a subset of the data and get approximate count of rows in the table.
For example, suppose you have a table `t` with partitioning columns `(h1,h2)`:

```sql
create table t (h1 int, h2 int, r1 int, r2 int, v int, 
                         primary key ((h1, h2), r1, r2));
```
We can use this function to query a subset of the data (in this case, 1/128 of the data):
```sql
select count(*) from t where partition_hash(h1, h2) >= 0 and
                                      partition_hash(h1, h2) < 512;
```
The value `512` comes from dividing the full hash partition range by the number of subsets that you want to query (`65536/128=512`).

### Parallel full table scans

To do a distributed scan, you can issue, in this case, 128 queries each using a different hash range:

```sql
.. where partition_hash(h1, h2) >= 0 and partition_hash(h1, h2) < 512;
```

```sql
.. where partition_hash(h1, h2) >= 512 and partition_hash(h1, h2) <1024 ;
```

and so on, till the last segment/range of `512` in the partition space:

```sql
.. where partition_hash(h1, h2) >= 65024;
```

Here is a full implementation of a parallel table scan using `partition_hash` in [Python 3](https://github.com/yugabyte/yb-tools/blob/main/ycql_table_row_count.py) and [Go](https://github.com/yugabyte/yb-tools/tree/main/ycrc).

## WriteTime function

The `WriteTime` function returns the timestamp in microseconds when a column was written.
For example, suppose you have a table `page_views` with a column named `views`:

```sql
 SELECT writetime(views) FROM page_views;

 writetime(views)
------------------
 1572882871160113

(1 rows)
```

## TTL function

The TTL function returns the number of seconds until a column or row expires. 
Assuming you have a table `page_views` and a column named `views`:

```sql
SELECT TTL(views) FROM page_views;

 ttl(views)
------------
      86367

(1 rows)
```

## Examples

```sql
ycqlsh:example> CREATE TABLE test_cast (k INT PRIMARY KEY, ts TIMESTAMP);
```

```sql
ycqlsh:example> INSERT INTO test_cast (k, ts) VALUES (1, '2018-10-09 12:00:00');
```

```sql
ycqlsh:example> SELECT CAST(ts AS DATE) FROM test_cast;
```

```
 cast(ts as date)
------------------
       2018-10-09
```

## See also

- [All Expressions](..##expressions)
