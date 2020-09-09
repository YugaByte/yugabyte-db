---
title: KairosDB
linkTitle: KairosDB
description: KairosDB
aliases:
  - /develop/ecosystem-integrations/kairosdb/
block_indexing: true
menu:
  stable:
    identifier: kairosdb
    parent: ecosystem-integrations
    weight: 574
isTocNested: true
showAsideToc: true
---

[KairosDB](http://kairosdb.github.io/) is a Java-based time-series metrics API that leverages Cassandra as it's underlying distributed database. This page shows how it can be integrated with YugabyteDB's Cassandra-compatible YCQL API.

## 1. Start local cluster

Follow [Quick start](../../../quick-start/) to run a local YugabyteDB cluster and test YugabyteDB's YCQL API to confirm that you have a YCQL service running on `localhost:9042`.

## 2. Download KairosDB

Download KairosDB as stated below. Latest releases are available in [releases](https://github.com/kairosdb/kairosdb/releases) of the KairosDB repository.

```sh
$ wget https://github.com/kairosdb/kairosdb/releases/download/v1.2.0-beta3/kairosdb-1.2.0-0.3beta.tar.gz
$ tar xvfz kairosdb-1.2.0-0.3beta.tar.gz
$ cd kairosdb/
```

You can follow the [Getting started](http://kairosdb.github.io/docs/build/html/GettingStarted.html) to see how to configure KairosDB in general. For the purpose of integrating with the local YugabyteDB cluster running at `localhost:9042`, simply open `conf/kairosdb.properties` and comment out the default in-memory datastore as below.

```
#kairosdb.service.datastore=org.kairosdb.datastore.h2.H2Module
```

Uncomment the following line to make Cassandra the datastore.

```
kairosdb.service.datastore=org.kairosdb.datastore.cassandra.CassandraModule
```

## 3. Start KairosDB

```sh
$ ./bin/kairosdb.sh run
```

You should see the following lines if KairosDB starts up successfully.

```
18:34:01.094 [main] INFO  [AbstractConnector.java:338] - Started SelectChannelConnector@0.0.0.0:8080
18:34:01.098 [main] INFO  [Main.java:522] - Starting service class org.kairosdb.core.telnet.TelnetServer
18:34:01.144 [main] INFO  [Main.java:378] - ------------------------------------------
18:34:01.144 [main] INFO  [Main.java:379] -      KairosDB service started
18:34:01.145 [main] INFO  [Main.java:380] - ------------------------------------------
```

## 4. Verify Cassandra integration with ycqlsh

Run `ycqlsh` to connect and use YugabyteDB's YCQL API.

Assuming you are using the macOS or Linux binary

```sh
$ ./bin/ycqlsh localhost
```

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh>
```

- Run a cql command to verify it is working.

```sql
ycqlsh> describe keyspaces;
```

```
kairosdb  system_schema  system_auth  system
```

```sql
ycqlsh> use kairosdb;
ycqlsh:kairosdb> describe tables;
```

```
row_keys       data_points    string_index
row_key_index  service_index  row_key_time_index
```

## 5. Test KairosDB

### Push data

Push metric data into KairosDB as per the instructions [here](http://kairosdb.github.io/docs/build/html/PushingData.html).

### Query data

Query metric data into KairosDB as per the instructions [here](http://kairosdb.github.io/docs/build/html/QueryingData.html).
