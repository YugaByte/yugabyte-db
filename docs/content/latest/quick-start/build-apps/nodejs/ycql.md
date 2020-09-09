---
title: Build a Node.js application that uses YCQL
headerTitle: Build a Node.js application
linkTitle: Node.js
description: Build a sample Node.js application with the Yugabyte Node.js driver for YCQL.
menu:
  latest:
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
    <a href="{{< relref "./ysql-pg.md" >}}" class="nav-link ">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - PG driver
    </a>
  </li>
  <li >
    <a href="{{< relref "./ysql-sequelize.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - Sequelize
    </a>
  </li>
  <li>
    <a href="{{< relref "./ycql.md" >}}" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the Yugabyte Node.js Driver for YCQL

To install the [YugabyteDB Node.js driver for YCQL](https://github.com/yugabyte/cassandra-nodejs-driver), run the following `npm install` command:

```sh
$ npm install yb-ycql-driver
```

## Create the sample Node.js application

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the YCQL shell. If not, follow these steps in [Quick start YCQL guide](../../../../api/ycql/quick-start/).
- installed a recent version of [Node.js](https://nodejs.org/en/download/).
- installed the JavaScript [async](https://github.com/caolan/async) utility to work with asynchronous Javascript. To install `async`, run the following `npm install` command:

```sh
$ npm install --save async
```

### Write the sample Node.js application

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

To use the application, run the following command:

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
