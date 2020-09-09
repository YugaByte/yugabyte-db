---
title: Build a Go application that uses GORM and YSQL
headerTitle: Build a Go application
linkTitle: Go
description: Build an Go application that uses GORM and YSQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: Go
    identifier: go-2
    weight: 552
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/go/ysql-pq" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - PQ
    </a>
  </li>
  <li >
    <a href="/latest/quick-start/build-apps/go/ysql-gorm" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - GORM
    </a>
  </li>
  <li>
    <a href="/latest/quick-start/build-apps/go/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

The following tutorial implements an an ORM example using [GORM](https://gorm.io/), the ORM library for Golang, that implements a simple REST API server. The scenario is that of an e-commerce application. Database access in this application is managed using GORM. The e-commerce database (`ysql_gorm`) includes the following tables:

- `users` table — the users of the e-commerce site
- `products` table — the products being sold
- `orders` table — the orders placed by the users
- `orderline` table — each line item of an order

The source for the above application can be found in the [repository](https://github.com/yugabyte/orm-examples/tree/master/golang/gorm). There are a number of options that can be customized in the properties file located at `src/config/config.json`.

## Before you begin

This tutorial assumes that you have satisfied the following prerequisites.

### YugabyteDB

YugabyteDB is up and running. If you are new to YugabyteDB, you can have YugabyteDB up and running within five minutes by following the steps in [Quick start](../../../../quick-start/).

### Go

Go 1.8, or later, is installed. The latest releases are available on the [Go Downloads page](https://golang.org/dl/).

### Go dependencies

To install the required Go dependencies, run the following commands.

```sh
go get github.com/jinzhu/gorm
go get github.com/jinzhu/gorm/dialects/postgres
go get github.com/google/uuid
go get github.com/gorilla/mux
go get github.com/lib/pq
go get github.com/lib/pq/hstore
```

## Clone the "orm-examples" repository

Clone the Yugabyte [`orm-examples` repository](https://github.com/yugabyte/orm-examples) by running the following command.

```sh
$ git clone https://github.com/yugabyte/orm-examples.git
```

Run the following `export` command to specify the `GOPATH` environment variable.

```sh
export GOPATH=$GOPATH:$HOME/orm-examples/golang/gorm
```

## Build and run the application

Change to the `gorm` directory.

```sh
$ cd ./golang/gorm
```

Create the `ysql_gorm` database in YugabyteDB by running the following `ysqlsh` command from the YugabyteDB home directory.

```sh
$ ./bin/ysqlsh -c "CREATE DATABASE ysql_gorm"
```

Build and start the REST API server by running the following shell script.

```sh
$ ./build-and-run.sh
```

The REST API server will start and listen for requests at `http://localhost:8080`.

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

```json
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

```json
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

```json
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

As mentioned earlier, the source for this application can be found in the Yugabyte [orm-examples](https://github.com/yugabyte/orm-examples/tree/master/golang/gorm) repository.
