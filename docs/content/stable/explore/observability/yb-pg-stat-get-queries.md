---
title: View terminated queries with yb_terminated_queries
linkTitle: Terminated queries
description: View terminated queries for all SQL statements executed by a server.
headerTitle: View terminated queries with yb_terminated_queries
headcontent: See why a query failed
aliases:
  - /explore/query-1-performance/yb-pg-stat-get-queries/
menu:
  stable:
    identifier: yb-terminated-queries
    parent: explore-observability
    weight: 350
type: docs
---

Use the YugabyteDB `yb_terminated_queries` view to see terminated queries and the reason for their termination.

When a query quits for unexpected reasons, information about the query and the responsible backend is stored. You can access this information by using `yb_terminated_queries` view which is built on top of the `yb_pg_stat_get_queries` function. The view returns queries using the following criteria:

- Temporary file size exceeds `temp_file_limit`.
- Terminated by SIGSEGV - the query terminated due to a crash in the PostgreSQL process.
- Terminated by SIGKILL - the query was killed by the system's out of memory killer because the node is running out of memory.

{{% explore-setup-single-new %}}

## Supported fields

At a ysqlsh prompt, run the following meta-commands to return the fields supported by `yb_terminated_queries`:

```sql
yugabyte=# \x
yugabyte=# \d yb_terminated_queries
```

```output
                    View "pg_catalog.yb_terminated_queries"
       Column       |           Type           | Collation | Nullable | Default
--------------------+--------------------------+-----------+----------+---------
 databasename       | name                     |           |          |
 backend_pid        | integer                  |           |          |
 query_text         | text                     |           |          |
 termination_reason | text                     |           |          |
 query_start_time   | timestamp with time zone |           |          |
 query_end_time     | timestamp with time zone |           |          |
```

The following table describes the fields and their values:

| Field | Type | Description |
| :---- | :--- | :---------- |
| databasename | Name | Name of the database to which the backend was connected when the query was terminated. |
| backend_pid | Integer | Backend process ID. |
| query_text | Text | The query that was executed, up to a maximum of 1,023 bytes. The captured text can be shorter if `track_activity_query_size` is lower. |
| termination_reason | Text | An explanation of why the query was terminated. One of:<br/>Terminated by SIGKILL<br/>Terminated by SIGSEGV<br/>temporary file size exceeds temp_file_limit (xxx kB)
| query_start_time | Timestampz | Time at which the query started. |
| query_end_time | Timestampz | Time at which the query was terminated. |

## Examples

### PostgreSQL crash

To simulate a crash in the PostgreSQL process, send a SIGSEGV signal to the backend process.

In a ysqlsh session, get the backend `pid`:

```sql
yugabyte=# SELECT pg_backend_pid();
```

```output
 pg_backend_pid
----------------
           4650
(1 row)
```

In the same session, start a long-running query, so that you have time to send a signal while the query is running:

```sql
yugabyte=# SELECT * FROM generate_series(1, 123456789);
```

In another session, send the terminating signal to the backend process:

```sh
$ kill -SIGSEGV 4650 # the pid of the backend process
```

Verify that the query is listed as a terminated query as follows:

```sql
yugabyte=# SELECT backend_pid, query_text, termination_reason FROM yb_terminated_queries;
```

```output
 backend_pid |                  query_text                  |  termination_reason
-------------+----------------------------------------------+-----------------------
        4650 | SELECT * FROM generate_series(1, 123456789); | Terminated by SIGSEGV
(1 row)
```

### Exceed the temporary file limit

To simulate a query termination, set `temp_file_limit` to 0KB:

```sql
yugabyte=# SET temp_file_limit TO 0;
```

Now any query that requires a temporary file will result in an error.

To ensure failure, run a query that generates hundreds of millions of rows as follows:

```sql
yugabyte=# SELECT * FROM generate_series(1, 123456789);
```

```output
ERROR:  temporary file size exceeds temp_file_limit (0kB)
```

