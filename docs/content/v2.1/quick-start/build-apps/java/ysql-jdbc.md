---
title: Build a Java application that uses YSQL
headerTitle: Build a Java application
linkTitle: Java
description: Build a simple Java application that uses YSQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: Java
    identifier: java-1
    weight: 550
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/java/ysql-jdbc" class="nav-link active">
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
    <a href="/latest/quick-start/build-apps/java/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Maven

To build your Java application using the [PostgreSQL JDBC driver](https://jdbc.postgresql.org/), add the following Maven dependency to your application:

```mvn
<dependency>
  <groupId>org.postgresql</groupId>
  <artifactId>postgresql</artifactId>
  <version>42.2.5</version>
</dependency>
```

## Working example

### Prerequisites

This tutorial assumes that you have:

- YugabyteDB up and running. If you are new to YugabyteDB, you can download, install, and have YugabyteDB up and running within five minutes by following the steps in the [Quick Start guide](../../../../quick-start/).
- Java Development Kit (JDK) 1.8, or later, is installed. JDK installers for Linux and macOS can be downloaded from [OpenJDK](http://jdk.java.net/), [AdoptOpenJDK](https://adoptopenjdk.net/), or [Azul Systems](https://www.azul.com/downloads/zulu-community/).
- [Apache Maven](https://maven.apache.org/index.html) 3.3, or later, is installed.

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
      <groupId>org.postgresql</groupId>
      <artifactId>postgresql</artifactId>
      <version>42.2.5</version>
    </dependency>
  </dependencies>

  <build>
    <plugins>
      <plugin>
        <groupId>org.apache.maven.plugins</groupId>
        <artifactId>maven-compiler-plugin</artifactId>
        <version>3.7.0</version>
        <configuration>
          <source>1.8</source>
          <target>1.8</target>
        </configuration>
      </plugin>
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

### Write an application

Create the appropriate directory structure as expected by Maven.

```sh
$ mkdir -p src/main/java/com/yugabyte/sample/apps
```

Copy the following contents into the file `src/main/java/com/yugabyte/sample/apps/YBSqlHelloWorld.java`.

```java
package com.yugabyte.sample.apps;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;

public class YBSqlHelloWorld {
  public static void main(String[] args) {
    try {
      // Create the DB connection
      Class.forName("org.postgresql.Driver");
      Connection connection = null;
      connection = DriverManager.getConnection(
                 "jdbc:postgresql://127.0.0.1:5433/yugabyte","yugabyte", "yugabyte");

      // Create table 'employee'
      String createStmt = "CREATE TABLE employee (id int PRIMARY KEY, " +
                                                 "name varchar, " +
                                                 "age int, " +
                                                 "language varchar);";
      connection.createStatement().execute(createStmt);
      System.out.println("Created table employee");

      // Insert a row.
      String insertStmt = "INSERT INTO employee (id, name, age, language)" +
                                                " VALUES (1, 'John', 35, 'Java');";
      connection.createStatement().executeUpdate(insertStmt);
      System.out.println("Inserted data: " + insertStmt);

      // Query the row and print out the result.
      String selectStmt = "SELECT name, age, language FROM employee WHERE id = 1;";
      PreparedStatement pstmt = connection.prepareStatement(selectStmt);
      ResultSet rs = pstmt.executeQuery();
      while (rs.next()) {
          String name = rs.getString(1);
          int age = rs.getInt(2);
          String language = rs.getString(3);
          System.out.println("Query returned: " +
                             "name=" + name + ", age=" + age + ", language: " + language);
      }

      // Close the client.
      connection.close();
    } catch (Exception e) {
        System.err.println("Error: " + e.getMessage());
    }
  }
}
```

### Build and run the application

To build the application, run the following command.

```sh
$ mvn package
```

To run the program, run the following command.

```sh
$ java -cp "target/hello-world-1.0.jar:target/lib/*" com.yugabyte.sample.apps.YBSqlHelloWorld
```

You should see the following as the output.

```
Created table employee
Inserted data: INSERT INTO employee (id, name, age, language) VALUES (1, 'John', 35, 'Java');
Query returned: name=John, age=35, language: Java
```
