---
title: Restore data for YCQL
headerTitle: Restore data
linkTitle: Restore data
description: Restore data in YugabyteDB for YCQL
aliases:
  - /manage/backup-restore/backing-up-data
block_indexing: true
menu:
  stable:
    identifier: restore-data-ycql
    parent: backup-restore
    weight: 703
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/stable/manage/backup-restore/restore-data" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/stable/manage/backup-restore/restore-data-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Restore the schema

In order to restore the schema, run the following command.

```sh
$ ycqlsh -e "source 'schema.cql'"
```

### Restoring data from a backup

To restore data from a backup, run the following command.

```sh
$ ycqlsh -e "COPY <keyspace name>.<table name> FROM 'data.csv' WITH HEADER = TRUE ;"
```

You can restore data from a backup that has a subset of columns as well.

## Options

- Connecting to a remote host and port

The default host is `127.0.0.1` and the default port is `9042`. You can override these values as shown below.

```
cqlsh -e <command> <host> [<port>]
```

- Copy options

The syntax to specify options in the `COPY FROM` command is shown below.

```sql
COPY table_name [( column_list )]
FROM 'file_name'[, 'file2_name', ...] | STDIN
[WITH option = 'value' [AND ...]]
```

There are a number of useful options in the `COPY FROM` command used to perform the backup. Some of these are outlined below.

| Option  | Description |
| --------------- | ---------------- |
| DELIMITER | Character used to separate fields, default value is `,`.  |
| HEADER    | Boolean value (`true` or `false`). If true, inserts the column names in the first row of data on exports. Default value is `false`. |
| CHUNKSIZE | The chunk size for each insert. Default value is `1000` |
| INGESTRATE | Desired ingest rate in rows per second. Must be greater than the chunk size. Default value is `100000` |

## Example

Let us restore the backup you had performed in the [example section of backing up data](/manage/backup-restore/backing-up-data/#example), 
where you had walked through how to create the `myapp_schema.cql` schema backup and the `myapp_data.csv` data backup files. 

If you have created the keyspace and the table with the data, remember to drop them using cqlsh. You can drop the table by running the following query:

```sql
ycqlsh> DROP TABLE myapp.stock_market;
```

You can drop the keyspace by running the following:

```sql
ycqlsh> DROP KEYSPACE myapp;
```


### Restore the table schema

You can import the schema from the `myapp_schema.cql` schema backup file by running the following:

```sh
$ ycqlsh -f myapp_schema.cql
```

The schema backup file `myapp_schema.cql` should look as show below:

```sh
$ cat myapp_schema.cql
```

```
CREATE KEYSPACE myapp WITH replication = {'class': 'SimpleStrategy', 'replication_factor': '3'}  AND durable_writes = true;

CREATE TABLE myapp.stock_market (
    stock_symbol text,
    ts text,
    current_price float,
    PRIMARY KEY (stock_symbol, ts)
) WITH CLUSTERING ORDER BY (ts ASC)
    AND default_time_to_live = 0;
```

You can verify that the table was created by connecting to the cluster using `ycqlsh` and running the following :

```sql
ycqlsh> DESC myapp.stock_market;
```

```
CREATE TABLE myapp.stock_market (
    stock_symbol text,
    ts text,
    current_price float,
    PRIMARY KEY (stock_symbol, ts)
) WITH CLUSTERING ORDER BY (ts ASC)
    AND default_time_to_live = 0;
```

### Restore the data

You can restore the data from the `myapp_data.csv` data backup file as follows:

```sh
$ ycqlsh -e "COPY myapp.stock_market FROM 'myapp_data.csv' WITH HEADER = TRUE ;"
```

The data backup file `myapp_data.csv` should look as follows:

```
$ cat myapp_data.csv
stock_symbol,ts,current_price
AAPL,2017-10-26 09:00:00,157.41
AAPL,2017-10-26 10:00:00,157
FB,2017-10-26 09:00:00,170.63
FB,2017-10-26 10:00:00,170.10001
GOOG,2017-10-26 09:00:00,972.56
GOOG,2017-10-26 10:00:00,971.90997
```

Note that the procedure to import data from a partial backup is identical. You can verify that the data has been restored by connecting 
to the cluster using `ycqlsh` and running the following query:

```sql
ycqlsh> SELECT * FROM myapp.stock_market;
```

```
 stock_symbol | ts                  | current_price
--------------+---------------------+---------------
         GOOG | 2017-10-26 09:00:00 |        972.56
         GOOG | 2017-10-26 10:00:00 |     971.90997
         AAPL | 2017-10-26 09:00:00 |        157.41
         AAPL | 2017-10-26 10:00:00 |           157
           FB | 2017-10-26 09:00:00 |        170.63
           FB | 2017-10-26 10:00:00 |     170.10001

(6 rows)
```
