---
title: Use change data capture (CDC) to Kafka
headerTitle: Change data capture (CDC) to Kafka
linkTitle: CDC to Kafka
description: Learn how to use change data capture (CDC) API to send data changes to Apache Kafka.
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
block_indexing: true
menu:
  stable:
    parent: cdc
    identifier: cdc-to-kafka
    weight: 692
type: page
isTocNested: true
showAsideToc: true
---

Follow the steps below to connect a YugabyteDB cluster to use the Change Data Capture (CDC) API to send data changes to Apache Kafka. To learn about the change data capture (CDC) architecture, see [Change data capture (CDC)](../../../architecture/cdc-architecture).

## Prerequisites

### YugabyteDB

Create a YugabyteDB cluster using the steps outlined in [Manual Deployment](../../manual-deployment/).

### Java

A JRE (or JDK), for Java 8 or 11, is installed. 

{{< note title="Note" >}}

The Confluent Platform currently only supports Java 8 and 11. If you do not use one of these, an error message is generated and it will not start. For details related to the Confluent Platform, see [Java version requirements](https://docs.confluent.io/current/cli/installing.html#java-version-requirements).

{{< /note >}}

### Apache Kafka

A local install of the Confluent Platform should be up and running. The [Confluent Platform](https://docs.confluent.io/current/platform.html) includes [Apache Kafka](https://docs.confluent.io/current/kafka/introduction.html) and additional tools and services (including Zookeeper and Avro), making it easy for you to quickly get started using the Kafka event streaming platform.

To get a local Confluent Platform (with Apache Kafka) up and running quickly, follow the steps in the [Confluent Platform Quick Start (Local)](https://docs.confluent.io/current/quickstart/ce-quickstart.html#ce-quickstart).

## 1. Create the source table

With your local YugabyteDB cluster running, create a table, called `users`, in the default database (`yugabyte`).

```postgresql
CREATE TABLE users (name text, pass text, id int, primary key (id));
```

## 2. Create Avro schemas

The Kafka Connect YugabyteDB Source Connector supports the use of [Apache Avro schemas](http://avro.apache.org/docs/current/#schemas) to serialize and deserialize tables. You can use the [Schema Registry](https://docs.confluent.io/current/schema-registry/index.html) in the Confluent Platform to create and manage Avro schema files. For a step-by-step tutorial, see [Schema Registry Tutorial](https://docs.confluent.io/current/schema-registry/schema_registry_tutorial.html).

Create two Avro schemas, one for the `users` table and one for the primary key of the table. After this step, you should have two files: `table_schema_path.avsc` and `primary_key_schema_path.avsc`.

You can use the following two Avro schema examples that will work with the `users` table you created.

**`table_schema_path.avsc`:**

```json
{
  "type":"record",
  "name":"Table",
  "namespace":"org.yb.cdc.schema",
  "fields":[
  { "name":"name", "type":["null", "string"] },
  { "name":"pass", "type":["null", "string"] },
  { "name":"id", "type":["null", "int"] }
  ]
}
```

**`primary_key_schema_path.avsc`:**

```json
{
  "type":"record",
  "name":"PrimaryKey",
  "namespace":"org.yb.cdc.schema",
  "fields":[
  { "name":"id", "type":["null", "int"] }
  ]
}
```

## 3. Start the Apache Kafka services

1. Create a Kafka topic.

    ```sh
    ./bin/kafka-topics --create --partitions 1 --topic users_topic --bootstrap-server localhost:9092 --replication-factor 1
    ```

2. Start the Kafka consumer service.

    ```sh
    bin/kafka-avro-console-consumer --bootstrap-server localhost:9092 --topic users_topic --key-deserializer=io.confluent.kafka.serializers.KafkaAvroDeserializer     --value-deserializer=io.confluent.kafka.serializers.KafkaAvroDeserializer
    ```

## 4. Download the Kafka Connect YugabyteDB Source Connector

Download the Kafka Connect YugabyteDB Source Connector JAR file (`yb-cdc-connector.jar`).

```sh
$ wget -O yb-cdc-connector.jar https://github.com/yugabyte/yb-kafka-connector/blob/master/yb-cdc/yb-cdc-connector.jar?raw=true

```

## 5. Log to Kafka

Run the following command to start logging an output stream of data changes from the YugabyteDB `cdc` table to Apache Kafka.

```sh
java -jar yb-cdc-connector.jar \
--table_name yugabyte.cdc \
--topic_name cdc-test \
--table_schema_path table_schema_path.avsc \
--primary_key_schema_path primary_key_schema_path.avsc
```

The example above uses the following parameters:

- `--table_name` — Specifies the namespace and table, where namespace is the database (YSQL) or keyspace (YCQL).
- `--master_addrs` — Specifies the IP addresses for all of the YB-Master servers that are producing or consuming. Default value is `127.0.0.1:7100`. If you are using a 3-node local cluster, then you need to specify a comma-delimited list of the addresses for all of your YB-Master servers.
- `topic_name` — Specifies the Apache Kafka topic name.
- `table_schema_path` — Specifies the location of the Avro file (`.avsc`) for the table schema.
- `primary_key_schema_path` — Specifies the location of the Avro file (`.avsc`) for the primary key schema.

## 6. Write values and observe

In another window, write values to the table and observe the values on your Kafka output stream.
