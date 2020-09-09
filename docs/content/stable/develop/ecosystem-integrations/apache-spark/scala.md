---
title: Build a Scala application using Apache Spark and YugabyteDB
headerTitle: Apache Spark
linkTitle: Apache Spark
description: Build a Scala application using Apache Spark and YugabyteDB
aliases:
  - /develop/ecosystem-integrations/apache-spark/
  - /stable/develop/ecosystem-integrations/apache-spark/
block_indexing: true
menu:
  stable:
    identifier: apache-spark-1-scala
    parent: ecosystem-integrations
    weight: 572
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./scala.md" >}}" class="nav-link active">
      <i class="icon-scala" aria-hidden="true"></i>
      Scala
    </a>
  </li>

  <li >
    <a href="{{< relref "./java.md" >}}" class="nav-link">
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

### sbt

To build your Scala application using the YugabyteDB Spark Connector for YCQL, add the following sbt dependency to your application:

```
libraryDependencies += "com.yugabyte.spark" %% "spark-cassandra-connector" % "2.4-yb-2"
```

## Build a sample application

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the YCQL shell (`ycqlsh`). If not, please follow these steps in the [quick start guide](../../../../api/ycql/quick-start/).

- installed Scala version 2.12 or later and sbt 1.3.8 or later/

- installed the [`sbt-assembly`](https://github.com/sbt/sbt-assembly) plugin in your sbt project as shown below.

```sh
addSbtPlugin("com.eed3si9n" % "sbt-assembly" % "0.14.10")
```

### Create the sbt build file

Create an sbt build file (`build.sbt`) and add the following content into it.

```sbt
name := "CassandraSparkWordCount"
version := "1.0"
scalaVersion := "2.11.12"
scalacOptions := Seq("-unchecked", "-deprecation")

val sparkVersion = "2.4.4"

// maven repo at https://mvnrepository.com/artifact/com.yugabyte.spark/spark-cassandra-connector
libraryDependencies += "com.yugabyte.spark" %% "spark-cassandra-connector" % "2.4-yb-2"

// maven repo at https://mvnrepository.com/artifact/org.apache.spark/spark-core
libraryDependencies += "org.apache.spark" %% "spark-core" % sparkVersion % Provided

// maven repo at https://mvnrepository.com/artifact/org.apache.spark/spark-sql
libraryDependencies += "org.apache.spark" %% "spark-sql" % sparkVersion % Provided
```

### Write a sample application

Copy the following contents into the file `CassandraSparkWordCount.scala`.

```scala
package com.yugabyte.sample.apps

import com.datastax.spark.connector.cql.CassandraConnector
import org.apache.spark.SparkConf
import org.apache.spark.sql.SparkSession


object CassandraSparkWordCount {

  val DEFAULT_KEYSPACE = "ybdemo";
  val DEFAULT_INPUT_TABLENAME = "lines";
  val DEFAULT_OUTPUT_TABLENAME = "wordcounts";

  def main(args: Array[String]): Unit = {

    // Setup the local spark master, with the desired parallelism.
    val conf =
      new SparkConf()
        .setAppName("yb.wordcount")
        .setMaster("local[*]")
        .set("spark.cassandra.connection.host", "127.0.0.1")

    val spark =
      SparkSession
        .builder
        .config(conf)
        .getOrCreate

    // Create the Spark context object.
    val sc = spark.sparkContext

    // Create the Cassandra connector to Spark.
    val connector = CassandraConnector.apply(conf)

    // Create a Cassandra session, and initialize the keyspace.
    val session = connector.openSession


    //------------ Setting Input source (Cassandra table only) -------------\\

    val inputTable = DEFAULT_KEYSPACE + "." + DEFAULT_INPUT_TABLENAME

    // Drop the sample table if it already exists.
    session.execute(s"DROP TABLE IF EXISTS ${inputTable};")

    // Create the input table.
    session.execute(
      s"""
        CREATE TABLE IF NOT EXISTS ${inputTable} (
          id INT,
          line VARCHAR,
          PRIMARY KEY(id)
        );
      """
    )

    // Insert some rows.
    val prepared = session.prepare(
      s"""
        INSERT INTO ${inputTable} (id, line) VALUES (?, ?);
      """
    )

    val toInsert = Seq(
      (1, "ten nine eight seven six five four three two one"),
      (2, "ten nine eight seven six five four three two"),
      (3, "ten nine eight seven six five four three"),
      (4, "ten nine eight seven six five four"),
      (5, "ten nine eight seven six five"),
      (6, "ten nine eight seven six"),
      (7, "ten nine eight seven"),
      (8, "ten nine eight"),
      (9, "ten nine"),
      (10, "ten")
    )

    for ((id, line) <- toInsert) {
      // Note: new Integer() is required here to impedance match with Java
      //       since Scala Int != Java Integer.
      session.execute(prepared.bind(new Integer(id), line))
    }


    //------------- Setting output location (Cassandra table) --------------\\

    val outTable = DEFAULT_KEYSPACE + "." + DEFAULT_OUTPUT_TABLENAME

    // Drop the output table if it already exists.
    session.execute(s"DROP TABLE IF EXISTS ${outTable};")

    // Create the output table.
    session.execute(
      s"""
        CREATE TABLE IF NOT EXISTS ${outTable} (
          word VARCHAR PRIMARY KEY,
          count INT
        );
      """
    )


    //--------------------- Read from Cassandra table ----------------------\\

    // Read rows from table as a DataFrame.
    val df =
      spark
        .read
        .format("org.apache.spark.sql.cassandra")
        .options(
          Map(
            "keyspace" -> DEFAULT_KEYSPACE, // "ybdemo".
            "table" -> DEFAULT_INPUT_TABLENAME // "lines".
          )
        )
        .load


    //------------------------ Perform Word Count --------------------------\\

    import spark.implicits._

    // ----------------------------------------------------------------------
    // Example with RDD.
    val wordCountRdd =
      df.select("line")
        .rdd // reduceByKey() operates on PairRDDs. Start with a simple RDD.
        // Similar to: https://spark.apache.org/examples.html
        // vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
        .flatMap(x => x.getString(0).split(" ")) // This creates the PairRDD.
        .map(word => (word, 1))
        .reduceByKey(_ + _)
        // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    // This is not used for saving, but it could be.
    wordCountRdd
      .toDF("word", "count") // convert to DataFrame for pretty printing.
      .show

    // ----------------------------------------------------------------------
    // Example using DataFrame.
    val wordCountDf =
      df.select("line")
        .flatMap(x => x.getString(0).split(" "))
        .groupBy("value").count // flatMap renames column to "value".
        .toDF("word", "count") // rename columns.

    wordCountDf.show


    //---------------------- Save to Cassandra table -----------------------\\

    // -----------------------------------------------------------------------
    // Save the output to the YCQL table, using RDD as the source.
    // This has been tested to be fungible with the DataFrame->CQL code block.

    /* Comment this line out to enable this code block.
    // This import (for this example) is only needed for the
    // <RDD>.wordCountRdd.saveToCassandra() call.
    import com.datastax.spark.connector._
    wordCountRdd.saveToCassandra(
      DEFAULT_KEYSPACE, // "ybdemo".
      DEFAULT_OUTPUT_TABLENAME, // "wordcounts".
      SomeColumns(
        "word", // first column name.
        "count" // second column name.
      )
    )
    // */

    // ----------------------------------------------------------------------
    // Save the output to the YCQL table, using DataFrame as the source.

    // /* Uncomment this line out to disable this code block.
    wordCountDf
      .write
      .format("org.apache.spark.sql.cassandra")
      .options(
        Map(
          "keyspace" -> DEFAULT_KEYSPACE, // "ybdemo".
          "table" -> DEFAULT_OUTPUT_TABLENAME // "wordcounts".
        )
      )
      .save
    // */


    // ----------------------------------------------------------------------
    // Disconnect from Cassandra.
    session.close

    // Stop the Spark Session.
    spark.stop
  } // def main

} // object CassandraSparkWordCount
```

### Build and run the application

To build the JAR file, run the following command.

```sh
$ sbt assembly
```

To run the program, run the following command.

```sh
$ spark-submit --class com.yugabyte.sample.apps.CassandraSparkWordCount \
    target/scala-2.11/CassandraSparkWordCount-assembly-1.0.jar
```

You should see a table similar to the following as the output.

```
+-----+-----+
| word|count|
+-----+-----+
|  two|    2|
|eight|    8|
|seven|    7|
| four|    4|
|  one|    1|
|  six|    6|
|  ten|   10|
| nine|    9|
|three|    3|
| five|    5|
+-----+-----+
```
