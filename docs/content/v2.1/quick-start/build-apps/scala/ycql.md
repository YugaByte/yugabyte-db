---
title: Build a Scala application that uses YCQL
headerTitle: Build a Scala application
linkTitle: Scala
description: Build a Scala application that uses YCQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: Scala
    identifier: scala-1
    weight: 558
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li>
    <a href="/latest/quick-start/build-apps/scala/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>



## sbt

To build your Java application using the YugabyteDB Java driver for YCQL, add the following sbt dependency to your application:

```sbt
libraryDependencies += "com.yugabyte" % "cassandra-driver-core" % "3.2.0-yb-19"
```

## Working Example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe and are able to interact with it using the YCQL shell. If not, please follow these steps in the [quick start guide](../../../../api/ycql/quick-start/).
- installed Scala version 2.12+ and sbt 1.3.8+

### Create the sbt build file

Create a sbt build file `build.sbt` and add the following content into it.

```sbt
name := "YBCqlHelloWorld"
version := "1.0"
scalaVersion := "2.12.11"
scalacOptions := Seq("-unchecked", "-deprecation")

// https://mvnrepository.com/artifact/com.yugabyte/cassandra-driver-core
libraryDependencies += "com.yugabyte" % "cassandra-driver-core" % "3.2.0-yb-19"
```

### Writing a sample app

Copy the following contents into the file `YBCqlHelloWorld.scala`.

```scala
package com.yugabyte.sample.apps

import com.datastax.driver.core.Cluster
import com.datastax.driver.core.ResultSet
import com.datastax.driver.core.Row
import com.datastax.driver.core.Session


object YBCqlHelloWorld {

  def main(args: Array[String]): Unit = {
    try {
      // Create a Cassandra client.
      val cluster = Cluster.builder.addContactPoint("127.0.0.1").build
      val session = cluster.connect

      // Create keyspace 'ybdemo' if it does not exist.
      val createKeyspace = "CREATE KEYSPACE IF NOT EXISTS ybdemo;";
      val createKeyspaceResult = session.execute(createKeyspace);
      println("Created keyspace ybdemo");

      // Create table 'employee' if it does not exist.
      val createTable = """
        CREATE TABLE IF NOT EXISTS ybdemo.employee (
          id int PRIMARY KEY,
          name varchar,
          age int,
          language varchar
        );
      """
      val createResult = session.execute(createTable)
      println("Created table employee")

      // Insert a row.
      val insert = """
        INSERT INTO ybdemo.employee
        |(id, name, age, language)
        |VALUES
        |(1, 'John', 35, 'Scala');
        """.trim.stripMargin('|').replaceAll("\n", " ")
      val insertResult = session.execute(insert)
      println(s"Inserted data: ${insert}")

      // Query the row and print out the result.
      val select = """
        SELECT name, age, language
         FROM  ybdemo.employee
         WHERE id = 1;
      """
      val rows = session.execute(select).all
      val name = rows.get(0).getString(0)
      val age = rows.get(0).getInt(1)
      val language = rows.get(0).getString(2)
      println(
        s"""
          Query returned ${rows.size}
          |row: name=${name},
          |age=${age},
          |language=${language}
        """.trim.stripMargin('|').replaceAll("\n", " ")
      )

      // Close the client.
      session.close
      cluster.close
    } catch {
      case e: Throwable => println(e.getMessage)
    }

  } //  def main

} // object YBCqlHelloWorld
```


### Build and run the application

To build the application, just run the following command.

```sh
$ sbt compile
```

To run the program, run the following command.

```sh
$ sbt run
```

You should see the following as the output.

```
Created keyspace ybdemo
Created table employee
Inserted data: INSERT INTO ybdemo.employee (id, name, age, language) VALUES (1, 'John', 35, 'Scala');
Query returned 1 row: name=John, age=35, language=Scala
```