To find the query in `yb_terminated_queries`, enter the following command:

```sql
yugabyte=# SELECT backend_pid, query_text, termination_reason FROM yb_terminated_queries;
```

```output
 backend_pid |                  query_text                  |                termination_reason
-------------+----------------------------------------------+---------------------------------------------------
       23052 | SELECT * FROM generate_series(1, 123456789); | temporary file size exceeds temp_file_limit (0kB)
(1 row)
```

### Out of memory

When a system is running critically low on memory, the out of memory killer will begin force killing processes. To simulate this, send a KILL signal to the backend process.

In a ysqlsh session, get the backend PID:

```sql
yugabyte=# SELECT pg_backend_pid();
```

```output
 pg_backend_pid
----------------
           4801
(1 row)
```

In the same session, start a long-running query so that you have time to send a signal while the query is running:

```sql
yugabyte=# SELECT * FROM generate_series(1, 123456789);
```

In another session, send the terminating signal to the backend process:

```sh
$ kill -KILL 4801 # the pid of the backend process
```

Verify that the query is listed as a terminated query as follows:

```sql
yugabyte=# SELECT backend_pid, query_text, termination_reason FROM yb_terminated_queries;
```

```output
 backend_pid |                  query_text                  |  termination_reason
-------------+----------------------------------------------+-----------------------
        4801 | SELECT * FROM generate_series(1, 123456789); | Terminated by SIGKILL
(1 row)
```

### Return a query from another database

Create a terminated query by running the following command:

```sql
yugabyte=# SET temp_file_limit TO 0;
yugabyte=# SELECT 'db1' FROM generate_series(1, 123456789);
```

Create a second database and connect to it as follows:

```sql
yugabyte=# CREATE DATABASE new_db;
yugabyte=# \c new_db;
```

Create a second terminated query by running the following command:

```sql
new_db=# SET temp_file_limit TO 0;
new_db=# SELECT 'db2' FROM generate_series(1, 123456789);
```

Querying the `yb_terminated_queries` view without providing a database name returns both queries:

```sql
new_db=# SELECT query_text FROM yb_terminated_queries;
```

```output
                    query_text
--------------------------------------------------
 SELECT 'db1' FROM generate_series(1, 123456789);
 SELECT 'db2' FROM generate_series(1, 123456789);
(2 rows)
```

When you query `yb_terminated_queries` with the name of the current database, you only see the entries for that database.

Use the database name to get the terminated queries from the `yugabyte` database as follows:

```sql
new_db=# SELECT query_text FROM yb_terminated_queries WHERE databasename='yugabyte';
```

```output
                    query_text
--------------------------------------------------
 SELECT 'db1' FROM generate_series(1, 123456789);
(1 row)
```

Use the database name to get the terminated queries from the new database as follows:

```sql
new_db=# SELECT query_text FROM yb_terminated_queries WHERE databasename='new_db';
```

```output
                    query_text
--------------------------------------------------
 SELECT 'db2' FROM generate_series(1, 123456789);
(1 row)
```

## Limitations

- The underlying data returned by the query is refreshed at 500 ms intervals, so if a recently terminated query is not listed, try querying again.
- The backend holds up to 1000 failed queries before it starts to overwrite the first queries.
- If the stat collector process is abruptly terminated, the underlying data may be corrupted and invalid.

## Learn more

- Refer to [Cluster-wide database views](../cluster-wide-db-views/) to query terminated queries across every live YB-TServer from a single session (`gv$yb_terminated_queries`).
- For information on the temporary file limit, refer to [temp_file_limit](../../../reference/configuration/yb-tserver/#temp-file-limit) YB-TServer flag.
- Refer to [Get query statistics using pg_stat_statements](../../../launch-and-manage/monitor-and-alert/query-tuning/pg-stat-statements/) to track planning and execution of all the SQL statements.
- Refer to [View live queries with pg_stat_activity](../pg-stat-activity/) to analyze live queries.
