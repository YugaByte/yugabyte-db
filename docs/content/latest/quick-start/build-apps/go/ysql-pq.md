---
title: Build a Go application that uses YSQL
headerTitle: Build a Go application
linkTitle: Go
description: Build a sample Go application with the Go PostgreSQL driver and perform basic database operations.
aliases:
  - /develop/client-drivers/go/
  - /latest/develop/client-drivers/go/
  - /latest/develop/build-apps/go/
  - /latest/quick-start/build-apps/go/
menu:
  latest:
    parent: build-apps
    name: Go
    identifier: go-1
    weight: 552
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/go/ysql-pq" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - PQ
    </a>
  </li>
  <li >
    <a href="/latest/quick-start/build-apps/go/ysql-gorm" class="nav-link">
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

The following tutorial creates a simple Go application that connects to a YugabyteDB cluster using the [Go PostgreSQL driver](https://godoc.org/github.com/lib/pq), performs a few basic database operations — creating a table, inserting data, and running a SQL query — and then prints the results to the screen.

## Before you begin

This tutorial assumes that you have satisfied the following prerequisites.

### YugabyteDB

YugabyteDB is up and running. If not, please follow these steps in the [Quick Start guide](../../../../quick-start/explore-ysql/).

### Go

[Go version 1.8](https://golang.org/dl/), or later, is installed.

### Go PostgreSQL driver

The [Go PostgreSQL driver package (`pq`)](https://godoc.org/github.com/lib/pq) is a Go PostgreSQL driver for the `database/sql` package.

To install the package locally, run the following command:

```sh
$ go get github.com/lib/pq
```

## Create the sample Go application

Create a file `ybsql_hello_world.go` and copy the contents below.

```go
package main

import (
  "database/sql"
  "fmt"
  "log"

  _ "github.com/lib/pq"
)

const (
  host     = "127.0.0.1"
  port     = 5433
  user     = "yugabyte"
  password = "yugabyte"
  dbname   = "yugabyte"
)

func main() {
    psqlInfo := fmt.Sprintf("host=%s port=%d user=%s "+
                            "password=%s dbname=%s sslmode=disable",
                            host, port, user, password, dbname)
    db, err := sql.Open("postgres", psqlInfo)
    if err != nil {
        log.Fatal(err)
    }

    var createStmt = `CREATE TABLE employee (id int PRIMARY KEY,
                                             name varchar,
                                             age int,
                                             language varchar)`;
    if _, err := db.Exec(createStmt); err != nil {
        log.Fatal(err)
    }
    fmt.Println("Created table employee")

    // Insert into the table.
    var insertStmt string = "INSERT INTO employee(id, name, age, language)" +
        " VALUES (1, 'John', 35, 'Go')";
    if _, err := db.Exec(insertStmt); err != nil {
        log.Fatal(err)
    }
    fmt.Printf("Inserted data: %s\n", insertStmt)

    // Read from the table.
    var name string
    var age int
    var language string
    rows, err := db.Query(`SELECT name, age, language FROM employee WHERE id = 1`)
    if err != nil {
        log.Fatal(err)
    }
    defer rows.Close()
    fmt.Printf("Query for id=1 returned: ");
    for rows.Next() {
        err := rows.Scan(&name, &age, &language)
        if err != nil {
           log.Fatal(err)
        }
        fmt.Printf("Row[%s, %d, %s]\n", name, age, language)
    }
    err = rows.Err()
    if err != nil {
        log.Fatal(err)
    }

    defer db.Close()
}
```

## Run the application

To use the application, run the following command:

```sh
$ go run ybsql_hello_world.go
```

You should see the following output.

```
Created table employee
Inserted data: INSERT INTO employee(id, name, age, language) VALUES (1, 'John', 35, 'Go')
Query for id=1 returned: Row[John, 35, Go]
```
