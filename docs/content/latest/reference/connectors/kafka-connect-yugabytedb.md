---
title: Kafka Connect YugabyteDB
headerTitle: Kafka Connect YugabyteDB
linkTitle: Kafka Connect YugabyteDB
description: Use Kafka Connect YugabyteDB to stream YugabyteDB table updates to Kafka topics.
section: REFERENCE
menu:
  latest:
    identifier: kafka-connect-yugabytedb
    parent: connectors
    weight: 2930
isTocNested: true
showAsideToc: true
---

There are two approaches of integrating [YugabyteDB](https://github.com/yugabyte/yugabyte-db) with Apache Kafka. Kafka provides [Kafka Connect](https://docs.confluent.io/3.0.0/connect/intro.html), a connector SDK for building such integrations.

<img src="https://raw.githubusercontent.com/yugabyte/yb-kafka-connector/master/logos/dsql-kafka.png" align="center" alt="Kafka Connect YugabyteDB Connector Architecture"/>

## Kafka Connect YugabyteDB Source Connector

The Kafka Connect YugabyteDB source connector streams table updates in YugabyteDB to Kafka topics. It is based on YugabyteDB's Change Data Capture (CDC) feature. CDC allows the connector to simply subscribe to these table changes and then publish the changes to selected Kafka topics.

For an example of the source connector in action, see [CDC to Kafka](../../../deploy/cdc/cdc-to-kafka/) page.

## Kafka Connect YugabyteDB Sink Connector

The Kafka Connect YugabyteDB Sink Connector delivers data from Kafka topics into YugabyteDB tables. The connector subscribes to specific topics in Kafka and then writes to specific tables in YugabyteDB as soon as new messages are received in the selected topics.

### Prerequisites

For building and using this project, the following tools must be installed on your system.

- JDK 1.8 or later.
- Maven 3.3 or later.
- Clone this repository into `~/yb-kafka/yb-kafka-connector/` directory.

### Setup and use

1. Set up and start Kafka.

Download the Apache Kafka `tar` file.

    ```sh
    mkdir -p ~/yb-kafka
    cd ~/yb-kafka
    wget http://apache.cs.utah.edu/kafka/2.5.0/kafka_2.12-2.5.0.tgz
    tar -xzf kafka_2.12-2.5.0.tgz
    ```
Any latest version can be used — this is an example.

2. Start Zookeeper and Kafka server.

    ```sh
    ~/yb-kafka/kafka_2.12-2.5.0/bin/zookeeper-server-start.sh config/zookeeper.properties &
    ~/yb-kafka/kafka_2.12-2.5.0/bin/kafka-server-start.sh config/server.properties &
    ```

3. Create a Kafka topic.

    ```sh
    $ ~/yb-kafka/kafka_2.12-2.5.0/bin/kafka-topics.sh --create --zookeeper localhost:2181--replication-factor 1 --partitions 1 --topic test
    ```

    This needs to be done only once.

4. Run the following to produce data in that topic:

    ```sh
    $ ~/yb-kafka/kafka_2.12-2.5.0/bin/kafka-console-producer.sh --broker-list localhost:9092--topic test_topic
    ```

5. Just cut-and-paste the following lines at the prompt:

     ```
     {"key" : "A", "value" : 1, "ts" : 1541559411000}
     {"key" : "B", "value" : 2, "ts" : 1541559412000}
     {"key" : "C", "value" : 3, "ts" : 1541559413000}
     ```

     Feel free to `Ctrl-C` this process or switch to a different shell as more values can be added later as well to the same topic.

2. Install YugabyteDB and create the database table.

    [Install YugabyteDB and start a local cluster](../../../quick-start/install/).
    Create a database and table by running the following command. You can find `ycqlsh` in the `bin`  subdirectory located inside the YugabyteDB installation folder.

    ```postgresql
    yugabyte=# CREATE DATABASE IF NOT EXISTS demo;
    yugabyte=# \c demo
    demo=# CREATE TABLE demo.test_table (key text, value bigint, ts timestamp, PRIMARY KEY (key));
    ```

3. Set up and run the Kafka Connect Sink.

    Set up the JAR files required by Kafka Connect.

    ```sh
    $ cd ~/yb-kafka/yb-kafka-connector/
    mvn clean install -DskipTests
    $ cp  
    $ cd ~/yb-kafka/kafka_2.12-2.5.0/libs/
    $ wget https://repo1.maven.org/maven2/io/netty/netty-all/4.1.51.Final/netty-all-4.1.51.Final.jar
    $ wget https://repo1.maven.org/maven2/com/yugabyte/cassandra-driver-core/3.8.0-yb-5/cassandra-driver-core-3.8.0-yb-5.jar
    $ wget https://repo1.maven.org/maven2/io/dropwizard/metrics/metrics-core/4.1.11/metrics-core-4.1.11.jar
    ```

    Finally, run the Kafka Connect YugabyteDB Sink Connector in standalone mode:

    ```sh
    $ ~/yb-kafka/kafka_2.12-2.5.0/bin/connect-standalone.sh ~/yb-kafka/yb-kafka-connector/resourcesexamples/kafka.connect.properties ~/yb-kafka/yb-kafka-connector/resources/examplesyugabyte.sink.properties
    ```

    *Notes*:

    - Setting the `bootstrap.servers` to a remote host/ports in the`kafka.connect.properties`file can help connect to any accessible existing Kafka cluster.
    - The `database` and `tablename` values in the `yugabyte.sink.properties` file should match the values in the ysqlsh commands in step 5.
    - The `topics` value should match the topic name from producer in step 6.
    - Setting the `yugabyte.cql.contact.points` to a non-local list of host and ports will help connect to any remote-accessible existing YugabyteDB cluster.
   - Check the console output (optional).

     You should see something like this (relevant lines from `YBSinkTask.java`) on the console:

    ```
    [2018-10-28 16:24:16,037] INFO Start with keyspace=demo, table=test_table(com.yb.connect.sink.YBSinkTask:79)
    [2018-10-28 16:24:16,054] INFO Connecting to nodes: /127.0.0.1:9042,/127.0.0.2:9042,127.0.0.3:9042 (com.yb.connect.sink.YBSinkTask:189)
    [2018-10-28 16:24:16,517] INFO Connected to cluster: cluster1(com.yb.connect.sink.YBSinkTask:155)
    [2018-10-28 16:24:16,594] INFO Processing 3 records from Kafka.(com.yb.connect.sink.YBSinkTask:95)
    [2018-10-28 16:24:16,602] INFO Insert INSERT INTO demo.test_table(key,ts,value) VALUES (?,?,? (com.yb.connect.sink.YBSinkTask:439)
    [2018-10-28 16:24:16,612] INFO Prepare SinkRecord ...
    [2018-10-28 16:24:16,618] INFO Bind 'ts' of type timestamp(com.yb.connect.sink.YBSinkTask:255)
    ...
    ```

4. Confirm that the rows are in the target table in the YugabyteDB cluster, using `ysqlsh`.

   ```postgresql
   demo=# select * from demo.test_table;
   ```

   ```
   key | value | ts
   ----+-------+---------------------------------
     A |     1 | 2018-11-07 02:56:51.000000+0000
     C |     3 | 2018-11-07 02:56:53.000000+0000
     B |     2 | 2018-11-07 02:56:52.000000+0000
   ```

   Note that timestamp values are printed in human-readable date format.
