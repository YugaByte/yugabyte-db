---
title: Explore YCQL, the Yugabyte Cloud QL API
headerTitle: 3. Explore Yugabyte Cloud QL
linkTitle: 3. Explore distributed SQL APIs
description: Explore Yugabyte Cloud QL (YCQL), a semi-relational distributed SQL API
image: /images/section_icons/quick_start/explore_ycql.png
aliases:
  - /quick-start/test-cassandra/
  - /latest/quick-start/test-cassandra/
  - /latest/quick-start/test-ycql/
  - /latest/api/ycql/quick-start/
menu:
  latest:
    parent: quick-start
    name: 3. Explore distributed SQL
    identifier: explore-dsql-2-ycql
    weight: 130
type: page
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/explore/ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

 <li >
    <a href="/latest/quick-start/explore/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
  
</ul>

After [creating a local cluster](../../create-local-cluster/), follow the instructions below to explore YugabyteDB's semi-relational [Yugabyte Cloud QL](../../../api/ycql) API.

[**ycqlsh**](../../../admin/ycqlsh/) is the command line shell for interacting with the YCQL API. You will use ycqlsh for this tutorial.

## 1. Connect with ycqlsh

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#macos" class="nav-link active" id="macos-tab" data-toggle="tab" role="tab" aria-controls="macos" aria-selected="true">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>
  <li>
    <a href="#linux" class="nav-link" id="linux-tab" data-toggle="tab" role="tab" aria-controls="linux" aria-selected="false">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>
  <li>
    <a href="#docker" class="nav-link" id="docker-tab" data-toggle="tab" role="tab" aria-controls="docker" aria-selected="false">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>
  <li >
    <a href="#kubernetes" class="nav-link" id="kubernetes-tab" data-toggle="tab" role="tab" aria-controls="kubernetes" aria-selected="false">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="macos" class="tab-pane fade show active" role="tabpanel" aria-labelledby="macos-tab">
    {{% includeMarkdown "binary/explore-ycql.md" /%}}
  </div>
  <div id="linux" class="tab-pane fade" role="tabpanel" aria-labelledby="linux-tab">
    {{% includeMarkdown "binary/explore-ycql.md" /%}}
  </div> 
  <div id="docker" class="tab-pane fade" role="tabpanel" aria-labelledby="docker-tab">
    {{% includeMarkdown "docker/explore-ycql.md" /%}}
  </div>
  <div id="kubernetes" class="tab-pane fade" role="tabpanel" aria-labelledby="kubernetes-tab">
    {{% includeMarkdown "kubernetes/explore-ycql.md" /%}}
  </div>
</div>

## 2. Create a table

Create a keyspace called 'myapp'.

```sql
ycqlsh> CREATE KEYSPACE myapp;
```

Create a table named `stock_market'`, which can store stock prices at various timestamps for different stock ticker symbols.

```sql
ycqlsh> CREATE TABLE myapp.stock_market (
  stock_symbol text,
  ts text,
  current_price float,
  PRIMARY KEY (stock_symbol, ts)
);
```

## 3. Insert data

Let us insert some data for a few stock symbols into our newly created 'stock_market' table. You can copy-paste these values directly into your ycqlsh shell.

```sql
ycqlsh> INSERT INTO myapp.stock_market (stock_symbol,ts,current_price) VALUES ('AAPL','2017-10-26 09:00:00',157.41);
INSERT INTO myapp.stock_market (stock_symbol,ts,current_price) VALUES ('AAPL','2017-10-26 10:00:00',157);
```

```sql
ycqlsh> INSERT INTO myapp.stock_market (stock_symbol,ts,current_price) VALUES ('FB','2017-10-26 09:00:00',170.63);
INSERT INTO myapp.stock_market (stock_symbol,ts,current_price) VALUES ('FB','2017-10-26 10:00:00',170.1);
```

```sql
ycqlsh> INSERT INTO myapp.stock_market (stock_symbol,ts,current_price) VALUES ('GOOG','2017-10-26 09:00:00',972.56);
INSERT INTO myapp.stock_market (stock_symbol,ts,current_price) VALUES ('GOOG','2017-10-26 10:00:00',971.91);
```

## 4. Query the table

Query all the values you have inserted into the database for the stock symbol 'AAPL' as follows.

```sql
ycqlsh> SELECT * FROM myapp.stock_market WHERE stock_symbol = 'AAPL';
```

```
 stock_symbol | ts                  | current_price
--------------+---------------------+---------------
         AAPL | 2017-10-26 09:00:00 |        157.41
         AAPL | 2017-10-26 10:00:00 |           157

(2 rows)
```

Query all the values for `FB` and `GOOG` as follows.

```sql
ycqlsh> SELECT * FROM myapp.stock_market WHERE stock_symbol in ('FB', 'GOOG');
```

```
 stock_symbol | ts                  | current_price
--------------+---------------------+---------------
           FB | 2017-10-26 09:00:00 |        170.63
           FB | 2017-10-26 10:00:00 |     170.10001
         GOOG | 2017-10-26 09:00:00 |        972.56
         GOOG | 2017-10-26 10:00:00 |     971.90997

(4 rows)
```


{{<tip title="Next step" >}}

[Build an application](../../build-apps/)

{{< /tip >}}
