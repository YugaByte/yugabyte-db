---
title: YugabyteDB JDBC Driver (with cluster awareness and load balancing)
headerTitle: YugabyteDB JDBC Driver
linkTitle: YugabyteDB JDBC Driver
description: Add cluster awareness and load balancing to YugabyteDB distributed SQL databases 
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
section: REFERENCE
menu:
  latest:
    identifier: yugabytedb-jdbc-driver
    parent: drivers
    weight: 2941
aliases:
  - /latest/reference/connectors/yugabytedb-jdbc-driver
isTocNested: true
showAsideToc: true
---

## Overview

The YugabyteDB JDBC Driver is based on the open source [PostgreSQL JDBC Driver (PgJDBC)](https://github.com/pgjdbc/pgjdbc) and incorporates all of the functionality and behavior of that driver. The YugabyteDB JDBC Driver extends PgJDBC to add support for distributed SQL databases created in YugabyteDB universes, including cluster awareness and load balancing. Like PgJDBC, the YugabyteDB JDBC Driver is not based on [`libpq`](#libpq), but supports the [SCRAM-SHA-256 authentication method](../../../secure/authentication/password-authentication/#scram-sha-256).

### Cluster awareness

The YugabyteDB JBDC Driver supports distributed SQL databases on a YugabyteDB universe, or cluster, and adds cluster awareness. When you specify any one node in your YugabyteDB cluster as the initial *contact point*  (`YBClusterAwareDataSource`), the driver discovers the rest of the nodes in the universe and automatically responds to nodes being started, stopped, added, or removed.

### Connection pooling

Internally, the driver maintains a connection pool for each node and selects a live node to get a connection from. If a connection is available in the node's connection pool, a connection is made and used until released back to the pool. If a connection is not available, a new connection is created.

### Load balancing

When a connection is requested, the YugabyteDB JDBC driver uses a round-robin load balancing system to select a node to connect to. If that node has an available connection in the pool, a connection is opened. Upon releasing the connection, YugabyteDB returns the connection to the pool.

## Resources

To get the latest source code, file issues, and track enhancements, see the [Yugabyte JDBC Driver repository](https://github.com/yugabyte/jdbc-yugabytedb).

For details on functionality incorporated from the PostgreSQL JDBC driver, see [Documentation (PostgreSQL JDBC Driver)](https://jdbc.postgresql.org/documentation/documentation.html).

## Download

Add the following lines to your Apache Maven project to access and download the YugabyteDB JDBC Driver.

```
<dependency>
  <groupId>com.yugabyte</groupId>
  <artifactId>jdbc-yugabytedb</artifactId>
  <version>42.2.7-yb-3</version>
</dependency>
```

## Use the driver

1. Create the data source by passing an initial contact point.

    ```java
    String jdbcUrl = "jdbc:postgresql://127.0.0.1:5433/yugabyte";
    YBClusterAwareDataSource ds = new YBClusterAwareDataSource(jdbcUrl);
    ```

2. Use like a regular connection pooling data source.

    ```java
    // Using try-with-resources to auto-close the connection when done.
    try (Connection connection = ds.getConnection()) {
        // Use the connection as usual.
    } catch (java.sql.SQLException e) {
        // Handle/Report error.
    }
    ```

## Develop and test locally

1. Clone the [YugabyteDB JDBC Driver GitHub repository](https://github.com/yugabyte/jdbc-yugabytedb).

    ```sh
    git clone https://github.com/yugabyte/jdbc-yugabytedb.git && cd jdbc-yugabytedb
    ```

2. Build and install into your local Maven directory.

    ```sh
     mvn clean install -DskipTests
    ```

3. Add the lines below to your project.

    ```xml
    <dependency>
        <groupId>com.yugabyte</groupId>
        <artifactId>jdbc-yugabytedb</artifactId>
        <version>42.2.7-yb-3-SNAPSHOT</version>
    </dependency>
    ```

