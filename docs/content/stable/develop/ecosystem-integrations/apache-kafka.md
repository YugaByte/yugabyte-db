---
title: Apache Kafka
linkTitle: Apache Kafka
description: Apache Kafka
aliases:
  - /develop/ecosystem-integrations/apache-kafka/
block_indexing: true
menu:
  stable:
    identifier: apache-kafka
    parent: ecosystem-integrations
    weight: 571
isTocNested: true
showAsideToc: true
---

In this tutorial, you are going to use the [Kafka Connect-based Sink Connector for YugabyteDB](https://github.com/yugabyte/yb-kafka-connector) to store events from Apache Kafka into YugabyteDB using the [YCQL](../../../api/ycql) API.

## 1. Start local cluster

Start a YugabyteDB cluster on your [local machine](../../../quick-start/install/). Check that you are able to connect to YugabyteDB using `ycqlsh` by doing the following.

```sh
$ ./bin/ycqlsh localhost
```

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh>
```

Create the table that would store the Kafka events.

```sql
ycqlsh> CREATE KEYSPACE IF NOT EXISTS demo;
ycqlsh> CREATE TABLE demo.test_table (key text, value bigint, ts timestamp, PRIMARY KEY (key));
```

## 2. Download Apache Kafka

Download from the [Apache Kafka downloads page](https://kafka.apache.org/downloads). This tutorial uses the `2.11` version of Apache Kafka.

```sh
$ mkdir -p ~/yb-kafka && cd ~/yb-kafka
$ wget http://apache.cs.utah.edu/kafka/2.0.0/kafka_2.12-2.5.0.tgz
$ tar xvfz kafka_2.12-2.5.0.tgz && cd kafka_2.12-2.5.0
```

## 3. Install the Kafka Sink Connector for YugabyteDB

Clone the git `yb-kafka-connector` git repo.

```sh
$ cd ~/yb-kafka
$ git clone https://github.com/yugabyte/yb-kafka-connector.git
$ cd yb-kafka-connector/
```

Build the repository to get the connector JAR file.

```sh
$ mvn clean install -DskipTests
```

The connector jar `yb-kafka-connnector-1.0.0.jar` is now placed in the `./target` directory. Copy this JAR file to the `libs` directory in Kafka Home.

```sh
$ cp ./target/yb-kafka-connnector-1.0.0.jar ~/yb-kafka/kafka_2.12-2.5.0/libs/
```

Go to the Kafka `libs` directory and get the additional required JAR files that the connector depends on (including the driver for the YCQL API)

```sh
$ cd ~/yb-kafka/kafka_2.12-2.5.0/libs/
$ wget https://repo1.maven.org/maven2/io/netty/netty-all/4.1.51.Final/netty-all-4.1.51.Final.jar
$ wget https://repo1.maven.org/maven2/com/yugabyte/cassandra-driver-core/3.8.0-yb-5/cassandra-driver-core-3.8.0-yb-5.jar
$ wget https://repo1.maven.org/maven2/io/dropwizard/metrics/metrics-core/4.1.11/metrics-core-4.1.11.jar
```

## 4. Start ZooKeeper and Kafka

Now you can start ZooKeeper and Kafka as shown below.

```sh
$ cd ~/yb-kafka/kafka_2.12-2.5.0
```

```sh
$ ./bin/zookeeper-server-start.sh config/zookeeper.properties &
```

```sh
$ ./bin/kafka-server-start.sh config/server.properties &
```

Now, create the Kafka topic that will be used to persist messages in the YugabyteDB table created earlier.

```sh
$ ./bin/kafka-topics.sh --create \
    --zookeeper localhost:2181 \
    --replication-factor 1 \
    --partitions 1 \
    --topic test
```

## 5. Start Kafka Sink Connector for YugabyteDB

At this point, you have YugabyteDB's YCQL APU running at 9042 port with the `test_table` table created in the `demo` keyspace. We also have Kafka running at the 9092 port with the `test_topic` topic created. We are ready to start the connector.

```sh
$ ./bin/connect-standalone.sh \
    ~/yb-kafka/yb-kafka-connector/resources/examples/kafka.connect.properties \
    ~/yb-kafka/yb-kafka-connector/resources/examples/yugabyte.sink.properties
```

The `yugabyte.sink.properties` file used above (and shown below) has the configuration necessary for this sink to work correctly. You will have to change this file to the Kafka topic and YugabyteDB table necessary for your application.

```
# Sample yugabyte sink properties.

name=yugabyte-sink
connector.class=com.yb.connect.sink.YBSinkConnector

topics=test_topic

yugabyte.cql.keyspace=demo
yugabyte.cql.tablename=test_table
```

## 6. Produce events for Kafka

We can now produce some events into Kafka using the `kafka-console-producer.sh` utility that ships with Kafka.

```sh
$ ~/yb-kafka/kafka_2.12-2.5.0/bin/kafka-console-producer.sh
    --broker-list localhost:9092 \
    --topic test_topic
```

Enter the following.

```
{"key" : "A", "value" : 1, "ts" : 1541559411000}
{"key" : "B", "value" : 2, "ts" : 1541559412000}
{"key" : "C", "value" : 3, "ts" : 1541559413000}
```

## 7. Verify events in YugabyteDB

The events above should now show up as rows in the YugabyteDB table.

```sql
ycqlsh> SELECT * FROM demo.test_table;
```

```
key | value | ts
----+-------+---------------------------------
  A |     1 | 2018-11-07 02:56:51.000000+0000
  C |     3 | 2018-11-07 02:56:53.000000+0000
  B |     2 | 2018-11-07 02:56:52.000000+0000
```
