---
title: E-commerce sample application
headerTitle: E-Commerce app
linkTitle: E-Commerce app
description: Run an e-commerce sample application built on YugabyteDB.
block_indexing: true
menu:
  v2.1:
    identifier: ecommerce-app
    parent: realworld-apps
    weight: 582
isTocNested: true
showAsideToc: true
---

## Overview

Yugastore is a sample, full-stack online bookstore, or more generally, an e-commerce app built on top of YugabyteDB. It is a cloud native, distributed app built on a microservices architecture. YugabyteDB simplifies the development of such apps by providing a SQL-like API (based on Cassandra Query Language) and a Redis-API on top of a common database. The app can be deployed and run on VMs or using StatefulSets in Kubernetes.

You can browse the [Yugastore source-code on GitHub](https://github.com/yugabyte/yugastore). It is fully open-source.

![Yugastore app screenshot](/images/develop/realworld-apps/ecommerce-app/yugastore-app-screenshots.png)

## Features

### Product catalog management

A product catalog is a database of information about all the products in the online store. Operations such as inserting new products, updating existing products and listing all the products in the online store should be efficiently supported.

Each product could have a lot of associated attributes, such as title, pricing, number of reviews, etc. It should be possible to add static and dynamic attributes for each product. Static attributes do not change very often, for example the title and description of a book. Dynamic attributes change frequently, for example the user rating for the product or number of reviews it has received.

### Product catalog display

The product catalog display is the “web” layer which displays different views of the product catalog to end users.

### Other features coming soon

The following features will be added as the app evolves over time:

- shopping cart
- order history tracking
- pageview tracking
- user personalization

## Architecture

This app is built using the following stack:

- Frontend: [React](https://reactjs.org/)
- Backend: [Express](https://expressjs.com/) and [NodeJS](https://nodejs.org/en/)
- Database: [YugabyteDB](https://www.yugabyte.com/)

![YERN stack](/images/develop/realworld-apps/ecommerce-app/yugabyte-express-react-nodejs.png)

All the source for the various components can be found in the following locations:

- React UI
  - You can see all the React routes in [index.js](https://github.com/yugabyte/yugastore/blob/master/ui/src/index.js).
  - You can find the components in the [ui/src/components](https://github.com/yugabyte/yugastore/tree/master/ui/src/components) subdirectory.

- Express / NodeJS web framework
  - You can find all the REST API routes for products in [routes/product.js](https://github.com/yugabyte/yugastore/blob/master/routes/products.js).
  - You can find the web framework setup in [app.js](https://github.com/yugabyte/yugastore/blob/master/app.js).

- Models
  - The sample data is present in [models/products.json](https://github.com/yugabyte/yugastore/blob/master/models/sample_data.json)
  - The data loader is in the file [models/yugabyte/db_init.js](https://github.com/yugabyte/yugastore/blob/master/models/yugabyte/db_init.js).

The sections below describe the architecture / data model for the various features in the app.

### Product catalog management

The inventory of products is modeled as a table using the Cassandra-compatible YCQL API. Each product has a unique `id` which is an integer in this example. The product `id` is the [primary key partition column](../../learn/data-modeling/#partition-key-columns-required). This ensures that all the data for one product (identified by its product `id`) is colocated in the database.

```sql
ycqlsh> DESCRIBE TABLE yugastore.products;

CREATE TABLE yugastore.products (
    id int PRIMARY KEY,
    name text,
    description text,
    price double,
    author text,
    type text,
    img text,
    category text,
    num_reviews int,
    total_stars int
);
```

The dynamic attributes for rendering sorted views (such as *highly rated* or *most reviewed*) are stored in Redis sorted sets.

```sql
127.0.0.1:6379> ZADD allproducts:num_stars <num-stars> <product-id>
```

The sample list of products are in the [`models/sample_data.json`](https://github.com/yugabyte/yugastore/blob/master/models/sample_data.json) file in the codebase. The file has entries such as the following:

```json
{
  "products": [
    {
      "id": 1,
      "name": "The Power of HABIT",
      "author": "Charles Duhigg",
      "type": "hardcover",
      ...
      "category": "business"
    },
    {
      "id": 2,
      "name": "Think and Grow Rich",
      "author": "Napoleon Hill",
      "type": "hardcover",
      ...
      "category": "business"
    },
    ...
```

The [`db_init.js`](https://github.com/yugabyte/yugastore/blob/master/models/yugabyte/db_init.js) node script loads the static attributes of the sample data using the following Cassandra batch insert API into YugabyteDB.

```js
insert_batch.push({
  query: insert,
  params: params
});
```

The dynamic attributes are loaded using the Redis-compatible YEDIS API into YugabyteDB.

```
ybRedisClient.zadd("allproducts:num_reviews", e.num_reviews, e.id);
ybRedisClient.zadd("allproducts:num_stars", e.num_stars, e.id);
ybRedisClient.zadd("allproducts:num_buys", numBuys, e.id);
ybRedisClient.zadd("allproducts:num_views", numViews, e.id);
```

### Product catalog display

Here you will examine the various display components of the product catalog in detail.

#### Homepage

The homepage is rendered by the `App` react component. The React route is the following:

```js
<Route exact path="/" component={App} />
```

It uses the following REST API to query Express/NodeJS for all the products:

```
/products
```

Internally, the following query is executed against the database and the results are rendered.

```sql
SELECT * FROM yugastore.products;
```

#### Top-level category pages

List products that have a given value for the category attribute. This is used to compose views of products grouped by the common categories such as "business books", "mystery books", etc.  The links to these pages are displayed on the homepage. The screenshot below shows the links that are the top-level category pages. They form the top navigation in the web app.

![View by categories](/images/develop/realworld-apps/ecommerce-app/yugastore-by-categories.png)

Take the example of the *business* category page.

This is rendered by the `Products` react component. Here is the react route:

```js
<Route path="/business"
  render={(props) => (
    <Products
      name={"Business Books"}
      query={"category/business"} /> 
  )} />
```

The component internally uses the following REST API:

```
/products/catgory/business
```

The following query is executed against the database:

```sql
SELECT * FROM yugastore.products WHERE category='business';
```

#### Sorted list views based on dynamic attributes

List products in the descending (or ascending) order of certain frequently updated attributes. Used to build the top navigation bar with entries such as "most reviewed", "highest rated", etc.

![View by dynamic attributes](/images/develop/realworld-apps/ecommerce-app/yugastore-by-dynamic-attrs.png)

Let us take the example of books with the *highest rating* as an example.

These product lists are also rendered by the `Products` React component.

```js
<Route path="/sort/num_stars"
  render={(props) => (
    <Products
      name={"Books with the Highest Rating"}
      query={"sort/num_stars"} /> 
  )} />
```

The component internally uses the following REST API:

```
/products/sort/num_stars
```

The top 10 product IDs sorted in a descending order by their rating is fetched from Redis with the following:

```js
ybRedis.zrevrange("allproducts:num_stars", 0, 10, 'withscores', ...)
```

Then, a `SELECT` is issued against each of those product IDs with the following `IN` query:

```sql
SELECT * FROM yugastore.products WHERE id IN ?;
```

#### Product details view

This view shows all the details of a product. An example product details page for item with `id=5` is shown below:

![Product details](/images/develop/realworld-apps/ecommerce-app/yugastore-product-details.png)

The React route for this view is `ShowProduct`:

```js
<Route exact path="/item/:id" component={ShowProduct} />
```

The component internally uses the following REST API:

```
/products/details/5
```

The following query is executed against the database to fetch all the product details:

```sql
SELECT * FROM yugastore.products WHERE id=5;
```

If you had another table with extended product information, you could fetch data from that table as well and add it into the result. Finally, each time this page is hit, increment a counter in order to track how many times the current product was viewed.

```
ybRedis.incrby("pageviews:product:5:count", 1);
```

## Summary

This application is a blueprint for building eCommerce and other similar web applications. The instructions to build and run the application, as well as the source code, can be found in the [Yugastore GitHub repository](https://github.com/yugabyte/yugastore).
