---
title: Bulk import for YCQL
headerTitle: Bulk import
linkTitle: Bulk import
description: Export data from Apache Cassandra and MySQL and bulk import data into YugabyteDB for YCQL.
aliases:
  - /latest/manage/data-migration/bulk-import/
menu:
  latest:
    identifier: manage-bulk-import
    parent: manage-bulk-import-export
    weight: 704
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

Depending on the data volume imported, various bulk import tools can be used to load data into YugabyteDB. This page documents bulk import for YugabyteDB’s [Cassandra-compatible YCQL API](../../../../api/ycql).

We will first export data from existing Apache Cassandra and MySQL tables. Thereafter, we will import the data using the various bulk load options supported by YugabyteDB. We will use a generic IoT time series data use case as a running example to illustrate the import process.

## Create Destination Table

Following is the schema of the destination YugabyteDB table.

```sql
CREATE KEYSPACE example;
USE EXAMPLE;

CREATE TABLE SensorData (
  customer_name text,
  device_id int,
  ts timestamp,
  sensor_data map<text, double>,
  PRIMARY KEY((customer_name, device_id), ts)
);
```

## Prepare Source Data

We will prepare a csv (comma-separated values) file where each row of entries must match with the column types declared in the table schema above. Concretely, each comma-separated value must be a valid Cassandra Query Language (CQL) literal for its corresponding type, except the top-level quotes are not needed (e.g. foo rather than 'foo' for strings). 

### Generate Sample Data

If you do not have the data already available in a database table, you can create sample data for the import using the instructions below.

```sh
#!/bin/bash

# Usage: ./generate_data.sh <number_of_rows> <output_filename>
# Example ./generate_data.sh 1000 sample.csv

if [ "$#" -ne 2 ]
then
  echo "Usage: ./generate_data.sh <number_of_rows> <output_filename>"
  Echo "Example ./generate_data.sh 1000 sample.csv"
  exit 1
fi

> $2 # clearing file
for i in `seq 1 $1`
do 
  echo customer$((i%10)),$((i%3)),2017-11-11 12:30:$((i%60)).000000+0000,\"{temp:$i, humidity:$i}\" >> $2
done
```
```
customer1,1,2017-11-11 12:32:1.000000+0000,"{temp:1, humidity:1}"
customer2,2,2017-11-11 12:32:2.000000+0000,"{temp:2, humidity:2}"
customer3,0,2017-11-11 12:32:3.000000+0000,"{temp:3, humidity:3}"
customer4,1,2017-11-11 12:32:4.000000+0000,"{temp:4, humidity:4}"
customer5,2,2017-11-11 12:32:5.000000+0000,"{temp:5, humidity:5}"
customer6,0,2017-11-11 12:32:6.000000+0000,"{temp:6, humidity:6}"
```

### Export from Apache Cassandra

If you already had the data in an Apache Cassandra table, then use the following command to create a csv file with the data.

```sql
ycqlsh> COPY example.SensorData TO '/path/to/sample.csv';
```

### Export from MySQL

If you already had the data in a MySQL table named `SensorData`, then use the following command to create a csv file with the data.

```sql
SELECT customer_name, device_id, ts, sensor_data
FROM SensorData 
INTO OUTFILE '/path/to/sample.csv' FIELDS TERMINATED BY ',';
```

## Import data

These instructions are organized by the size of the input datasets, ranging from small (MBs of data) to larger datasets (GBs of data). 

### Small datasets (MBs)

Cassandra’s CQL Shell provides the COPY FROM (see also COPY TO) command which allows importing data from csv files. 

```sql
ycqlsh> COPY example.SensorData FROM '/path/to/sample.csv';
```

{{< note title="Note" >}}

By default, `COPY` exports timestamps in `yyyy-MM-dd HH:mm:ss.SSSZ` format. 

{{< /note >}}

### Large datasets (GBs)

[`cassandra-loader`](https://github.com/brianmhess/cassandra-loader) is a general purpose bulk loader for CQL that supports various types of delimited files (particularly CSV files). For more details, review the README of the [YugabyteDB cassandra-loader fork](https://github.com/yugabyte/cassandra-loader/). Note that cassandra-loader requires quotes for collection types (for example, “[1,2,3]” rather than [1,2,3] for lists).

#### Install cassandra-loader

You can do this as shown below.

```sh
$ wget https://github.com/yugabyte/cassandra-loader/releases/download/v0.0.27-yb-2/cassandra-loader
```

```sh
$ chmod a+x cassandra-loader
```

#### Run cassandra-loader

```sh
time ./cassandra-loader \
  -dateFormat 'yyyy-MM-dd HH:mm:ss.SSSSSSX' \
  -f sample.csv \
  -host <clusterNodeIP> \
  -schema "example.SensorData(customer_name, device_id, ts, sensor_data)"
```

For additional options, refer to the [cassandra-loader options](https://github.com/yugabyte/cassandra-loader#options).
