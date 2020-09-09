---
title: Prepare a cluster
headerTitle: Prepare a cluster
linkTitle: Prepare a cluster
description: Prepare a YugabyteDB cluster for importing PostgreSQL data.
menu:
  latest:
    identifier: migrate-postgresql-prepare-cluster
    parent: migrate-from-postgresql
    weight: 760
isTocNested: false
showAsideToc: true
---

This section outlines some of the important considerations before loading data into the cluster.

## Separate DDL schema from data

It is recommended to run the DDL schema generation first before loading the data exports. This is essential to ensure that the tables are properly created ahead of starting to use them.

## Order data by primary key

The data should be ordered by the primary key when it is being imported if possible. Importing a data set that is ordered by the primary key is typically much faster because the data being loaded will all get written to a node as a larger batch of rows, as opposed to writing a few rows across multiple nodes.

## Multiple parallel imports

It is more efficient if the source data being imported is split into multiple files, so that these files can be imported in parallel across the nodes of the cluster. This can be done by running multiple `COPY` commands in parallel. For example, a large CSV data file should be split into multiple smaller CSV files.

## Programmatic batch inserts

Below are some recommendations when performing batch loads of data programmatically.

1. Use multi-row inserts to do the batching. This would require setting certain properties based on the driver being used. For example, with the JDBC driver with Java, set the following property to use multi-row inserts: reWriteBatchedInserts=true

2. Use a batch size of 128 when using multi-row batch inserts.

3. Use the `PREPARE` - `BIND` - `EXECUTE` paradigm instead of inlining literals to avoid statement reparsing overhead.

4. To ensure optimal utilization of all nodes across the cluster by balancing the load, uniformly distribute the SQL statements across all nodes in the cluster.

5. It may be necessary to increase the parallelism of the load in certain scenarios. For example, in the case of a loader using a single thread to load data, it may not be possible to utilize a large cluster optimally. In these cases, it may be necessary to increase the number of threads or run multiple loaders in parallel.

6. Note that `INSERT .. ON CONFLICT` statements are not yet fully optimized as of YugabyteDB v2.2, so it is recommended to use simple INSERT statements if possible

## Indexes during data load

As of YugabyteDB v2.2, it is recommended to create indexes before loading the data.

{{< note title="Note" >}}
This recommendation is subject to change in the near future with the introduction of online index rebuilds, which enables creating indexes after loading all the data.
{{< /note >}}

## Disable constraints and triggers temporarily

While loading data that is exported from another RDBMS, the source data set may not necessarily need to be checked for relational integrity since this was already performed when inserting into the source database. In such cases, disable checks such as FOREIGN KEY constraints, as well as triggers if possible. This would reduce the number of steps the database needs to perform while inserting data, which would speed up data loading.

