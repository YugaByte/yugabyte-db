---
title: Explore YSQL, the Yugabyte SQL API
headerTitle: 3. Explore Yugabyte SQL 
linkTitle: 3. Explore distributed SQL APIs
description: Explore Yugabyte SQL (YSQL), a PostgreSQL-compatible fully-relational distributed SQL API
image: /images/section_icons/quick_start/explore_ysql.png
aliases:
  - /stable/quick-start/explore-ysql/
  - /stable/quick-start/explore/
  - /quick-start/test-postgresql/
  - /stable/quick-start/test-postgresql/
  - /stable/quick-start/test-ysql/
  - /stable/explore/postgresql/joins/
  - /stable/explore/postgresql/aggregations/
  - /stable/explore/postgresql/expressions/
  - /stable/explore/postgresql/views/
block_indexing: true
menu:
  stable:
    parent: quick-start
    name: 3. Explore distributed SQL
    identifier: explore-dsql-1-ysql
    weight: 130
type: page
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/quick-start/explore/ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

 <li >
    <a href="/stable/quick-start/explore/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
  
</ul>

After [creating a local cluster](../../create-local-cluster/), you can start exploring YugabyteDB's PostgreSQL-compatible, fully-relational [Yugabyte SQL API](../../../api/ysql/).

[**ysqlsh**](../../../admin/ysqlsh/) is the command line shell for interacting with the YSQL API. You will use `ysqlsh` for this tutorial.

## 1. Load sample data

Follow the steps below to create a database and load sample data.

{{< note title="Note" >}}

The five SQL scripts (aka `.sql` files) used to create and load the sample data in the steps below are located in the `share` directory of your YugabyteDB installation. You can verify the files are available by entering the following `ls` command from the YugabyteDB home directory.

```sh
$ ls share/
```

The `share` directory includes sample dataset files available for creating databases for learning YugabyteDB. The files that will be used in the steps below are `schema.sql`, `orders.sql`, `products.sql`,`reviews.sql` and `users.sql`.

{{< /note >}}

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
    {{% includeMarkdown "binary/explore-ysql.md" /%}}
  </div>
  <div id="linux" class="tab-pane fade" role="tabpanel" aria-labelledby="linux-tab">
    {{% includeMarkdown "binary/explore-ysql.md" /%}}
  </div>
  <div id="docker" class="tab-pane fade" role="tabpanel" aria-labelledby="docker-tab">
    {{% includeMarkdown "docker/explore-ysql.md" /%}}
  </div>
  <div id="kubernetes" class="tab-pane fade" role="tabpanel" aria-labelledby="kubernetes-tab">
    {{% includeMarkdown "kubernetes/explore-ysql.md" /%}}
  </div>
</div>

1. Create a database (`yb_demo`) by using the following `CREATE DATABASE` command.

    ```postgresql
    yugabyte=# CREATE DATABASE yb_demo;
    ```

2. Connect to the new database using the following YSQL shell `\c` meta command.

    ```postgresql
    yugabyte=# \c yb_demo;
    ```

3. Create the database schema, which includes four tables, by running the following `\i` meta command.

    ```postgresql
    yb_demo=# \i share/schema.sql;
    ```

4. Load the data into the tables by running the following four `\i` commands.

    ```postgresql
    yb_demo=# \i share/products.sql
    ```

    ```postgresql
    yb_demo=# \i share/users.sql
    ```

    ```postgresql
    yb_demo=# \i share/orders.sql
    ```

    ```postgresql
    yb_demo=# \i share/reviews.sql
    ```

    You now have sample data and are ready to begin exploring YSQL in YugabyteDB.

## 2. Simple queries

Lets us look at the schema of the `products` table. You can do this as follows:

```postgresql
yb_demo=# \d products
```

You should see an output like the following:

```
                                        Table "public.products"
   Column   |            Type             | Collation | Nullable |               Default                
------------+-----------------------------+-----------+----------+--------------------------------------
 id         | bigint                      |           | not null | nextval('products_id_seq'::regclass)
 created_at | timestamp without time zone |           |          | 
 category   | text                        |           |          | 
 ean        | text                        |           |          | 
 price      | double precision            |           |          | 
 quantity   | integer                     |           |          | 5000
 rating     | double precision            |           |          | 
 title      | text                        |           |          | 
 vendor     | text                        |           |          | 
Indexes:
    "products_pkey" PRIMARY KEY, lsm (id HASH)
```

To see how many products there are in this table, you can run the following query.

```postgresql
yb_demo=# SELECT count(*) FROM products;
```

You should see an output which looks like the following:

