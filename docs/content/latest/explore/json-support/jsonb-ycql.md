---
title: JSON Support
headerTitle: JSON Support
linkTitle: JSON Support
description: JSON Support in YugabyteDB.
headcontent: JSON Support in YugabyteDB.
image: <div class="icon"><i class="fas fa-file-invoice"></i></div>
menu:
  latest:
    name: JSON Support
    identifier: explore-json-support-2-ycql
    parent: explore
    weight: 232
isTocNested: true
showAsideToc: true
---

JSON data types are for storing JSON (JavaScript Object Notation) data, as specified in [RFC 7159](https://tools.ietf.org/html/rfc7159). Such data can also be stored as `text`, but the JSON data types have the advantage of enforcing that each stored value is valid according to the JSON rules. There are also assorted JSON-specific functions and operators available for data stored in these data types.

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/json-support/jsonb-ysql/" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/explore/json-support/jsonb-ycql/" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>


The JSON data type supported in YCQL is `jsonb`. The JSON functionality in YCQL is **a subset** of the [JSON functionality in PostgreSQL](https://www.postgresql.org/docs/11/datatype-json.html).

## 1. Prerequisites

You need a YugabyteDB cluster to run through the steps below. If do not have a YugabyteDB cluster, you can create one on your local machine as shown below.

```sh
$ ./bin/yb-ctl create
```

Connect to the cluster using `ysqlsh` to run through the examples below.

```sh
$ ./bin/ycqlsh
```

Create table with a JSONB column

```sql
ycqlsh> CREATE KEYSPACE store;
```

```sql
ycqlsh> CREATE TABLE store.books ( id int PRIMARY KEY, details jsonb );
```

Insert JSONB documents

```sql
INSERT INTO store.books (id, details) VALUES
  (1, '{ "name": "Macbeth", "author": { "first_name": "William", "last_name": "Shakespeare" }, "year": 1623, "editors": ["John", "Elizabeth", "Jeff"] }');
INSERT INTO store.books (id, details) VALUES 
  (2, '{ "name": "Hamlet", "author": { "first_name": "William", "last_name": "Shakespeare" }, "year": 1603, "editors": ["Lysa", "Mark", "Robert"] }');
INSERT INTO store.books (id, details) VALUES 
  (3, '{ "name": "Oliver Twist", "author": { "first_name": "Charles", "last_name": "Dickens" }, "year": 1838, "genre": "novel", "editors": ["Mark", "Tony", "Britney"] }');
INSERT INTO store.books (id, details) VALUES 
  (4, '{ "name": "Great Expectations", "author": { "first_name": "Charles", "last_name": "Dickens" }, "year": 1950, "genre": "novel", "editors": ["Robert", "John", "Melisa"] }');
INSERT INTO store.books (id, details) VALUES 
  (5, '{ "name": "A Brief History of Time", "author": { "first_name": "Stephen", "last_name": "Hawking" }, "year": 1988, "genre": "science", "editors": ["Melisa", "Mark", "John"] }');
```

## 2. Query JSON documents

Let us list all the rows we inserted using the command below.

```sql
ycqlsh> SELECT * FROM store.books;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------------------
  5 | {"author":{"first_name":"Stephen","last_name":"Hawking"},"editors":["Melisa","Mark","John"],"genre":"science","name":"A Brief History of Time","year":1988}
  1 |                            {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["John","Elizabeth","Jeff"],"name":"Macbeth","year":1623}
  4 |      {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Robert","John","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
  2 |                                {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["Lysa","Mark","Robert"],"name":"Hamlet","year":1603}
  3 |             {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Mark","Tony","Britney"],"genre":"novel","name":"Oliver Twist","year":1838}
```

### Using `->` and `->>`

- Select with condition on JSONB object value

```sql
ycqlsh> SELECT * FROM store.books WHERE details->'author'->>'first_name' = 'William' AND details->'author'->>'last_name' = 'Shakespeare';
```

```
 id | details
----+----------------------------------------------------------------------------------------------------------------------------------
  1 | {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["John","Elizabeth","Jeff"],"name":"Macbeth","year":1623}
  2 |     {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["Lysa","Mark","Robert"],"name":"Hamlet","year":1603}
```

- Select with condition on JSONB array element

```sql
ycqlsh> SELECT * FROM store.books WHERE details->'editors'->>0 = 'Mark';
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  3 | {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Mark","Tony","Britney"],"genre":"novel","name":"Oliver Twist","year":1838}
```

- Select with condition using on JSONB element

```sql
ycqlsh> SELECT * FROM store.books WHERE CAST(details->>'year' AS integer) = 1950;
```

```
 id | details
----+--------------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Robert","John","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```


## 3. Update JSON documents

There are a number of ways to update a JSON document, as shown below.

### Update entire document

You can do this as shown below.

```sql
ycqlsh> UPDATE store.books SET details = '{"author":{"first_name":"Carl","last_name":"Sagan"},"editors":["Ann","Rob","Neil"],"genre":"science","name":"Cosmos","year":1980}' WHERE id = 1;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 1;
```

```
 id | details
----+-----------------------------------------------------------------------------------------------------------------------------------
  1 | {"author":{"first_name":"Carl","last_name":"Sagan"},"editors":["Ann","Rob","Neil"],"genre":"science","name":"Cosmos","year":1980}
```

### Update an attribute

```sql
ycqlsh> UPDATE store.books SET details->'author'->>'first_name' = '"Steve"' WHERE id = 4;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 4;
```

```
 id | details
----+------------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"Steve","last_name":"Dickens"},"editors":["Robert","John","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

### Update an array element

```sql
ycqlsh> UPDATE store.books SET details->'editors'->>1 = '"Jack"' WHERE id = 4;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 4;
```

```
 id | details
----+------------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"Steve","last_name":"Dickens"},"editors":["Robert","Jack","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

- Update a subdocument.

```sql
ycqlsh> UPDATE store.books SET details->'author' = '{"first_name":"John", "last_name":"Doe"}' WHERE id = 4;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 4;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"John","last_name":"Doe"},"editors":["Robert","Jack","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

## 4. Upserts

### Add attributes

Update a missing JSONB document resulting in an insert.

```sql
INSERT INTO store.books (id, details) VALUES
  (6, '{}');
ycqlsh> UPDATE store.books SET details->'editors' = '["Adam", "Bryan", "Charles"]' WHERE id = 6;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 6;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  6 | {"editors":["Adam","Bryan","Charles"]}
```

### Add subdocuments

- Update a missing JSONB document resulting in an insert of a subdocument.

```sql
ycqlsh> UPDATE store.books SET details->'author' = '{"first_name":"Jack", "last_name":"Kerouac"}' WHERE id = 6;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 6;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  6 | {"author":{"first_name":"Jack","last_name":"Kerouac"},"editors":["Adam","Bryan","Charles"]}
```

{{< note title="Note" >}}
JSONB upsert only works for JSON objects and not for other data types like arrays, integers, strings, etc. Additionally, only the leaf property of an object will be inserted if it is missing. We do not support upsert on non-leaf properties.
{{< /note >}}


