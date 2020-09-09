---
title: Build a NodeJS application that uses YSQL
headerTitle: Build a NodeJS application
linkTitle: NodeJS
description: Build a NodeJS application that uses the pg driver and YSQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: NodeJS
    identifier: nodejs-1
    weight: 551
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/nodejs/ysql-pg" class="nav-link active">
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
    <a href="/latest/quick-start/build-apps/nodejs/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the pg driver

Install the NodeJS driver using the following command. You can find further details and the source for the driver [here](https://node-postgres.com/).

```sh
$ npm install pg
```

## Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB and created a universe. If not, please follow these steps in the [Quick Start guide](../../../../quick-start/explore-ysql/).
- installed a recent version of `node`. If not, you can find install instructions [here](https://nodejs.org/en/download/).

We will be using the [async](https://github.com/caolan/async) JS utility to work with asynchronous Javascript. Install this by running the following command:

```sh
$ npm install --save async
```

## Sample JavaScript code

Create a file `yb-ysql-helloworld.js` and add the following content to it.

```js
var pg = require('pg');
const async = require('async');
const assert = require('assert');

var conString = "postgres://postgres@localhost:5433/postgres";
var client = new pg.Client(conString);

async.series([
  function connect(next) {
    client.connect(next);
  },
  function createTable(next) {
    // The create table statement.
    const create_table = 'CREATE TABLE employee (id int PRIMARY KEY, ' +
                                                 'name varchar, ' +
                                                 'age int, ' +
                                                 'language varchar);';
    // Create the table.
    console.log('Creating table employee');
    client.query(create_table, next);
  },
  function insert(next) {
    // Create a variable with the insert statement.
    const insert = "INSERT INTO employee (id, name, age, language) " +
                                        "VALUES (1, 'John', 35, 'NodeJS');";
    // Insert a row with the employee data.
    console.log('Inserting row with: %s', insert)
    client.query(insert, next);
  },
  function select(next) {
    // Query the row for employee id 1 and print the results to the console.
    const select = 'SELECT name, age, language FROM employee WHERE id = 1;';
    client.query(select, function (err, result) {
      if (err) return next(err);
      var row = result.rows[0];
      console.log('Query for id=1 returned: name=%s, age=%d, language=%s',
                                            row.name, row.age, row.language);
      next();
    });
  }
], function (err) {
  if (err) {
    console.error('There was an error', err.message, err.stack);
  }
  console.log('Shutting down');
  client.end();
});
```

## Run the application

To run the application, type the following:

```sh
$ node yb-ysql-helloworld.js
```

You should see the following output.

```
Creating table employee
Inserting row with: INSERT INTO employee (id, name, age, language) VALUES (1, 'John', 35, 'NodeJS');
Query for id=1 returned: name=John, age=35, language=NodeJS
Shutting down
```
