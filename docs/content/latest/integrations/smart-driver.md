---
title: JDBC Smart Driver
linkTitle: JDBC Smart Driver
description: Smart JDBC driver for YSQL
aliases:
menu:
  latest:
    identifier: smart-driver
    parent: integrations
    weight: 577
isTocNested: true
showAsideToc: true
---

JDBC Smart Driver for YSQL is a distributed driver based on the [PostgreSQL JDBC Driver](https://github.com/pgjdbc/pgjdbc).

The following are the key features of JDBC Smart Driver:

- It is **cluster-aware**, which eliminates the need for an external load balancer.

  The driver package includes a `YBClusterAwareDataSource` class that uses one initial contact point for the YugabyteDB cluster as a means of discovering all the nodes and, if required, refreshing the list of live endpoints with every new connection attempt. The refresh is triggered if stale information (older than 5 minutes) is discovered.

- It is **topology-aware**, wich is essential for geographically-distributed applications.

  The driver uses servers that are part of a set of geo-locations specified by topology keys.

## Creating the Smart Driver

You have a choice of obtaining the Smart Driver from Maven or creating it yourself.

### How to Obtain the Driver from Maven

To get the driver from Maven, add the following lines to your Maven project:

```properties
<dependency>
  <groupId>com.yugabyte</groupId>
  <artifactId>jdbc-yugabytedb</artifactId>
  <version>42.2.7-5-beta.1</version>
</dependency>
```

### How to Build the Driver

To build the driver locally, follow this procedure:

1. Clone the following repository:

   ```sh
   git clone https://github.com/yugabyte/jdbc-yugabytedb.git && cd jdbc-yugabytedb
   ```

2. Build and install into your local Maven folder by executing the following command:

   ```sh
   mvn clean install -DskipTests
   ```

   If you encounter a GPG key error, you can avoid it by using the following command instead:

   ```sh
   mvn clean install -DskipTests -Dgpg.skip
   ```

3. Add the following lines to your Maven project:

   ```properties
   <dependency>
       <groupId>com.yugabyte</groupId>
       <artifactId>jdbc-yugabytedb</artifactId>
       <version>42.2.7-yb-5-beta.1</version>
   </dependency> 
   ```

## Using the Smart Driver

Cluster-aware load balancing is performed using the following connection property:

- `load-balance`, which is disabled by default. You can enable it by setting its value to `true`.

Topology-aware load balancing requires the following connection property in addition to `load-balance`:

- `topology-keys`, which accepts a comma separated geo-location values. You can specify the geo-location as cloud:region:zone.

To use the Smart Driver, specify the following:

- Pass new connection properties for load balancing in the connection URL or properties bag.

  To enable uniform load balancing across all servers, you set the `load-balance` property to `true` in the URL, as per the following example:

  ```java
  String yburl = "jdbc:postgresql://127.0.0.1:5433/yugabyte?user=yugabyte&password=yugabyte&load-balance=true";
  DriverManager.getConnection(yburl);
  ```

  To specify topology keys, you set the `topology-keys` property to comma separated values, as per the following example:

  ```java
  String yburl = "jdbc:postgresql://127.0.0.1:5433/yugabyte?user=yugabyte&password=yugabyte&load-balance=true&topology-keys=cloud1:region1:zone1,cloud1:region1.zone2";
  DriverManager.getConnection(yburl);
  ```

- Configure `YBClusterAwareDataSource` for uniform load balancing and then use it to create a connection, as per the following example:

  ```java
  String jdbcUrl = "jdbc:postgresql://127.0.0.1:5433/yugabyte";
  YBClusterAwareDataSource ds = new YBClusterAwareDataSource();
  ds.setUrl(jdbcUrl);
  ds.setLoadBalance("true");
  // Set topology keys to enable topology-aware distribution
  ds.setTopologyKeys("cloud1.region1.zone1,cloud1.region2.zone2");
  // Provide more end points to prevent first connection failure 
  // if an initial contact point is not available 
  ds.setAdditionalEndpoints("127.0.0.2:5433,127.0.0.3:5433");
  
  Connection conn = ds.getConnection();
  ```

- Configure `YBClusterAwareDataSource` with a pooling solution such as Hikari and then use it to create a connection, as per the following example:

  ```java
  Properties poolProperties = new Properties();
  poolProperties.setProperty("dataSourceClassName", "com.yugabyte.ysql.YBClusterAwareDataSource");
  poolProperties.setProperty("maximumPoolSize", 10);
  poolProperties.setProperty("dataSource.serverName", "127.0.0.1");
  poolProperties.setProperty("dataSource.portNumber", "5433");
  poolProperties.setProperty("dataSource.databaseName", "yugabyte");
  poolProperties.setProperty("dataSource.user", "yugabyte");
  poolProperties.setProperty("dataSource.password", "yugabyte");
  poolProperties.setProperty("dataSource.loadBalance", "true");
  // Provide additional end points
  String additionalEndpoints = "127.0.0.2:5433,127.0.0.3:5433,127.0.0.4:5433,127.0.0.5:5433";
  poolProperties.setProperty("dataSource.additionalEndpoints", additionalEndpoints);
  // Load balance between specific geo-locations using topology keys
  String geoLocations = "cloud1.region1.zone1,cloud1.region2.zone2";
  poolProperties.setProperty("dataSource.topologyKeys", geoLocations);
  
  poolProperties.setProperty("poolName", name);
  
  HikariConfig config = new HikariConfig(poolProperties);
  config.validate();
  HikariDataSource ds = new HikariDataSource(config);
  
  Connection conn = ds.getConnection();
  ```

## Examples

To access a sample application that uses Yugabyte Smart Driver, visit [YugabyteDB JDBC driver](https://github.com/yugabyte/jdbc-yugabytedb).

To be able to use the samples, you need to complete the following steps: 

- Install YugabyteDB by following instructions provided in [Quick Start Guide](https://docs.yugabyte.com/latest/quick-start/install/). 

- Build the examples by running `mvn package`.

- Run the `run.sh` script, as per the following guideline:

  ```
  ./run.sh [-v] [-i] -D -</path_to_yugabyte_installation/>
  ```

  In the precediding command, replace:

  - *[-v] [-i]* &nbsp; with `-v` if you want to run the script in `VERBOSE` mode.

  - *[-v] [-i]* &nbsp; with `-i` if you want to run the script in `INTERACTIVE` mode.

  - *[-v] [-i]* &nbsp; with `-v` `-i` if you want to run the script in both `VERBOSE` and `INTERACTIVE` mode at the same time.

  - *</path_to_yugabyte_installation/>* &nbsp; with the path to the directory where you installed YugabyteDB. 

  <br>For more details, you can consult the help for the script.<br>

  <br> The following is an example of a shell command that runs the script:

  ```sh
  ./run.sh -v -i -D ~/yugabyte-2.7.1.1/
  ```

  <br>The `run` script starts a YugabyteDB cluster, demonstrates load balancing through Java applications, and then destroys the cluster.

  <br>In the beginning, the script displays a menu with two options: `UniformLoadBalance` and `TopologyAwareLoadBalance`. Selecting one of these options starts running the corresponding script with its Java application in the background.