```
 count
-------
   200
(1 row)
```

Now let us run a query to select the `id`, `title`, `category` and `price` columns for the first five products.

```postgresql
yb_demo=# SELECT id, title, category, price, rating
          FROM products
          LIMIT 5;
```

You should see an output like the following:

```
 id  |           title            | category |      price       | rating 
-----+----------------------------+----------+------------------+--------
  22 | Enormous Marble Shoes      | Gizmo    | 21.4245199604423 |    4.2
  38 | Lightweight Leather Gloves | Gadget   | 44.0462485589292 |    3.8
 162 | Gorgeous Copper Knife      | Gadget   | 22.3785988001101 |    3.3
 174 | Rustic Iron Keyboard       | Gadget   | 74.4095392945406 |    4.4
  46 | Rustic Linen Keyboard      | Gadget   | 78.6996782532274 |      4
(5 rows)
```

To view the next 3 products, you simply add an `OFFSET 5` clause to start from the fifth product.

```postgresql
yb_demo=# SELECT id, title, category, price, rating
          FROM products
          LIMIT 3 OFFSET 5;
```

You should see an output which looks like the following:

```
 id  |           title           | category  |      price       | rating 
-----+---------------------------+-----------+------------------+--------
 152 | Enormous Aluminum Clock   | Widget    | 32.5971248660044 |    3.6
   3 | Synergistic Granite Chair | Doohickey | 35.3887448815391 |      4
 197 | Aerodynamic Concrete Lamp | Gizmo     | 46.7640712447334 |    4.6
(3 rows)
```

## 3. JOINs

A JOIN clause is used to combine rows from two or more tables, based on a related column between them. Let us do this by combining some orders with the information of the corresponding users that placed the order.

From the `orders` table, you are going to select the `total` column that represents the total amount the user paid. For each of these orders, you are going to fetch the `id`, the `name` and the `email` from the `users` table of the corresponding users that placed those orders. The related column between the two tables is the user's id. This can be expressed as the following join query:

```postgresql
yb_demo=# SELECT users.id, users.name, users.email, orders.id, orders.total
          FROM orders INNER JOIN users ON orders.user_id=users.id
          LIMIT 10;
```

You should see something like the following:

```
  id  |        name         |             email             |  id   |      total
------+---------------------+-------------------------------+-------+------------------
  616 | Rex Thiel           | rex-thiel@gmail.com           |  4443 | 101.414602060277
 2289 | Alanis Kovacek      | alanis.kovacek@yahoo.com      | 17195 | 71.8499366564206
   37 | Jaleel Collins      | jaleel.collins@gmail.com      |   212 | 38.8821451022809
 2164 | Cordia Farrell      | cordia.farrell@gmail.com      | 16223 | 37.7489430287531
 1528 | Donny Murazik       | murazik-donny@hotmail.com     | 11546 | 52.3082273751586
 1389 | Henriette O'Connell | connell-o-henriette@yahoo.com | 10551 | 69.3117644687696
 2408 | Blake Jast          | jast.blake@hotmail.com        | 18149 | 150.788925887077
 1201 | Kaycee Keebler      | kaycee-keebler@gmail.com      |  8937 | 48.3440955866708
 1421 | Cornell Cartwright  | cornell-cartwright@gmail.com  | 10772 | 191.867670306882
  523 | Deonte Hoeger       | hoeger.deonte@hotmail.com     |  3710 | 71.4010754169826
(10 rows)
```

## 4. Distributed transactions

In order to track the quantities accurately, each product being ordered in some quantity by a user has to decrement the corresponding product inventory quantity. These operations should be performed inside a transaction.

Imagine the user with id `1` wants to order for `10` units of the product with id `2`.

Before running the transaction, you can verify that you have `5000` units of product `2` in stock by running the following query:

```postgresql
yb_demo=# SELECT id, category, price, quantity FROM products WHERE id=2;
```

```
SELECT id, category, price, quantity FROM products WHERE id=2;
 id | category  |      price       | quantity
----+-----------+------------------+----------
  2 | Doohickey | 70.0798961307176 |     5000
(1 row)
```

Now, to place the order, you can run the following transaction:

```postgresql
yb_demo=# BEGIN TRANSACTION;

/* First insert a new order into the orders table. */
INSERT INTO orders
  (id, created_at, user_id, product_id, discount, quantity, subtotal, tax, total)
VALUES (
  (SELECT max(id)+1 FROM orders)                 /* id */,
  now()                                          /* created_at */,
  1                                              /* user_id */,
  2                                              /* product_id */, 
  0                                              /* discount */,
  10                                             /* quantity */,
  (10 * (SELECT price FROM products WHERE id=2)) /* subtotal */,
  0                                              /* tax */,
  (10 * (SELECT price FROM products WHERE id=2)) /* total */
) RETURNING id;

/* Next decrement the total quantity from the products table. */
UPDATE products SET quantity = quantity - 10 WHERE id = 2;

COMMIT;
```

