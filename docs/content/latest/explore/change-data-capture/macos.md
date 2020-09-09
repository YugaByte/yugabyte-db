---
title: Explore change data capture (CDC) on macOS
headerTitle: Change data capture (CDC)
linkTitle: Change data capture (CDC)
description: Use a local YugabyteDB cluster (on macOS) to stream data changes to stdout using the CDC API.
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
aliases:
  - /latest/deploy/cdc/cdc-to-stdout/
  - /latest/explore/change-data-capture/
  - /latest/explore/change-data-capture-macos/
menu:
  latest:
    identifier: change-data-capture-1-macos
    parent: explore
    weight: 249
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/change-data-capture/macos" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/change-data-capture/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

</ul>

[Change data capture (CDC)](../../../architecture/cdc-architecture) can be used to asynchronously stream data changes from a YugabyteDB cluster to external systems like message queues and OLAP warehouses. The data changes in YugabyteDB are detected, captured, and then output to the specified target.  In the steps below, you will use a local YugabyteDB cluster to stream data changes to `stdout` using the CDC API.

This tutorial uses the [yb-ctl](../../../admin/yb-ctl) local cluster management utility.

## 1. Create a universe

```sh
$ ./bin/yb-ctl create
```

## 2. Add a database table

Start your local YugabyteDB cluster and run `ysqlsh` to connect to the service.

```sh
$ ./bin/ysqlsh 
```

Add a table, named `products`, to the default `yugabyte` database.

```postgresql
CREATE TABLE products(
  id         bigserial PRIMARY KEY,
  created_at timestamp,
  category   text,
  ean        text,
  price      float,
  quantity   int default(5000),
  rating     float,
  title      text,
  vendor     text
);
```

## 3. Download the CDC Connector for stdout

Download the stdout CDC Connector JAR file.

```sh
$ wget https://downloads.yugabyte.com/yb-cdc-connector.jar
```

## 4. Stream the log output stream to stdout

Run the command below to to start logging an output stream of data changes from the YugabyteDB `cdc` table to `stdout`.

```sh
java -jar yb-cdc-connector.jar --table_name yugabyte.products 
```

The example above uses the following parameters:

- `--table_name` — Specifies the namespace and table, where namespace is the database (YSQL) or keyspace (YCQL).
- `--master_addrs` — Specifies the IP addresses for all of the YB-Master servers that are producing or consuming. Default value is `127.0.0.1:7100`. If you are using a 3-node local cluster, then you need to specify a comma-delimited list of the addresses for all of your YB-Master servers.

## 5. Insert values and observe

In another terminal shell, write some values to the table and observe the values on your `stdout` output stream.

```postgresql
INSERT INTO products (
  id, 
  category, 
  created_at, 
  ean, 
  price, 
  rating, 
  title, 
  vendor) 
VALUES (
  14, 
  'Widget', 
  '2017-12-31T14:41:56.870Z', 
  8833419218504, 
  25.09876359271891, 
  4.0, 
  'Awesome Concrete Shoes', 
  'McClure-Lockman'); 
```

```
2020-01-16 14:52:01,597 [INFO|org.yb.cdc.LogClient|LogClient] time: 6468465138045198336
operation: WRITE
key {
  key: "id"
  value {
    int64_value: 14
  }
}
changes {
  key: "created_at"
  value {
    int64_value: 568046516870000
  }
}
changes {
  key: "category"
  value {
    string_value: "Widget"
  }
}
changes {
  key: "ean"
  value {
    string_value: "8833419218504"
  }
}
changes {
  key: "price"
  value {
    double_value: 25.09876359271891
  }
}
changes {
  key: "quantity"
  value {
    int32_value: 5000
  }
}
changes {
  key: "rating"
  value {
    double_value: 4.0
  }
}
changes {
  key: "title"
  value {
    string_value: "Awesome Concrete Shoes"
  }
}
changes {
  key: "vendor"
  value {
    string_value: "McClure-Lockman"
  }
}
```
