---
title: Build a Node.js application that uses YSQL
headerTitle: Build a Node.js application
linkTitle: Node.js
description: Build a Node.js application with the pg driver that uses YSQL.
aliases:
  - /develop/client-drivers/nodejs/
  - /latest/develop/client-drivers/nodejs/
  - /latest/develop/build-apps/nodejs/  
  - /latest/quick-start/build-apps/nodejs/
menu:
  latest:
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
    <a href="{{< relref "./ysql-pg.md" >}}" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - pg driver
    </a>
  </li>
  <li >
    <a href="{{< relref "./ysql-sequelize.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - Sequelize
    </a>
  </li>
  <li>
    <a href="{{< relref "./ycql.md" >}}" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the node-postgres driver

Install the [`node-postgres` driver](../../../../reference/drivers/ysql-client-drivers/#node-postgres) using the following command.

```sh
$ npm install pg
```

The `node-postgres` module is installed.

## Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB and created a universe. If not, follow the steps in [Quick Start](../../../../quick-start).
- installed a recent version of [`node`](https://nodejs.org/en/download/).
- installed the [async](https://github.com/caolan/async) JS utility to work with asynchronous Javascript. Install this by running the following command:

```sh
$ npm install --save async
```

## Create the sample Node.js application

Create a file `yb-ysql-helloworld.js` and copy the following content to it.

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