We can verify that the order got inserted by running the following:

```postgresql
yb_demo=# select * from orders where id = (select max(id) from orders);
```

```
  id   |         created_at         | user_id | product_id | discount | quantity |     subtotal     | tax |      total       
-------+----------------------------+---------+------------+----------+----------+------------------+-----+------------------
 18761 | 2020-01-30 09:24:29.784078 |       1 |          2 |        0 |       10 | 700.798961307176 |   0 | 700.798961307176
(1 row)
```

We can also verify that total quantity of product id `2` in the inventory is `4990` by running the following query.

```postgresql
yb_demo=# SELECT id, category, price, quantity FROM products WHERE id=2;
```

```
 id | category  |      price       | quantity
----+-----------+------------------+----------
  2 | Doohickey | 70.0798961307176 |     4990
(1 row)
```

## 5. Built-in functions

YSQL supports a rich set of built-in functions. In this example, you will look at some functions such as `DISTINCT`, `MIN`, `MAX` and `AVG` in the context of the data set.

- How are users signing up for my site?

To answer this question, you should list the unique set of `source` channels present in the database. This can be achieved as follows:

```postgresql
yb_demo=# SELECT DISTINCT(source) FROM users;
```

```
source
-----------
 Facebook
 Twitter
 Organic
 Affiliate
 Google
(5 rows)
```

- What is the min, max and average price of products in the store?

```postgresql
yb_demo=# SELECT MIN(price), MAX(price), AVG(price) FROM products;
```

```
min               |       max        |       avg
------------------+------------------+------------------
 15.6919436739704 | 98.8193368436819 | 55.7463996679207
(1 row)
```

## 6. Aggregations

The `GROUP BY` clause is commonly used to perform aggregations. Below are a couple of examples of using these to answer some types of questions about the data.

- What is the most effective channel for user signups?

```postgresql
yb_demo=# SELECT source, count(*) AS num_user_signups
          FROM users
          GROUP BY source
          ORDER BY num_user_signups DESC;
```

```
source     | num_user_signups
-----------+------------------
 Facebook  |              512
 Affiliate |              506
 Google    |              503
 Twitter   |              495
 Organic   |              484
(5 rows)
```

- What are the most effective channels for product sales by revenue?

```postgresql
yb_demo=# SELECT source, ROUND(SUM(orders.total)) AS total_sales
          FROM users LEFT JOIN orders ON users.id=orders.user_id
          GROUP BY source
          ORDER BY total_sales DESC;
```

```
  source   | total_sales
-----------+-------------
 Facebook  |      333454
 Google    |      325184
 Twitter   |      320150
 Organic   |      319637
 Affiliate |      297605
(5 rows)
```

## 7. Views

Let us answer the questions below by creating a view.

- What percentage of the total sales is from the Facebook channel?

```postgresql
yb_demo=# CREATE VIEW channel AS
            (SELECT source, ROUND(SUM(orders.total)) AS total_sales
             FROM users LEFT JOIN orders ON users.id=orders.user_id
             GROUP BY source
             ORDER BY total_sales DESC);
```

Now that the view is created, you can see it in our list of relations.

```postgresql
yb_demo=# \d
```

```
               List of relations
 Schema |      Name       |   Type   |  Owner
--------+-----------------+----------+----------
 public | channel         | view     | yugabyte
 public | orders          | table    | yugabyte
 public | orders_id_seq   | sequence | yugabyte
 public | products        | table    | yugabyte
 public | products_id_seq | sequence | yugabyte
 public | reviews         | table    | yugabyte
 public | reviews_id_seq  | sequence | yugabyte
 public | users           | table    | yugabyte
 public | users_id_seq    | sequence | yugabyte
(9 rows)
```

```postgresql
yb_demo=# SELECT source, 
            total_sales * 100.0 / (SELECT SUM(total_sales) FROM channel) AS percent_sales
          FROM channel
          WHERE source='Facebook';
```

```
  source  |  percent_sales
----------+------------------
 Facebook | 20.8927150492159
(1 row)
```

<!--
## 8. Secondary indexes

Coming soon.

## 9. JSONB column type

Coming soon.
-->

{{<tip title="Next step" >}}

[Build an application](../../build-apps/)

{{< /tip >}}
