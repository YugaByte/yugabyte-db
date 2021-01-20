---
title: Build a YugabyteDB application using Java and YEDIS
headerTitle: Build an application using Java
linkTitle: Java
description: Use Java to build a YugabyteDB application that interacts with YEDIS
aliases:
  - /latest/yedis/develop/client-drivers/java
menu:
  latest:
    identifier: client-drivers-yedis-java
    parent: develop-yedis
isTocNested: true
showAsideToc: true
---

## Maven

To build your Java application using the YugabyteDB version of the Jedis driver, add the following Maven dependency to your application:

```xml
<dependency>
  <groupId>com.yugabyte</groupId>
  <artifactId>jedis</artifactId>
  <version>2.9.0-yb-16</version>
</dependency>
```

## Working example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the Redis shell. If not, please follow these steps in [Quick start](../../../../quick-start/).
- JDK version 1.8 or later
- Maven 3.3 or later

### Create the Maven build file

Create a Maven build file `pom.xml` and add the following content into it.

```xml
<?xml version="1.0"?>
<!-- Copyright (c) Yugabyte, Inc. -->
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
      <artifactId>jedis</artifactId>
      <version>2.9.0-yb-16</version>
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

### Write a HelloWorld Java application

Create the appropriate directory structure as expected by Maven.

```sh
$ mkdir -p src/main/java/com/yugabyte/sample/apps
```

Copy the following contents into the file `src/main/java/com/yugabyte/sample/apps/YBRedisHelloWorld.java`.

```java
package com.yugabyte.sample.apps;

import java.util.HashMap;
import java.util.Map;
import redis.clients.jedis.Jedis;

public class YBRedisHelloWorld {
  public static void main(String[] args) {
    try {
      // Create a Jedis client.
      Jedis jedisClient = new Jedis("127.0.0.1");

      // Prepare the employee information to insert.
      String userid = "1";
      Map<String, String> userProfile = new HashMap<String, String>();
      userProfile.put("name", "John");
      userProfile.put("age", "35");
      userProfile.put("language", "Redis");

      // Insert the data.
      String result = jedisClient.hmset(userid, userProfile);
      System.out.println("HMSET returned " + result + ": id=1, name=John, age=35, language=Redis");

      // Query the data.
      Map<String, String> userData = jedisClient.hgetAll(userid);
      System.out.println("Query result: name=" + userData.get("name") +
                         ", age=" + userData.get("age") + ", language=" + userData.get("language"));

      // Close the client.
      jedisClient.close();
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

To start the application, run the following command.

```sh
% java -cp "target/hello-world-1.0.jar:target/lib/*" com.yugabyte.sample.apps.YBRedisHelloWorld
```

You should see the following as the output.

```
HMSET returned OK: id=1, name=John, age=35, language=Redis
Query result: name=John, age=35, language=Redis
```
