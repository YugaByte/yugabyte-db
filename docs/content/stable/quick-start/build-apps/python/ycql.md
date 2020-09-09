---
title: Build a Python application that uses YCQL
headerTitle: Build a Python application
linkTitle: Python
description: Build a Python application with the Yugabyte Python Driver for YCQL.
block_indexing: true
menu:
  stable:
    parent: build-apps
    name: Python
    identifier: python-3
    weight: 553
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="{{< relref "./ysql-psycopg2.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - psycopg2
    </a>
  </li>
  <li >
    <a href="{{< relref "./ysql-aiopg.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - aiopg
    </a>
  </li>
  <li >
    <a href="{{< relref "./ysql-sqlalchemy.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - SQL Alchemy
    </a>
  </li>
  <li>
    <a href="" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the Yugabyte Python Driver for YCQL

To install the [Yugabyte Python Driver for YCQL](https://github.com/yugabyte/cassandra-python-driver), run the following command:

```sh
$ pip install yb-cassandra-driver
```

## Create a sample Python application

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the YCQL shell. If not, follow the steps in [Quick start YCQL](../../../../api/ycql/quick-start/).

### Write the sample Python application

Create a file `yb-cql-helloworld.py` and copy the following content into it.

```python
from cassandra.cluster import Cluster

# Create the cluster connection.
cluster = Cluster(['127.0.0.1'])
session = cluster.connect()

# Create the keyspace.
session.execute('CREATE KEYSPACE IF NOT EXISTS ybdemo;')
print("Created keyspace ybdemo")

# Create the table.
session.execute(
  """
  CREATE TABLE IF NOT EXISTS ybdemo.employee (id int PRIMARY KEY,
                                              name varchar,
                                              age int,
                                              language varchar);
  """)
print("Created table employee")

# Insert a row.
session.execute(
  """
  INSERT INTO ybdemo.employee (id, name, age, language)
  VALUES (1, 'John', 35, 'NodeJS');
  """)
print("Inserted (id, name, age, language) = (1, 'John', 35, 'Python')")

# Query the row.
rows = session.execute('SELECT name, age, language FROM ybdemo.employee WHERE id = 1;')
for row in rows:
  print(row.name, row.age, row.language)

# Close the connection.
cluster.shutdown()
```

### Run the application

To run the application, type the following:

```sh
$ python yb-cql-helloworld.py
```

You should see the following output.

```
Created keyspace ybdemo
Created table employee
Inserted (id, name, age, language) = (1, 'John', 35, 'Python')
John 35 Python
```
