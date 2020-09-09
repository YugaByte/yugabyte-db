---
title: Data types in YCQL
headerTitle: Data types
linkTitle: 3. Data types
description: Learn about the data types in YCQL.
block_indexing: true
menu:
  stable:
    identifier: data-types-1-ycql
    parent: learn
    weight: 565
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/develop/learn/data-types-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/stable/develop/learn/data-types-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

This topic lists the various data types available in YugabyteDB’s [Cassandra-compatible YCQL API](../../../api/ycql).

## JSONB

There are a number of different serialization formats for JSON data, one of the popular formats being JSONB to efficiently model document data. And just in case you were wondering, JSONB stands for JSON Better.

The YCQL API supports the [JSONB data type](../../../api/ycql/type_jsonb/) to parse, store and query JSON documents natively. This data type is similar in query language syntax and functionality to the one supported by PostgreSQL. JSONB serialization allows for easy search and retrieval of attributes inside the document. This is achieved by storing all the JSON attributes in a sorted order, which allows for efficient binary search of keys. Similarly arrays are stored such that random access for a particular array index into the serialized json document is possible. [DocDB](../../../architecture/concepts/persistence/), YugabyteDB’s underlying storage engine, is document-oriented in itself which makes storing the data of the JSON data type lot more simple than otherwise possible.

Let us take the example of an ecommerce app of an online bookstore. The database for such a bookstore needs to store details of various books, some of which may have custom attributes. Below is an example of a JSON document that captures the details of a particular book, Macbeth written by William Shakespeare.

```json
{
   "name":"Macbeth",
   "author":{
      "first_name":"William",
      "last_name":"Shakespeare"
   },
   "year":1623,
   "editors":[
      "John",
      "Elizabeth",
      "Jeff"
   ]
}
```

### Create a table

The books table for this bookstore can be modelled as shown below. We assume that the id of each book is an int, but this could be a string or a uuid.

```sql
ycqlsh> CREATE KEYSPACE store;
```

```sql
ycqlsh> CREATE TABLE store.books ( id int PRIMARY KEY, details jsonb );
```

### Insert data

Next we insert some sample data for a few books into this store. You can copy and paste the following commands into the YCQL shell (`ycqlsh`) for YugabyteDB to insert the data.

```sql
INSERT INTO store.books (id, details) VALUES (1, 
  '{ "name"   : "Macbeth", 
     "author" : {"first_name": "William", "last_name": "Shakespeare"},
     "year"   : 1623,
     "editors": ["John", "Elizabeth", "Jeff"] }'
);
INSERT INTO store.books (id, details) VALUES (2,
  '{ "name"   : "Hamlet",
     "author" : {"first_name": "William", "last_name": "Shakespeare"},
     "year"   : 1603,
     "editors": ["Lysa", "Mark", "Robert"] }'
);
INSERT INTO store.books (id, details) VALUES (3,
  '{ "name"   : "Oliver Twist",
     "author" : {"first_name": "Charles", "last_name": "Dickens"},
     "year"   : 1838,
     "genre"  : "novel",
     "editors": ["Mark", "Tony", "Britney"] }'
);
INSERT INTO store.books (id, details) VALUES (4,
  '{ "name"   : "Great Expectations",
     "author" : {"first_name": "Charles", "last_name": "Dickens"},
     "year"   : 1950,
     "genre"  : "novel",
     "editors": ["Robert", "John", "Melisa"] }'
);
INSERT INTO store.books (id, details) VALUES (5,
  '{ "name"   : "A Brief History of Time",
     "author" : {"first_name": "Stephen", "last_name": "Hawking"},
     "year"   : 1988,
     "genre"  : "science",
     "editors": ["Melisa", "Mark", "John"] }'
);
```

Note the following interesting points about the book details above:

- The year attribute for each of the books is interpreted as an integer.
- The first two books do not have a genre attribute, which the others do.
- The author attribute is a map.
- The editors attribute is an array.

### Retrieve a subset of attributes

Running the following default `SELECT` query will return all attributes of each book.

```sql
ycqlsh> SELECT * FROM store.books;

```

But a number of times you might want to query just a subset of attributes from YugabyteDB database. Below is an example of a query that retrieves just the id and name for all the books.

```sql
ycqlsh> SELECT id, details->>'name' as book_title FROM store.books;
```

```
 id | book_title
----+-------------------------
  5 | A Brief History of Time
  1 |                 Macbeth
  4 |      Great Expectations
  2 |                  Hamlet
  3 |            Oliver Twist
```

### Query by attribute values - string

The name attribute is a string in the book details JSON document. Run the following to query the details of the book named *Hamlet*.

```sql
ycqlsh> SELECT * FROM store.books WHERE details->>'name'='Hamlet';
```

```
 id | details
----+---------------------------------------------------------------
  2 | {"author":{"first_name":"William","last_name":"Shakespeare"},
       "editors":["Lysa","Mark","Robert"],
       "name":"Hamlet","year":1603}
```

Note that you can query by attributes that exist only in some of the documents. For example, you can query for all books that have a genre of novel. Recall from before that all books do not have a genre attribute defined.

```sql
ycqlsh> SELECT id, details->>'name' as title,
              details->>'genre' as genre
         FROM store.books
         WHERE details->>'genre'='novel';
```

```
 id | title              | genre
----+--------------------+-------
  4 | Great Expectations | novel
  3 |       Oliver Twist | novel
```

### Query by attribute values - integer

The year attribute is an integer in the book details JSON document. Run the following to query the ID and name of books written after 1900.

```sql
ycqlsh> SELECT id, details->>'name' as title, details->>'year'
         FROM store.books
         WHERE CAST(details->>'year' AS integer) > 1900;
```

```
 id | title                   | expr
----+-------------------------+------
  5 | A Brief History of Time | 1988
  4 |      Great Expectations | 1950
```

### Query by attribute values - map

The author attribute is a map, which in turn consists of the attributes `first_name` and `last_name`. Let us fetch the IDs and titles of all books written by the author William Shakespeare.

```sql
ycqlsh> SELECT id, details->>'name' as title,
              details->>'author' as author
         FROM store.books
         WHERE details->'author'->>'first_name' = 'William' AND
               details->'author'->>'last_name' = 'Shakespeare';
```

```
 id | title   | author
----+---------+----------------------------------------------------
  1 | Macbeth | {"first_name":"William","last_name":"Shakespeare"}
  2 |  Hamlet | {"first_name":"William","last_name":"Shakespeare"}
```

### Query by attribute Values - array

The editors attribute is an array consisting of the first names of the editors of each of the books. You can query for the book titles where `Mark` is the first entry in the editors list as follows.

```sql
ycqlsh> SELECT id, details->>'name' as title,
              details->>'editors' as editors FROM store.books
         WHERE details->'editors'->>0 = 'Mark';
```

```
 id | title        | editors
----+--------------+---------------------------
  3 | Oliver Twist | ["Mark","Tony","Britney"]
```
