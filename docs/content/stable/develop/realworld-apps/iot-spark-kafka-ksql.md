---
title: IoT fleet management example application
headerTitle: IoT fleet management
linkTitle: IoT fleet management
description: Run this fleet management application built with Kafka, KSQL, Apache Spark, and YugabyteDB.
aliases:
  - /develop/realworld-apps/iot-spark-kafka/
  - /stable/develop/realworld-apps/iot-spark-kafka/
block_indexing: true
menu:
  stable:
    identifier: iot-spark-kafka
    parent: realworld-apps
    weight: 583
isTocNested: true
showAsideToc: true
---

## Overview

This is an end-to-end functional application with source code and installation instructions available on [GitHub](https://github.com/yugabyte/yb-iot-fleet-management). It is a blueprint for an IoT application built on top of YugabyteDB (using the Cassandra-compatible YCQL API) as the database, Confluent Kafka as the message broker, KSQL or Apache Spark Streaming for real-time analytics and Spring Boot as the application framework.

## Scenario

Assume that a fleet management company wants to track their fleet of vehicles which are delivering shipments. The vehicles performing the shipments are of different types (18 Wheelers, buses, large trucks, etc), and the shipments themselves happen over 3 routes (Route-37, Route-82, Route-43). The company wants to track:

- the breakdown of their vehicle types per shipment delivery route
- which vehicles are near road closures so that they can predict delays in deliveries

This app renders a dashboard showing both of the above. Below is a view of the real-time, auto-refreshing dashboard.

![YB IoT Fleet Management Dashboard](/images/develop/realworld-apps/iot-spark-kafka-ksql/yb-iot-fleet-management-screenshot.png)

## Application architecture

This application has the following subcomponents:

- Data Store - YugabyteDB for storing raw events from Kafka as well as the aggregates from the Data Processor
- Data Producer - Test program writing into Kafka
- Data Processor - KSQL or Apache Spark Streaming reading from Kafka, computing the aggregates and store results in the Data Store
- Data Dashboard - Spring Boot app using web sockets, jQuery and bootstrap

We will look at each of these components in detail. Below is an architecture diagram showing how these components fit together.

### Confluent Kafka, KSQL, and YugabyteDB (CKY Stack)

App architecture with the CKY stack is shown below. The same [Kafka Connect Sink Connector for YugabyteDB](https://github.com/yugabyte/yb-kafka-connector) is used for storing both the raw events as well as the aggregate data (that's generated using KSQL).

![YB IoT Fleet Management Architecture with KSQL](/images/develop/realworld-apps/iot-spark-kafka-ksql/yb-iot-fleet-mgmt-arch-kafka-ksql.png)

### Spark, Kafka, and YugabyteDB (SKY Stack)

App architecture with the SKY stack is shown below. The Kafka Connect Sink Connector for YugabyteDB is used for storing raw events from Kafka to YugabyteDB. The aggregate data generated via Apache Spark Streaming is persisted in YugabyteDB using the Spark-Cassandra Connector.

![YB IoT Fleet Management Architecture with Apache Spark](/images/develop/realworld-apps/iot-spark-kafka-ksql/yb-iot-fleet-mgmt-arch-kafka-spark.png)

## Data store

Stores all the user-facing data. YugabyteDB is used here, with the Cassandra-compatible [YCQL](../../../api/ycql) as the programming language.

All the data is stored in the keyspace `TrafficKeySpace`:

```sql
CREATE KEYSPACE IF NOT EXISTS TrafficKeySpace
```

There is one table for storing the raw events. Note the `default_time_to_live` value below to ensure that raw events get auto-expired after the specified period of time. This is to ensure that the raw events do not consume up all the storage in the database and efficiently deleted from the database a short while after their aggregates have been computed.

```sql
CREATE TABLE TrafficKeySpace.Origin_Table (
  vehicleId text,
  routeId text,
  vehicleType text,
  longitude text,
  latitude text,
  timeStamp timestamp,
  speed double,
  fuelLevel double,
PRIMARY KEY ((vehicleId), timeStamp))
WITH default_time_to_live = 3600;
```

There are three tables that hold the user-facing data - `Total_Traffic` for the lifetime traffic information, `Window_Traffic` for the last 30 seconds of traffic and `poi_traffic` for the traffic near a point of interest (road closures). The data processor constantly updates these tables, and the dashboard reads from these tables. Below are the schemas for these tables.

```sql
CREATE TABLE TrafficKeySpace.Total_Traffic (
    routeId text,
    vehicleType text,
    totalCount bigint,
    timeStamp timestamp,
    recordDate text,
    PRIMARY KEY (routeId, recordDate, vehicleType)
);

CREATE TABLE TrafficKeySpace.Window_Traffic (
    routeId text,
    vehicleType text,
    totalCount bigint,
    timeStamp timestamp,
    recordDate text,
    PRIMARY KEY (routeId, recordDate, vehicleType)
);

CREATE TABLE TrafficKeySpace.poi_traffic(
    vehicleid text,
    vehicletype text,
    distance bigint,
    timeStamp timestamp,
    PRIMARY KEY (vehicleid)
);
```

## Data producer

A program that generates random test data and publishes it to the Kafka topic `iot-data-event`. This emulates the data received from the connected vehicles using a message broker in the real world.

A single data point is a JSON payload and looks as follows:

```
{
  "vehicleId":"0bf45cac-d1b8-4364-a906-980e1c2bdbcb",
  "vehicleType":"Taxi",
  "routeId":"Route-37",
  "longitude":"-95.255615",
  "latitude":"33.49808",
  "timestamp":"2017-10-16 12:31:03",
  "speed":49.0,
  "fuelLevel":38.0
}
```

The [Kafka Connect Sink Connector for YugabyteDB](https://github.com/yugabyte/yb-kafka-connector) reads the above `iot-data-event` topic, transforms the event into a YCQL `INSERT` statement and then calls YugabyteDB to persist the event in the `TrafficKeySpace.Origin_Table` table.

## Data processor

### KSQL

KSQL is the open source streaming SQL engine for Apache Kafka. It provides an easy-to-use yet powerful interactive SQL interface for stream processing on Kafka, without the need to write code in a programming language such as Java or Python. It supports a wide range of streaming operations, including data filtering, transformations, aggregations, joins, windowing, and sessionization.

The first step in using KSQL is to create a stream from the raw events as shown below.

```sql
CREATE STREAM traffic_stream (
           vehicleId varchar,
           vehicleType varchar,
           routeId varchar,
           timeStamp varchar,
           latitude varchar,
           longitude varchar)
    WITH (
           KAFKA_TOPIC='iot-data-event',
           VALUE_FORMAT='json',
           TIMESTAMP='timeStamp',
           TIMESTAMP_FORMAT='yyyy-MM-dd HH:mm:ss');
```

Various aggreations/queries can now be run on the above stream with results of each type of query stored in a topic of its own. This application uses 3 such queries/topics. Thereafter, the [Kafka Connect Sink Connector for YugabyteDB](https://github.com/yugabyte/yb-kafka-connector) reads these 3 topics and persists the results into the 3 corresponding tables in YugabyteDB.

```sql
CREATE TABLE total_traffic
     WITH ( PARTITIONS=1,
            KAFKA_TOPIC='total_traffic',
            TIMESTAMP='timeStamp',
            TIMESTAMP_FORMAT='yyyy-MM-dd HH:mm:ss') AS
     SELECT routeId,
            vehicleType,
            count(vehicleId) AS totalCount,
            max(rowtime) AS timeStamp,
            TIMESTAMPTOSTRING(max(rowtime), 'yyyy-MM-dd') AS recordDate
     FROM traffic_stream
     GROUP BY routeId, vehicleType;
```

```sql
CREATE TABLE window_traffic
     WITH ( TIMESTAMP='timeStamp',
            KAFKA_TOPIC='window_traffic',
            TIMESTAMP_FORMAT='yyyy-MM-dd HH:mm:ss',
            PARTITIONS=1) AS
     SELECT routeId,
            vehicleType,
            count(vehicleId) AS totalCount,
            max(rowtime) AS timeStamp,
            TIMESTAMPTOSTRING(max(rowtime), 'yyyy-MM-dd') AS recordDate
     FROM traffic_stream
     WINDOW HOPPING (SIZE 30 SECONDS, ADVANCE BY 10 SECONDS)
     GROUP BY routeId, vehicleType;
```

```sql
CREATE STREAM poi_traffic
      WITH ( PARTITIONS=1,
             KAFKA_TOPIC='poi_traffic',
             TIMESTAMP='timeStamp',
             TIMESTAMP_FORMAT='yyyy-MM-dd HH:mm:ss') AS
      SELECT vehicleId,
             vehicleType,
             cast(GEO_DISTANCE(cast(latitude AS double),cast(longitude AS double),33.877495,-95.50238,'KM') AS bigint) AS distance,
             timeStamp
      FROM traffic_stream
      WHERE GEO_DISTANCE(cast(latitude AS double),cast(longitude AS double),33.877495,-95.50238,'KM') < 30;
```

### Apache Spark streaming

This is a Apache Spark streaming application that consumes the data stream from the Kafka topic, converts them into meaningful insights and writes the resulting aggregate data back to YugabyteDB.

Spark communicates with YugabyteDB using the Spark-Cassandra connector. This is done as follows:

```java
SparkConf conf =
  new SparkConf().setAppName(prop.getProperty("com.iot.app.spark.app.name"))
                 .set("spark.cassandra.connection.host",prop.getProperty("com.iot.app.cassandra.host"))
```

The data is consumed from a Kafka stream and collected in 5 second batches. This is achieved as follows:

```java
JavaStreamingContext jssc = new JavaStreamingContext(conf,Durations.seconds(5));

JavaPairInputDStream<String, IoTData> directKafkaStream =
  KafkaUtils.createDirectStream(jssc,
                                String.class,
                                IoTData.class,
                                StringDecoder.class,
                                IoTDataDecoder.class,
                                kafkaParams,
                                topicsSet
                                );
```

It computes the following:

- Compute a breakdown by vehicle type and the shipment route across all the vehicles and shipments done so far.
- Compute the above breakdown for active shipments. This is done by computing the breakdown by vehicle type and shipment route for the last 30 seconds.
- Detect the vehicles which are within a 20 mile radius of a given Point of Interest (POI), which represents a road-closure.

## Data dashboard

This is a [Spring Boot](http://projects.spring.io/spring-boot/) application which queries the data from YugabyteDB and pushes the data to the webpage using [Web Sockets](http://docs.spring.io/spring/docs/current/spring-framework-reference/html/websocket.html#websocket-intro) and [jQuery](https://jquery.com/). The data is pushed to the web page in fixed intervals so data will be refreshed automatically. Dashboard displays data in charts and tables. This web page uses [bootstrap.js](http://getbootstrap.com/) to display the dashboard containing charts and tables.

We create entity classes for the three tables `Total_Traffic`, `Window_Traffic` and `Poi_Traffic`, and DAO interfaces for all the entities extending `CassandraRepository`. For example, you create the DAO class for `TotalTrafficData` entity as follows.

```java
@Repository
public interface TotalTrafficDataRepository extends CassandraRepository<TotalTrafficData> {
  @Query("SELECT * FROM traffickeyspace.total_traffic WHERE recorddate = ? ALLOW FILTERING")
  Iterable<TotalTrafficData> findTrafficDataByDate(String date);
}
```

In order to connect to YugabyteDB cluster and get connection for database operations, you write the assandraConfig class. This is done as follows:

```java
public class CassandraConfig extends AbstractCassandraConfiguration {
  @Bean
  public CassandraClusterFactoryBean cluster() {
    // Create a Cassandra cluster to access YugabyteDB using CQL.
    CassandraClusterFactoryBean cluster = new CassandraClusterFactoryBean();
    // Set the database host.
    cluster.setContactPoints(environment.getProperty("com.iot.app.cassandra.host"));
    // Set the database port.
    cluster.setPort(Integer.parseInt(environment.getProperty("com.iot.app.cassandra.port")));
    return cluster;
  }
}
```

Note that currently the Dashboard does not use the raw events table and relies only on the data stored in the aggregates tables.

## Summary

This application is a blue print for building IoT applications. The instructions to build and run the application, as well as the source code can be found in the [IoT Fleet Management GitHub repository](https://github.com/yugabyte/yb-iot-fleet-management).
