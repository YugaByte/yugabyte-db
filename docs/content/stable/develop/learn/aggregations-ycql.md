---
title: Aggregations in YCQL
headerTitle: Aggregations
linkTitle: 5. Aggregations
description: Learn how YugabyteDB YCQL supports standard aggregation functions.
block_indexing: true
menu:
  stable:
    identifier: aggregations-1-ycql
    parent: learn
    weight: 567
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/develop/learn/aggregations-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/stable/develop/learn/aggregations-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

YugabyteDB supports a number of standard aggregation functions. Let us go through some of these using an example. Consider a products table as shown below.

| ProductID | ProductName          | SupplierID | CategoryID | Unit | Price | Quantity
| ---       | ---                  | ---        | ---        | ---  | --- | ---
| 1 | Chais                        | 1 | 1 | 10 boxes x 20 bags  | 18    | 25
| 2 | Chang                        | 1 | 1 | 24 - 12 oz bottles  | 19    | 12
| 3 | Aniseed Syrup                | 1 | 2 | 12 - 550 ml bottles | 10    | 10
| 4 | Chef Anton's Cajun Seasoning | 2 | 2 | 48 - 6 oz jars      | 22    | 9
| 5 | Chef Anton's Gumbo Mix       | 2 | 2 | 36 boxes            | 21.35 | 40

Let us create this table with `ProductID` as the primary hash key.

```sql
ycqlsh> CREATE KEYSPACE store;
```

```sql
ycqlsh> CREATE TABLE store.products (ProductID BIGINT PRIMARY KEY, ProductName VARCHAR, SupplierID INT, CategoryID INT, Unit TEXT, Price FLOAT, Quantity INT);
```

Now let us populate the sample data.

```sql
INSERT INTO store.products (ProductID, ProductName, SupplierID, CategoryID, Unit, Price, Quantity) VALUES (1, 'Chais', 1, 1, '10 boxes x 20 bags', 18, 25);
INSERT INTO store.products (ProductID, ProductName, SupplierID, CategoryID, Unit, Price, Quantity) VALUES (2, 'Chang', 1, 1, '24 - 12 oz bottles', 19, 12);
INSERT INTO store.products (ProductID, ProductName, SupplierID, CategoryID, Unit, Price, Quantity) VALUES (3, 'Aniseed Syrup', 1, 2, '12 - 550 ml bottles', 10, 10);
INSERT INTO store.products (ProductID, ProductName, SupplierID, CategoryID, Unit, Price, Quantity) VALUES (4, 'Chef Anton''s Cajun Seasoning', 2, 2, '48 - 6 oz jars', 22, 9);
INSERT INTO store.products (ProductID, ProductName, SupplierID, CategoryID, Unit, Price, Quantity) VALUES (5, 'Chef Anton''s Gumbo Mix', 2, 2, '36 boxes', 21.35, 40);
```

## Counts

- Finding the number of item types in the store can be done as follows.

```sql
ycqlsh> SELECT COUNT(ProductID) FROM store.products;
```

```
 count(productid)
------------------
                5

(1 rows)
```

- We can give an alias name to the count column as follows.

```sql
ycqlsh> SELECT COUNT(ProductID) as num_products FROM store.products;
```

```
 num_products
--------------
            5

(1 rows)
```

- Finding the number of item types for supplier 1 can be done as follows.

You can do this as shown below.

```sql
ycqlsh> SELECT COUNT(ProductID) as supplier1_num_products FROM store.products WHERE SupplierID=1;
```

```
 supplier1_num_products
------------------------
                      3

(1 rows)
```

## Numeric aggregation functions

The standard aggregate functions of `min`, `max`, `sum`, `avg` and `count` are built-in functions.

- To find the total number of items in the store, run the following query.

```sql
ycqlsh> SELECT SUM(Quantity) FROM store.products;
```

```
 sum(quantity)
---------------
            96

(1 rows)
```

- To find the price of the cheapest and the most expensive item, run the following.

```sql
ycqlsh> SELECT MIN(Price), MAX(Price) FROM store.products;
```

```
 min(price) | max(price)
------------+------------
         10 |         22

(1 rows)
```

- To find the average price of all the items in the store, run the following.

```sql
ycqlsh> SELECT AVG(price) FROM store.products;
```

```
 system.avg(price)
-------------------
             18.07

(1 rows)
```
