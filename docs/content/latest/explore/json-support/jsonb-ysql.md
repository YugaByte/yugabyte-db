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
    identifier: explore-json-support-1-ysql
    parent: explore
    weight: 232
isTocNested: true
showAsideToc: true
---

JSON data types are for storing JSON (JavaScript Object Notation) data, as specified in [RFC 7159](https://tools.ietf.org/html/rfc7159). Such data can also be stored as `text`, but the JSON data types have the advantage of enforcing that each stored value is valid according to the JSON rules. There are also assorted JSON-specific functions and operators available for data stored in these data types.

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/json-support/jsonb-ysql/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/explore/json-support/jsonb-ycql/" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>


See the section [JSON data types and functionality](../../../api/ysql/datatypes/type_json/) for the YSQL reference documentation on the topic. 

{{< note title="Note" >}}
The JSON functionality in YSQL is nearly identical to the [JSON functionality in PostgreSQL](https://www.postgresql.org/docs/11/datatype-json.html). 
{{< /note >}}

There are two JSON data types supported in YSQL: `json` and `jsonb`.

* **The `jsonb` type** does not preserve white space, does not preserve the order of object keys, and does not keep duplicate object keys. If duplicate keys are specified in the input, only the last value is kept.

* **The `json` type** stores an exact copy of the input text, and therefore preserves semantically-insignificant white space between tokens, as well as the order of keys within JSON objects. Also, if a JSON object within the value contains the same key more than once, all the key/value pairs are kept. The processing functions consider the last value as the operative one.


{{< tip title="Tip" >}}
**When to use `jbonb` vs `json`?** In general, most applications should prefer to store JSON data as `jsonb`, unless there are quite specialized needs, such as legacy assumptions about ordering of object keys.

They accept *almost* identical sets of values as input. The major practical difference is one of efficiency:
* The `json` data type stores an exact copy of the input text, which processing functions must reparse on each execution
* The `jsonb` data is stored in a decomposed binary format that makes it slightly slower to input due to added conversion overhead, but significantly faster to process, since no reparsing is needed. `jsonb` also supports indexing, which can be a significant advantage.
{{< /tip >}}

This section will focus on only the `jsonb` type.

## 1. Prerequisites

You need a YugabyteDB cluster to run through the steps below. If do not have a YugabyteDB cluster, you can create one on your local machine as shown below.

```sh
$ ./bin/yb-ctl create
```

Connect to the cluster using `ysqlsh` to run through the examples below.

```sh
$ ./bin/ysqlsh
```

Next, create a simple table `books` which has a primary key and one `jsonb` column `doc` which contains various details about that book.

```plpgsql
create table books(k int primary key, doc jsonb not null);
```

Next, insert some rows which contain details about various books. These details are represented as JSON documents, as shown below.

```plpgsql
insert into books(k, doc) values
  (1,
  '{ "ISBN"    : 4582546494267,
     "title"   : "Macbeth", 
     "author"  : {"given_name": "William", "family_name": "Shakespeare"},
     "year"    : 1623}'),

  (2,
  '{ "ISBN"    : 8760835734528,
     "title"   : "Hamlet",
     "author"  : {"given_name": "William", "family_name": "Shakespeare"},
     "year"    : 1603,
     "editors" : ["Lysa", "Elizabeth"] }'),

  (3,
  '{ "ISBN"    : 7658956876542,
     "title"   : "Oliver Twist",
     "author"  : {"given_name": "Charles", "family_name": "Dickens"},
     "year"    : 1838,
     "genre"   : "novel",
     "editors" : ["Mark", "Tony", "Britney"] }'),
  (4,
  '{ "ISBN"    : 9874563896457,
     "title"   : "Great Expectations",
     "author"  : {"family_name": "Dickens"},
     "year"    : 1950,
     "genre"   : "novel",
     "editors" : ["Robert", "John", "Melisa", "Elizabeth"] }'),

  (5,
  '{ "ISBN"    : 8647295405123,
     "title"   : "A Brief History of Time",
     "author"  : {"given_name": "Stephen", "family_name": "Hawking"},
     "year"    : 1988,
     "genre"   : "science",
     "editors" : ["Melisa", "Mark", "John", "Fred", "Jane"] }'),

  (6,
  '{
    "ISBN"     : 6563973589123,
    "year"     : 1989,
    "genre"    : "novel",
    "title"    : "Joy Luck Club",
    "author"   : {"given_name": "Amy", "family_name": "Tan"},
    "editors"  : ["Ruilin", "Aiping"]}');
```

{{< note title="Note" >}}
Some of the rows in the example have some of the keys missing (intentional). But the row with "k=6" has every key. 
{{< /note >}}

## 2. Query JSON documents

List all the rows thus:

```plpgsql
select * from books;
```

This is the result:

```
yugabyte=# select * from books;
 k |                                                                                                         doc
---+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 5 | {"ISBN": 8647295405123, "year": 1988, "genre": "science", "title": "A Brief History of Time", "author": {"given_name": "Stephen", "family_name": "Hawking"}, "editors": ["Melisa", "Mark", "John", "Fred", "Jane"]}
 1 | {"ISBN": 4582546494267, "year": 1623, "title": "Macbeth", "author": {"given_name": "William", "family_name": "Shakespeare"}}
 6 | {"ISBN": 6563973589123, "year": 1989, "genre": "novel", "title": "Joy Luck Club", "author": {"given_name": "Amy", "family_name": "Tan"}, "editors": ["Ruilin", "Aiping"]}
 4 | {"ISBN": 9874563896457, "year": 1950, "genre": "novel", "title": "Great Expectations", "author": {"family_name": "Dickens"}, "editors": ["Robert", "John", "Melisa", "Elizabeth"]}
 2 | {"ISBN": 8760835734528, "year": 1603, "title": "Hamlet", "author": {"given_name": "William", "family_name": "Shakespeare"}, "editors": ["Lysa", "Elizabeth"]}
 3 | {"ISBN": 7658956876542, "year": 1838, "genre": "novel", "title": "Oliver Twist", "author": {"given_name": "Charles", "family_name": "Dickens"}, "editors": ["Mark", "Tony", "Britney"]}
(6 rows)
```

### Using `->` and `->>`

YSQL has two native operators, the `->` opertator (see [this section)](../../../api/ysql/datatypes/type_json/functions-operators/subvalue-operators/#the-160-160-160-160-operator)) and the `->>` operator (see [this section](../../../api/ysql/datatypes/type_json/functions-operators/subvalue-operators/#the-160-160-160-160-and-160-160-160-160-operators)), to query JSON documents. The first operator `->` returns a JSON object, while the operator `->>` returns text. These operators work on both `JSON` as well as `JSONB` columns to select a subset of attributes as well as to inspect the JSON document.


The example below shows how to select a few attributes from each document.

```plpgsql
SELECT doc->'title' AS book_title, 
       CONCAT(doc->'author'->'family_name', 
              ', ', doc->'author'->'given_name') AS author
    FROM books;
```
This is the result:
```
yugabyte=# SELECT doc->'title' AS book_title,
yugabyte-#        CONCAT(doc->'author'->'family_name',
yugabyte(#               ', ', doc->'author'->'given_name') AS author
yugabyte-#     FROM books;
        book_title         |          author
---------------------------+--------------------------
 "A Brief History of Time" | "Hawking", "Stephen"
 "Macbeth"                 | "Shakespeare", "William"
 "Joy Luck Club"           | "Tan", "Amy"
 "Great Expectations"      | "Dickens",
 "Hamlet"                  | "Shakespeare", "William"
 "Oliver Twist"            | "Dickens", "Charles"
(6 rows)
```

Because the -> operator returns an object, you can chain it to inspect deep into a JSON document, as shown below.

```plpgsql
select '{"title": "Macbeth", "author": {"given_name": "William"}}'::jsonb 
  -> 'author' -> 'given_name' as first_name;
```

This is the result:

```
yugabyte=# select '{"title": "Macbeth", "author": {"given_name": "William"}}'::jsonb
             -> 'author' -> 'given_name' as first_name;
 first_name
------------
 "William"
(1 row)
```

### Existence with `?`

The `?` operator (see [this section](../../../api/ysql/datatypes/type_json/functions-operators/key-or-value-existence-operators/#the-160-160-160-160-operator)) can be used to check if a JSON document contains a certain attribute. For example, if you want to find a count of the records where the `doc` column contains a property named *genre*,  run the following statement.

```plpgsql
SELECT doc->'title' AS book_title, 
       doc->'genre' as genre 
    FROM books WHERE doc ? 'genre';
```

This is the result:

```
yugabyte=# SELECT doc->'title' AS book_title,
yugabyte-#        doc->'genre' as genre
yugabyte-#     FROM books WHERE doc ? 'genre';
        book_title         |   genre
---------------------------+-----------
 "A Brief History of Time" | "science"
 "Joy Luck Club"           | "novel"
 "Great Expectations"      | "novel"
 "Oliver Twist"            | "novel"
(4 rows)
```

### Containment with `@>`

The containment operator `@>` (see [this section](../../../api/ysql/datatypes/type_json/functions-operators/containment-operators/)) tests whether one document contains another. If you want to find all books that contain the JSON value `{"author": {"given_name": "William"}}` (in other words, the author of the book has the given name *William*), do this:

```plpgsql
SELECT doc->'title' AS book_title, 
       CONCAT(doc->'author'->'family_name', 
              ', ', doc->'author'->'given_name') AS author
    FROM books 
    WHERE doc @> '{"author": {"given_name": "William"}}'::jsonb;
```

This is the result:

```
yugabyte=# SELECT doc->'title' AS book_title,
yugabyte-#        CONCAT(doc->'author'->'family_name',
yugabyte(#               ', ', doc->'author'->'given_name') AS author
yugabyte-#     FROM books
yugabyte-#     WHERE doc @> '{"author": {"given_name": "William"}}'::jsonb;
 book_title |          author
------------+--------------------------
 "Macbeth"  | "Shakespeare", "William"
 "Hamlet"   | "Shakespeare", "William"
(2 rows)
```


## 3. Update JSON documents

There are a number of ways to update a JSON document, as shown below.

### Add an attribute

Use the `||` operator (see [this section](../../../api/ysql/datatypes/type_json/functions-operators/concatenation-operator/)) to either update or insert the attribute into the existing JSON document. For example, if you want to add a `stock` attribute to all the books, do this:

```plpgsql
UPDATE books SET doc = doc || '{"stock": "true"}';
```

This is the result:

```
yugabyte=# SELECT doc->'title' AS title, doc->'stock' as stock FROM books;
           title           | stock
---------------------------+--------
 "A Brief History of Time" | "true"
 "Macbeth"                 | "true"
 "Joy Luck Club"           | "true"
 "Great Expectations"      | "true"
 "Hamlet"                  | "true"
 "Oliver Twist"            | "true"
(6 rows)
```

### Remove an attribute

Use the `-` opertaor (see [this section](../../../api/ysql/datatypes/type_json/functions-operators/remove-operators/#the-160-160-160-160-operator)) to remove an attribute:

```plpgsql
UPDATE books SET doc = doc - 'stock';
```

This will remove the field from all the documents, as shown below.

```
yugabyte=# SELECT doc->'title' AS title, doc->'stock' as stock FROM books;
           title           | stock
---------------------------+-------
 "A Brief History of Time" |
 "Macbeth"                 |
 "Joy Luck Club"           |
 "Great Expectations"      |
 "Hamlet"                  |
 "Oliver Twist"            |
(6 rows)
```

### Replace a document

To replace an entire document, run the following SQL statement.

```plpgsql
UPDATE books 
    SET doc = '{"ISBN": 4582546494267, "year": 1623, "title": "Macbeth", "author": {"given_name": "William", "family_name": "Shakespeare"}}' 
    WHERE k=1;
```

## 4. Built-in functions

YSQL supports a large number of operators and built-in functions that operate on JSON documents. This section highlights a few of these built-in functions.

{{< note title="Note" >}}
All of the built-in functions supported by PostgreSQL are supported in YSQL.

Check the reference documentation for a complete [list of JSON functions and operators](../../../api/ysql/datatypes/type_json/functions-operators/).
{{< /note >}}


### Expand JSON - `jsonb_each`

The [`jsonb_each()`](../../../api/ysql/datatypes/type_json/functions-operators/jsonb-each/) function expands the top-level JSON document into a set of key-value pairs, as shown below.


```plpgsql
SELECT jsonb_each(doc) FROM books WHERE k=1;
```

The output is shown below.

```
yugabyte=# SELECT jsonb_each(doc) FROM books WHERE k=1;
                                 jsonb_each
----------------------------------------------------------------------------
 (ISBN,4582546494267)
 (year,1623)
 (title,"""Macbeth""")
 (author,"{""given_name"": ""William"", ""family_name"": ""Shakespeare""}")
(4 rows)
```

### Retrieve keys - `jsonb_object_keys`

The [`jsonb_object_keys()`](../../../api/ysql/datatypes/type_json/functions-operators/jsonb-object-keys/) function retrieves the keys of the top-level JSON document thus:

```plpgsql
SELECT jsonb_object_keys(doc) FROM books WHERE k=1;
```

This is the result:

```
yugabyte=# SELECT jsonb_object_keys(doc) FROM books WHERE k=1;
 jsonb_object_keys
-------------------
 ISBN
 year
 title
 author
(4 rows)
```

### Format JSON - `jsonb_pretty`

When you select a `jsonb` (or `json`) value in `ysqlsh`, you see the terse `text` typecast of the value. The  [`jsonb_pretty()`](../../../api/ysql/datatypes/type_json/functions-operators/jsonb-pretty/) function returns a more human-readable format:

```plpgsql
SELECT jsonb_pretty(doc) FROM books WHERE k=1;
```

This is the result:

```
yugabyte=# SELECT jsonb_pretty(doc) FROM books WHERE k=1;
             jsonb_pretty
--------------------------------------
 {                                   +
     "ISBN": 4582546494267,          +
     "year": 1623,                   +
     "title": "Macbeth",             +
     "author": {                     +
         "given_name": "William",    +
         "family_name": "Shakespeare"+
     }                               +
 }
(1 row)
```

## 5. Constraints

See the [Create indexes and check constraints on JSON columns](../../../api/ysql/datatypes/type_json/create-indexes-check-constraints/) section.

You can create constraint on `jsonb` data types. Here are a couple of examples.

### Check JSON documents are objects

Here's how to insist that each JSON document is an object:

```plpgsql
alter table books 
add constraint books_doc_is_object
check (jsonb_typeof(doc) = 'object');
```

### Check ISBN is a 13-digit number

Here's how to insist that the ISBN is always defined and is a positive 13-digit number:

```plpgsql
alter table books 
add constraint books_isbn_is_positive_13_digit_number 
check (
  (doc->'ISBN') is not null
    and
  jsonb_typeof(doc->'ISBN') = 'number'
     and
  (doc->>'ISBN')::bigint > 0
    and
  length(((doc->>'ISBN')::bigint)::text) = 13
);
```


## 6. Indexes on JSON attributes

Indexes are essential to perform efficient lookups by document attributes. Without indexes, queries on document attributes end up performing a full table scan and process each JSON document. This section outlines some of the indexes supported.


### Secondary index

If you want to support range queries that reference the value for the *year* attribute, do this:

```plpgsql
CREATE INDEX books_year 
    ON books ((doc->>'year') ASC)
    WHERE doc->>'year' is not null;
```

This will make the following query efficient:

```plpgsql
select
  (doc->>'ISBN')::bigint as year,
  doc->>'title'          as title,
  (doc->>'year')::int    as year
from books
where (doc->>'year')::int > 1850
order by 3;
```

### Partial and expression indexes

You might want to index only those documents that contain the attribute (as opposed to indexing the rows that have a `NULL` value for that attribute). This is a common scenario because not all the documents would have all the attributes defined. This can be achieved using a *partial index*.

In the previous section where you created a secondary index, not all the books may have the `year` attribute defined. Suppose that you want to  index only those documents that have a `NOT NULL` `year` attribute create the following partial index:

```plpgsql
CREATE INDEX books_year 
    ON books ((doc->>'year') ASC)
    WHERE doc->>'year' IS NOT NULL;
```

### Unique index

You can create a unique index on the "ISBN" key for the books table as shown below.

```plpgsql
create unique index books_isbn_unq on books((doc->>'ISBN'));
```

Inserting a row with a duplicate value would fail as shown below. The book has a new primary key `k` but an existing ISBN, `4582546494267`.

```plpgsql
yugabyte=# insert into books values 
           (7, '{  "ISBN"    : 4582546494267, 
                   "title"   : "Fake Book with duplicate ISBN" }');
ERROR:  23505: duplicate key value violates unique constraint "books_isbn_unq"
```


## 7. Clean up (optional)

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./bin/yb-ctl destroy
```
