---
title: Build a NodeJS application that uses Sequelize ORM and YSQL
headerTitle: Build a NodeJS application
linkTitle: NodeJS
description: Build a NodeJS application that uses Sequelize ORM and YSQL.
block_indexing: true
menu:
  stable:
    parent: build-apps
    name: NodeJS
    identifier: nodejs-2
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
    <a href="{{< relref "./ysql-sequelize.md" >}}" class="nav-link active">
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


## Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB and created a universe with YSQL enabled. If not, please follow these steps in the [Quick Start guide](../../../../quick-start/explore-ysql/).

## Clone the orm-examples repo

```sh
$ git clone https://github.com/yugabyte/orm-examples.git
```

This repository has a node.js example that implements a simple REST API server. The scenario is that of an e-commerce application. Database access in this application is managed through the Sequelize ORM. It consists of the following.

- The users of the e-commerce site are stored in the `users` table.
- The `products` table contains a list of products the e-commerce site sells.
- The orders placed by the users are populated in the `orders` table. An order can consist of multiple line items, each of these are inserted in the `orderline` table.

The source for the above application can be found in the [repo](https://github.com/yugabyte/orm-examples/tree/master/node/sequelize). There are a number of options that can be customized in the properties file located at `config/config.json`. 

## Build the application

```sh
$ cd ./node/sequelize/
```

```sh
npm install
```

## Run the application

Bring the NodeJS API server at http://localhost:8080 with DEBUG logs on.

```sh
$ DEBUG=sequelize:* npm start
```

## Send requests to the application

Create 2 users.

```sh
$ curl --data '{ "firstName" : "John", "lastName" : "Smith", "email" : "jsmith@yb.com" }' \
   -v -X POST -H 'Content-Type:application/json' http://localhost:8080/users
```
```sh
$ curl --data '{ "firstName" : "Tom", "lastName" : "Stewart", "email" : "tstewart@yb.com" }' \
   -v -X POST -H 'Content-Type:application/json' http://localhost:8080/users
```

Create 2 products.

```sh
$ curl \
  --data '{ "productName": "Notebook", "description": "200 page notebook", "price": 7.50 }' \
  -v -X POST -H 'Content-Type:application/json' http://localhost:8080/products
```

```sh
$ curl \
  --data '{ "productName": "Pencil", "description": "Mechanical pencil", "price": 2.50 }' \
  -v -X POST -H 'Content-Type:application/json' http://localhost:8080/products
```

Create 2 orders.

```sh
$ curl \
  --data '{ "userId": "2", "products": [ { "productId": 1, "units": 2 } ] }' \
  -v -X POST -H 'Content-Type:application/json' http://localhost:8080/orders
```

```sh
$ curl \
  --data '{ "userId": "2", "products": [ { "productId": 1, "units": 2 }, { "productId": 2, "units": 4 } ] }' \
  -v -X POST -H 'Content-Type:application/json' http://localhost:8080/orders
```

## Query results

### Using the YSQL shell

```sh
$ ./bin/ysqlsh 
```

```
ysqlsh (11.2)
Type "help" for help.

yugabyte=#
```

```postgresql
yugabyte=# SELECT count(*) FROM users;
```

```
 count 
-------
     2
(1 row)
```

```postgresql
yugabyte=# SELECT count(*) FROM products;
```

```
 count 
-------
     2
(1 row)
```

```postgresql
yugabyte=# SELECT count(*) FROM orders;
```

```
 count 
-------
     2
(1 row)
```

### Using the REST API

```sh
$ curl http://localhost:8080/users
```

```
{
  "content": [
    {
      "userId": 2,
      "firstName": "Tom",
      "lastName": "Stewart",
      "email": "tstewart@yb.com"
    },
    {
      "userId": 1,
      "firstName": "John",
      "lastName": "Smith",
      "email": "jsmith@yb.com"
    }
  ],
  ...
}  
```

```sh
$ curl http://localhost:8080/products
```
```
{
  "content": [
    {
      "productId": 2,
      "productName": "Pencil",
      "description": "Mechanical pencil",
      "price": 2.5
    },
    {
      "productId": 1,
      "productName": "Notebook",
      "description": "200 page notebook",
      "price": 7.5
    }
  ],
  ...
}  
```

```sh
$ curl http://localhost:8080/orders
```

```
{
  "content": [
    {
      "orderTime": "2019-05-10T04:26:54.590+0000",
      "orderId": "999ae272-f2f4-46a1-bede-5ab765bb27fe",
      "user": {
        "userId": 2,
        "firstName": "Tom",
        "lastName": "Stewart",
        "email": "tstewart@yb.com"
      },
      "userId": null,
      "orderTotal": 25,
      "products": []
    },
    {
      "orderTime": "2019-05-10T04:26:48.074+0000",
      "orderId": "1598c8d4-1857-4725-a9ab-14deb089ab4e",
      "user": {
        "userId": 2,
        "firstName": "Tom",
        "lastName": "Stewart",
        "email": "tstewart@yb.com"
      },
      "userId": null,
      "orderTotal": 15,
      "products": []
    }
  ],
  ...
}  
```

## Explore the source

As highlighted earlier, the source for the above application can be found in the [orm-examples repository](https://github.com/yugabyte/orm-examples/tree/master/node/sequelize).
