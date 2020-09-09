---
title: Build a Python application using Apache Spark and YugabyteDB
headerTitle: Apache Spark
linkTitle: Apache Spark
description: Build a Python application using Apache Spark and YugabyteDB
block_indexing: true
menu:
  v2.1:
    identifier: apache-spark-3-python
    parent: ecosystem-integrations
    weight: 572

---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/develop/ecosystem-integrations/apache-spark/scala" class="nav-link">
      <i class="icon-scala" aria-hidden="true"></i>
      Scala
    </a>
  </li>

  <li >
    <a href="/latest/develop/ecosystem-integrations/apache-spark/java" class="nav-link">
      <i class="icon-java-bold" aria-hidden="true"></i>
      Java
    </a>
  </li>

  <li >
    <a href="/latest/develop/ecosystem-integrations/apache-spark/python" class="nav-link active">
      <i class="icon-python" aria-hidden="true"></i>
      Python
    </a>
  </li>

</ul>

## PySpark

To build your Python application using the YugabyteDB Spark Connector for YCQL, start PySpark with the following for Scala 2.11:

```sh
$ pyspark --packages com.yugabyte.spark:spark-cassandra-connector_2.11:2.4-yb-2
```
