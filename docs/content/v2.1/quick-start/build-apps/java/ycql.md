---
title: Build a Java application that uses YCQL
headerTitle: Build a Java application
linkTitle: Java
description: Build a Java application that uses YCQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: Java
    identifier: java-3
    weight: 550
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/java/ysql-jdbc" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - JDBC
    </a>
  </li>
  <li >
    <a href="/latest/quick-start/build-apps/java/ysql-spring-data" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - Spring Data JPA
    </a>
  </li>
  <li>
    <a href="/latest/quick-start/build-apps/java/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Maven

To build your Java application using the YugabyteDB Cassandra driver, add the following Maven dependency to your application:

```mvn
<dependency>
  <groupId>com.yugabyte</groupId>
  <artifactId>cassandra-driver-core</artifactId>
  <version>3.2.0-yb-18</version>
</dependency>
```

## Working Example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe and are able to interact with it using the YCQL shell. If not, please follow these steps in the [quick start guide](../../../../api/ycql/quick-start/).
- installed JDK version 1.8+ and maven 3.3+

### Create the Maven build file

Create a maven build file `pom.xml` and add the following content into it.

```mvn
<?xml version="1.0"?>
<project
  xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd"
  xmlns="http://maven.apache.org/POM/4.0.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <modelVersion>4.0.0</modelVersion>

  <groupId>com.yugabyte.sample.apps</groupId>
  <artifactId>hello-world</artifactId>
  <version>1.0</version>
  <packaging>jar</packaging>

  <dependencies>
    <dependency>
      <groupId>com.yugabyte</groupId>
      <artifactId>cassandra-driver-core</artifactId>
      <version>3.2.0-yb-18</version>
    </dependency>
  </dependencies>

  <build>
    <plugins>
      <plugin>
        <groupId>org.apache.maven.plugins</groupId>
        <artifactId>maven-dependency-plugin</artifactId>
        <version>2.1</version>
        <executions>
          <execution>
            <id>copy-dependencies</id>
            <phase>prepare-package</phase>
            <goals>
              <goal>copy-dependencies</goal>
            </goals>
            <configuration>
              <outputDirectory>${project.build.directory}/lib</outputDirectory>
              <overWriteReleases>true</overWriteReleases>
              <overWriteSnapshots>true</overWriteSnapshots>
              <overWriteIfNewer>true</overWriteIfNewer>
            </configuration>
          </execution>
        </executions>
      </plugin>
    </plugins>
  </build>
</project>
```

### Write a sample application

Create the appropriate directory structure as expected by Maven.

```sh
$ mkdir -p src/main/java/com/yugabyte/sample/apps
```

Copy the following contents into the file `src/main/java/com/yugabyte/sample/apps/YBCqlHelloWorld.java`.

```java
package com.yugabyte.sample.apps;

import java.util.List;
import com.datastax.driver.core.Cluster;
import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import com.datastax.driver.core.Session;

public class YBCqlHelloWorld {
  public static void main(String[] args) {
    try {
      // Create a Cassandra client.
      Cluster cluster = Cluster.builder()
                               .addContactPoint("127.0.0.1")
                               .build();
      Session session = cluster.connect();

      // Create keyspace 'ybdemo' if it does not exist.
      String createKeyspace = "CREATE KEYSPACE IF NOT EXISTS ybdemo;";
      ResultSet createKeyspaceResult = session.execute(createKeyspace);
      System.out.println("Created keyspace ybdemo");

      // Create table 'employee' if it does not exist.
      String createTable = "CREATE TABLE IF NOT EXISTS ybdemo.employee (id int PRIMARY KEY, " +
                                                                       "name varchar, " +
                                                                       "age int, " +
                                                                       "language varchar);";
      ResultSet createResult = session.execute(createTable);
      System.out.println("Created table employee");

      // Insert a row.
      String insert = "INSERT INTO ybdemo.employee (id, name, age, language)" +
                                          " VALUES (1, 'John', 35, 'Java');"; 
      ResultSet insertResult = session.execute(insert);
      System.out.println("Inserted data: " + insert);

      // Query the row and print out the result.
      String select = "SELECT name, age, language FROM ybdemo.employee WHERE id = 1;";
      ResultSet selectResult = session.execute(select);
      List<Row> rows = selectResult.all();
      String name = rows.get(0).getString(0);
      int age = rows.get(0).getInt(1);
      String language = rows.get(0).getString(2);
      System.out.println("Query returned " + rows.size() + " row: " +
                         "name=" + name + ", age=" + age + ", language: " + language);

      // Close the client.
      session.close();
      cluster.close();
    } catch (Exception e) {
        System.err.println("Error: " + e.getMessage());
    }
  }
}
```

### Build and run the application

To build the application, just run the following command.

```sh
$ mvn package
```

To run the program, run the following command.

```sh
$ java -cp "target/hello-world-1.0.jar:target/lib/*" com.yugabyte.sample.apps.YBCqlHelloWorld
```

You should see the following as the output.

```
Created keyspace ybdemo
Created table employee
Inserted data: INSERT INTO ybdemo.employee (id, name, age, language) VALUES (1, 'John', 35, 'Java');
Query returned 1 row: name=John, age=35, language: Java
```
