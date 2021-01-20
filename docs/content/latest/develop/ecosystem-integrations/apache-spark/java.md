---
title: Build a Java application using Apache Spark and YugabyteDB
headerTitle: Apache Spark
linkTitle: Apache Spark
description: Build and run a Java-based sample word-count application using Apache Spark and YugabyteDB.
menu:
  latest:
    identifier: apache-spark-2-java
    parent: ecosystem-integrations
    weight: 572
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./scala.md" >}}" class="nav-link">
      <i class="icon-scala" aria-hidden="true"></i>
      Scala
    </a>
  </li>

  <li >
    <a href="{{< relref "./java.md" >}}" class="nav-link active">
      <i class="icon-java-bold" aria-hidden="true"></i>
      Java
    </a>
  </li>

  <li >
    <a href="{{< relref "./python.md" >}}" class="nav-link">
      <i class="icon-python" aria-hidden="true"></i>
      Python
    </a>
  </li>

</ul>

## Before you begin

### Maven

To build your Java application using the YugabyteDB Spark Connector for YCQL, add the following snippet to your `pom.xml` for Scala 2.11:

```xml
<dependency>
  <groupId>com.yugabyte.spark</groupId>
  <artifactId>spark-cassandra-connector_2.11</artifactId>
  <version>2.4-yb-3</version>
</dependency>
```

## Run a sample application

### Run the Spark word-count sample application

You can run our Spark-based sample application with:

```sh
$ java -jar yb-sample-apps.jar --workload CassandraSparkWordCount --nodes 127.0.0.1:9042
```

It reads data from a table with sentences — by default, it generates an input table `ybdemo_keyspace.lines`, computes the frequencies of the words, and writes the result to the output table `ybdemo_keyspace.wordcounts`.

### Examine the source code

To look at the source code, you can check:

- the source file in our GitHub source repo [here](https://github.com/yugabyte/yugabyte-db/blob/master/java/yb-loadtester/src/main/java/com/yugabyte/sample/apps/CassandraSparkWordCount.java)
- untar the jar `java/yb-sample-apps-sources.jar` in the download bundle

Most of the logic is in the `run()` method of the `CassandraSparkWordCount` class (in the file `src/main/java/com/yugabyte/sample/apps/CassandraSparkWordCount.java`). Some of the key portions of the sample program are explained in the sections below.

## Main sections of an Apache Spark program on Yugabyte

### Initialize the Spark context

The SparkConf object is configured as follows:

```java
// Setup the local spark master, with the desired parallelism.
SparkConf conf = new SparkConf().setAppName("yb.wordcount")
                                .setMaster("local[1]")       // num Spark threads
                                .set("spark.cassandra.connection.host", hostname);

// Create the Java Spark context object.
JavaSparkContext sc = new JavaSparkContext(conf);

// Create the Cassandra connector to Spark.
CassandraConnector connector = CassandraConnector.apply(conf);

// Create a Cassandra session, and initialize the keyspace.
Session session = connector.openSession();
```

### Set the input source

To set the input data for Spark, you can do one of the following.

- Reading from a table with a column `line` as the input

```java
// Read rows from table and convert them to an RDD.
JavaRDD<String> rows = javaFunctions(sc).cassandraTable(keyspace, inputTable)
                                        .select("line")
                                        .map(row -> row.getString("line"));
```

- Reading from a file as the input:

```java
// Read the input file and convert it to an RDD.
JavaRDD<String> rows = sc.textFile(inputFile);
```

### Perform the word count processing

The word count is performed using the following code snippet.

```java
// Perform the word count.
JavaPairRDD<String, Integer> counts = rows.flatMap(line -> Arrays.asList(line.split(" ")).iterator())
                                          .mapToPair(word -> new Tuple2<String, Integer>(word, 1))
                                          .reduceByKey((x, y) ->  x + y);
```

### Set the output table

The output is written to the `outTable` table.

```java
// Create the output table.
session.execute("CREATE TABLE IF NOT EXISTS " + outTable +
                " (word VARCHAR PRIMARY KEY, count INT);");

// Save the output to the YCQL table.
javaFunctions(counts).writerBuilder(keyspace, outputTable, mapTupleToRow(String.class, Integer.class))
                     .withColumnSelector(someColumns("word", "count"))
                     .saveToCassandra();
```
