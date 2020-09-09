---
title: Build a NodeJS application that uses YCQL
headerTitle: Build a NodeJS application
linkTitle: NodeJS
description: Build a NodeJS application that uses YCQL
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: NodeJS
    identifier: nodejs-3
    weight: 551
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/nodejs/ysql-pg" class="nav-link ">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - PG driver
    </a>
  </li>
  <li >
    <a href="/latest/quick-start/build-apps/nodejs/ysql-sequelize" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - Sequelize
    </a>
  </li>
  <li>
    <a href="/latest/quick-start/build-apps/nodejs/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the NodeJS driver

Install the YugabyteDB NodeJS driver for YCQL using the following command. You can find the source for the driver [here](https://github.com/yugabyte/cassandra-nodejs-driver).

```sh
$ npm install yb-ycql-driver
```

## Working example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe and are able to interact with it using the YCQL shell. If not, please follow these steps in the [quick start guide](../../../../api/ycql/quick-start/).
- installed a recent version of `node`. If not, you can find install instructions [here](https://nodejs.org/en/download/).

We will be using the [async](https://github.com/caolan/async) JS utility to work with asynchronous Javascript. Install this by running the following command:

```sh
$ npm install --save async
```

### Write the JavaScript code

Create a file `yb-cql-helloworld.js` and add the following content to it.

```js
const ycql = require('yb-ycql-driver');
const async = require('async');
const assert = require('assert');

// Create a YB CQL client.
// DataStax Nodejs 4.0 loadbalancing default is TokenAwarePolicy with child DCAwareRoundRobinPolicy
// Need to provide localDataCenter option below or switch to RoundRobinPolicy
const loadBalancingPolicy = new ycql.policies.loadBalancing.RoundRobinPolicy ();
const client = new ycql.Client({ contactPoints: ['127.0.0.1'], policies : { loadBalancing : loadBalancingPolicy }});

async.series([
  function connect(next) {
    client.connect(next);
  },
  function createKeyspace(next) {
    console.log('Creating keyspace ybdemo');
    client.execute('CREATE KEYSPACE IF NOT EXISTS ybdemo;', next);
  },
  function createTable(next) {
    // The create table statement.
    const create_table = 'CREATE TABLE IF NOT EXISTS ybdemo.employee (id int PRIMARY KEY, ' +
                                                                     'name varchar, ' +
                                                                     'age int, ' +
                                                                     'language varchar, ' +
                                                                     'location jsonb);';
    // Create the table.
    console.log('Creating table employee');
    client.execute(create_table, next);
  },
  function insert(next) {
    // Create a variable with the insert statement.
    const insert = "INSERT INTO ybdemo.employee (id, name, age, language) " +
                                        "VALUES (1, 'John', 35, 'NodeJS', '{ \"city\": \"San Francisco\", \"state\": \"California\", \"lat\": 37.77, \"long\": 122.42 }');";
    // Insert a row with the employee data.
    console.log('Inserting row with: %s', insert)
    client.execute(insert, next);
  },
  function select(next) {

    // Query the row for employee id 1 and print the results to the console.
    const select = 'SELECT name, age, language, location FROM ybdemo.employee WHERE id = 1;';
    client.execute(select, function (err, result) {
      if (err) return next(err);
      var row = result.first();
      const city = JSON.parse(row.location).city;
      console.log('Query for id=1 returned: name=%s, age=%d, language=%s, city=%s',
                                            row.name, row.age, row.language, city);
      next();
    });
  }
], function (err) {
  if (err) {
    console.error('There was an error', err.message, err.stack);
  }
  console.log('Shutting down');
  client.shutdown();
});
```

### Run the application

To run the application, type the following:

```sh
$ node yb-cql-helloworld.js
```

You should see the following output.

```
Creating keyspace ybdemo
Creating table employee
Inserting row with: INSERT INTO ybdemo.employee (id, name, age, language) VALUES (1, 'John', 35, 'NodeJS');
Query for id=1 returned: name=John, age=35, language=NodeJS
Shutting down
```
