---
title: Build a Python application that uses YSQL and aiopg
headerTitle: Build a Python application
linkTitle: Python
description: Build a sample Python application with aiopg that uses YSQL.
menu:
  latest:
    parent: build-apps
    name: Python
    identifier: python-aiopg
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
    <a href="{{< relref "./ysql-aiopg.md" >}}" class="nav-link active">
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
    <a href="{{< relref "./ycql.md" >}}" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

The following tutorial creates a simple Python application that connects to a YugabyteDB cluster using the `aiopg
` database adapter, performs a few basic database operations — creating a table, inserting data, and running a SQL query — and prints the results to the screen.

## Before you begin

This tutorial assumes that you have satisfied the following prerequisites.

### YugabyteDB

YugabyteDB is up and running. If you are new to YugabyteDB, you can have YugabyteDB up and running within five minutes by following the steps in [Quick start](../../../../quick-start/).

### Python

Python 3, or later, is installed.

### aiopg database adapter

[aiopg](https://aiopg.readthedocs.io/en/stable/) package is installed. To install the package using `pip3` run:

```sh
$ pip3 install aiopg
```

For details about using this database adapter, see [aiopg documentation](https://aiopg.readthedocs.io/en/stable/).

## Create the sample Python application

Create a file `yb-sql-helloworld.py` and add the following content to it.

```python
import asyncio
import aiopg

dsn = 'dbname=yugabyte user=yugabyte password=yugabyte host=127.0.0.1 port=5433'


async def go():
    async with aiopg.create_pool(dsn) as pool:
        async with pool.acquire() as conn:
            # Open a cursor to perform database operations.
            async with conn.cursor() as cur:
                await cur.execute(f"""
                  DROP TABLE IF EXISTS employee;
                  CREATE TABLE employee (id int PRIMARY KEY,
                             name varchar,
                             age int,
                             language varchar);
                """)
                print("Created table employee")
                # Insert a row.
                await cur.execute("INSERT INTO employee (id, name, age, language) VALUES (%s, %s, %s, %s)",
                                  (1, 'John', 35, 'Python'))
                print("Inserted (id, name, age, language) = (1, 'John', 35, 'Python')")

                # Query the row.
                await cur.execute("SELECT name, age, language FROM employee WHERE id = 1")
                async for row in cur:
                    print("Query returned: %s, %s, %s" % (row[0], row[1], row[2]))


loop = asyncio.get_event_loop()
loop.run_until_complete(go())
```

### Run the application

To use the application, run the following Python script you just created.

```sh
$ python yb-sql-helloworld.py
```

You should see the following output.

```
Created table employee
Inserted (id, name, age, language) = (1, 'John', 35, 'Python')
Query returned: John, 35, Python
```
