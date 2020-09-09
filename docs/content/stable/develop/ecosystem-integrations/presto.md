---
title: Presto
linkTitle: Presto
description: Presto
aliases:
  - /develop/ecosystem-integrations/presto/
block_indexing: true
menu:
  stable:
    identifier: presto
    parent: ecosystem-integrations
    weight: 575
isTocNested: true
showAsideToc: true
---

[Presto](https://prestosql.io/) is a distributed SQL query engine optimized for ad-hoc analysis at interactive speed. It supports standard ANSI SQL, including complex queries, aggregations, joins, and window functions. It has a connector architecture to query data from many data sources.
This page shows how Presto can be setup to query YugabyteDB's YCQL tables.

## 1. Start local cluster

Follow [Quick start](../../../quick-start/) instructions to run a local YugabyteDB cluster. Test YugabyteDB's Cassandra compatible API as [documented](../../../quick-start/test-cassandra/) so that you can confirm that you have a Cassandra compatible service running on `localhost:9042`. We assume you have created the keyspace and table, and inserted sample data as described there.

## 2. Download and configure Presto

Detailed steps are documented [here](https://prestosql.io/docs/current/installation/deployment.html).
The following are the minimal setup steps for getting started.

```sh
$ wget https://repo1.maven.org/maven2/io/prestosql/presto-server/309/presto-server-309.tar.gz
```

```sh
$ tar xvf presto-server-309.tar.gz
```

```sh
$ cd presto-server-309
```

### Create the “etc”, “etc/catalog”, and “data” directory inside the installation directory

```sh
$ mkdir etc
```

```sh
$ mkdir etc/catalog
```

```sh
$ mkdir data
```

### Create node.properties file - replace &lt;username&gt; below

```sh
$ cat > etc/node.properties
```

```
node.environment=test
node.id=ffffffff-ffff-ffff-ffff-ffffffffffff
node.data-dir=/Users/<username>/presto-server-309/data
```

### Create jvm.config file

```sh
$ cat > etc/jvm.config
```

```
-server
-Xmx6G
-XX:+UseG1GC
-XX:G1HeapRegionSize=32M
-XX:+UseGCOverheadLimit
-XX:+ExplicitGCInvokesConcurrent
-XX:+HeapDumpOnOutOfMemoryError
-XX:+ExitOnOutOfMemoryError
```

### Create config.properties file

```sh
$ cat > etc/config.properties
```

```
coordinator=true
node-scheduler.include-coordinator=true
http-server.http.port=8080
query.max-memory=4GB
query.max-memory-per-node=1GB
discovery-server.enabled=true
discovery.uri=http://localhost:8080
```

### Create log.properties file

```sh
$ cat > etc/log.properties
```

```
io.prestosql=INFO
```

### Configure Cassandra connector to YugabyteDB

Create the cassandra catalog properties file in etc/catalog directory.
Detailed instructions are [here](https://prestosql.io/docs/current/connector/cassandra.html).

```sh
$ cat > etc/catalog/cassandra.properties
```

```
connector.name=cassandra
cassandra.contact-points=127.0.0.1
```

## 3. Download Presto CLI

```sh
$ cd ~/presto-server-309/bin
```

```sh
$ wget https://repo1.maven.org/maven2/io/prestosql/presto-cli/309/presto-cli-309-executable.jar
```

Rename jar to ‘presto’. It is meant to be a self-running binary.

```sh
$ mv presto-cli-309-executable.jar presto && chmod +x presto
```

## 4. Launch Presto server

```sh
$ cd presto-server-309
```

To run in foreground mode.

```sh
$ ./bin/launcher run       
```

To run in background mode.

```sh
$ ./bin/launcher start  
```

## 5. Test Presto queries

Use the presto CLI to run ad-hoc queries.

```sh
$ ./bin/presto --server localhost:8080 --catalog cassandra --schema default
```

Start using `myapp`.

```sql
presto:default> use myapp;
```

```sh
USE
```

Show the tables available.

```sql
presto:myapp> show tables;
```

```
 Table
-------
 stock_market
(1 row)
```

Describe a particular table.

```sql
presto:myapp> describe stock_market;
```

```
    Column     |  Type   | Extra | Comment 
---------------+---------+-------+---------
 stock_symbol  | varchar |       |         
 ts            | varchar |       |         
 current_price | real    |       |         
(3 rows)
```

### Query with filter

```sql
presto:myapp> select * from stock_market where stock_symbol = 'AAPL';
```

```
 stock_symbol |         ts          | current_price 
--------------+---------------------+---------------
 AAPL         | 2017-10-26 09:00:00 |        157.41 
 AAPL         | 2017-10-26 10:00:00 |         157.0 
(2 rows)
```

### Query with aggregates

```sql
presto:myapp> select stock_symbol, avg(current_price) from stock_market group by stock_symbol;
```

```
 stock_symbol |  _col1  
--------------+---------
 GOOG         | 972.235 
 AAPL         | 157.205 
 FB           | 170.365 
(3 rows)
```
