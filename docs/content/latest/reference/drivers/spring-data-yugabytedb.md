---
title: Spring Data YugabyteDB
headerTitle: Spring Data YugabyteDB
linkTitle: Spring Data YugabyteDB
description: Spring Data YugabyteDB
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
section: REFERENCE
menu:
  latest:
    identifier: spring-data-yugabytedb
    parent: drivers
    weight: 2943
aliases:
  - /latest/reference/connectors/spring-data-yugabytedb
isTocNested: true
showAsideToc: true
---


The Spring Data YugabyteDB driver brings the power of distributed SQL to Spring developers by using the [YugabyteDB JDBC Driver](https://github.com/yugabyte/jdbc-yugabytedb). The end result is that most features of PostgreSQL v11.2 are now available as a massively-scalable, fault-tolerant distributed database. Spring Data YugabyteDB is based on [Spring Data JPA](https://github.com/spring-projects/spring-data-jpa).

## Unique features

In addition to providing most PostgreSQL features on top of a scalable and resilient distributed database, this project aims to enable the following:

* Eliminate load balancer from SQL (cluster-awareness)
* Develop geo-distributed Apps (topology-awareness)
* Row level geo-partitioning support (partition-awareness)

Go to the [Spring Data YugabyteDB GitHub project](https://github.com/yugabyte/spring-data-yugabytedb/) to watch, star, file issues, and contribute.

## Get started

### Maven configuration

Add the Maven dependency:

```xml
<dependency>
  <groupId>com.yugabyte</groupId>
  <artifactId>spring-data-yugabytedb</artifactId>
  <version>2.1.10-yb-1</version>
</dependency>
```

### Data source configuration

To enable the YugabyteDB configuration, create configuration class:

```java
@Configuration
public class DatabaseConfiguration extends AbstractYugabyteConfiguration {
  // Here you can override the dataSource() method to configure the DataSource in code.
}
```

Configure your `application.properties`. For instance:

```
spring.yugabyte.initialHost=localhost
spring.yugabyte.port=5433
spring.yugabyte.database=yugabyte
spring.yugabyte.user=yugabyte
spring.yugabyte.password=yugabyte
spring.yugabyte.maxPoolSizePerNode=8
spring.yugabyte.connectionTimeoutMs=10000
spring.yugabyte.generate-ddl=true
spring.yugabyte.packages-to-scan=com.example.spring.jpa.springdatajpaexample.domain
```

To learn more about using the Spring Data YugabyteDB module, see [Spring Data YugabyteDB example](https://github.com/yugabyte/spring-data-yugabytedb-example).
