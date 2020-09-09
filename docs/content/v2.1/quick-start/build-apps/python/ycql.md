---
title: Build a Python application that uses YCQL
headerTitle: Build a Python application
linkTitle: Python
description: Build a Python application that uses YCQL.
block_indexing: true
menu:
  v2.1:
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
    <a href="/latest/quick-start/build-apps/python/ysql-psycopg2" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - Psycopg2
    </a>
  </li>
  <li >
    <a href="/latest/quick-start/build-apps/python/ysql-sqlalchemy" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - SQL Alchemy
    </a>
  </li>
  <li>
    <a href="/latest/quick-start/build-apps/python/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>


## Installation

Install the python driver using the following command.

```sh
$ pip install yb-cassandra-driver
```

## Working Example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe and are able to interact with it using the YCQL shell. If not, please follow these steps in the [quick start guide](../../../../api/ycql/quick-start/).


### Writing the python code

Create a file `yb-cql-helloworld.py` and add the following content to it.

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

### Running the application

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
